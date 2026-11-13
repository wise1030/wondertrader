# CloseoutExecutor — 收盘对冲渐进式执行策略设计

## 1. 问题背景

### 1.1 现状

当前 `executeCloseoutHedge()` (UftFutuMmStrategy.cpp:1478-1608) 的执行逻辑：

```
totalDelta = getNetDelta()
hedgeQty   = -totalDelta / hedgeRatio
executePrice = isBuy ? askPrice : bidPrice   // 对手价
// → 单笔 submitBuy/submitSell, flag=0 (普通限价), 全量一次发出
```

**问题：**
1. 单笔全量对手价 = 最大市场冲击成本（滑点）
2. flag=0 普通限价 → 部分成交后残余挂单，过期不撤则收盘后仍挂
3. 无 urgency 感知：距离收盘 30s 和 5min 走同样路径
4. 无迭代：发一单完事，不管成交多少
5. 已知 bug 链（L0.5 双账本异常）虽然根因在 UftMocker，但执行层缺乏防御

### 1.2 目标

在收盘前有限时间窗口内：
- **flatten pos**：消除全部净敞口
- **降低对冲成本**：避免单笔市价冲击，用渐进式执行
- **确定性保证**：时间耗尽前必须完成，最后兜底走 sweep

## 2. 整体架构

### 2.1 五阶段流水线

```
┌─────────────────────────────────────────────────────────┐
│                    on_tick 触发条件                       │
│   minutes_before 到期 || risk breach → closeout trigger  │
└──────────────┬──────────────────────────────────────────┘
               │
     ┌─────────▼─────────┐
     │  Phase 0: HALT    │  停止报价 (_trading_state.halt + pauseQuoting)
     │  ~0ms (同步)       │  禁止新报价产生
     └─────────┬─────────┘
               │
     ┌─────────▼─────────┐
     │  Phase 1: DRAIN   │  撤销所有 inflight 订单 (MM cancelAll + OrderRouter cancelAllBySource)
     │  0~N ticks         │  drain 判据: UnifiedOrderTracker.getOrderCount() == 0
     │                    │  超时兜底: max 3000ms → 强制进入 Phase 2
     └─────────┬─────────┘
               │
     ┌─────────▼─────────┐
     │  Phase 2: ASSESS  │  读取净敞口
     │  ~0ms (同步)       │  net_delta = _portfolio->getNetDelta()
     │                    │  remaining = -net_delta / hedge_ratio
     │                    │  如果 |remaining| < 0.01 → 直接 COMPLETED
     └─────────┬─────────┘
               │
     ┌─────────▼─────────┐
     │  Phase 3: EXECUTE │  urgency 驱动渐进式对冲 (核心)
     │  迭代执行           │  每轮: 算urgency → 选价格档 → 发FAK → 等成交 → 重算remaining
     │  直到 remaining≈0  │  时间耗尽 → 最后单 SWEEP 兜底
     │  或 时间耗尽        │
     └─────────┬─────────┘
               │
     ┌─────────▼─────────┐
     │  Phase 4: VERIFY  │  确认 remaining ≈ 0
     │  ~0ms (同步)       │  ✓ → COMPLETED
     │                    │  ✗ → log warning + RETRYING (下一轮重试)
     └───────────────────┘
```

### 2.2 与现有代码的对接

```
UftFutuMmStrategy::on_tick
  └→ coordinator->processTick()
       └→ processCloseout()
            └→ triggerCloseout() → FutuRiskMonitor: IDLE→TRIGGERED→FLATTENING
                 └→ [现有] _closeout_hedge_pending = true; wait N ticks
                      └→ [改造] executeCloseoutHedge(ctx)
                           └→ CloseoutExecutor::run(ctx)   ← 新组件接管
```

**关键改造点：** `executeCloseoutHedge()` 从「单笔全量」改为「调用 CloseoutExecutor 渐进执行」。CloseoutExecutor 在 on_tick 中被反复调用（每 tick 跑一轮），直到 remaining≈0 或超时。

## 3. 核心设计：urgency 驱动三级定价

### 3.1 urgency 模型

```
urgency = remaining_qty / (time_remaining_ms × avg_fill_rate)
```

- `remaining_qty`: 当前轮剩余手数（每轮重算）
- `time_remaining_ms`: 距离收盘的毫秒数
- `avg_fill_rate`: 过往几轮的实际成交速率（手/ms），初始估计 = remaining / planned_ms

urgency 的物理含义：**如果按当前价格档位被动挂单，能否在剩余时间内成交完**

| urgency 区间 | 定价策略 | 价格 | flag | 说明 |
|---|---|---|---|---|
| < 0.5 | PASSIVE | 盘口同侧 +1 tick | FAK (1) | 有充足时间，被动等成交 |
| 0.5 ~ 1.0 | MID_PASSIVE | mid 价 | FAK (1) | 时间尚可，中间价尝试 |
| 1.0 ~ 1.5 | AGGRESSIVE | 盘口对手价 | FAK (1) | 时间偏紧，吃对手价 |
| 1.5 ~ 2.0 | VERY_AGGRESSIVE | 对手价 +1 tick | FAK (1) | 时间紧，越过对手价 |
| > 2.0 | SWEEP | 对手价 +N tick | FAK (1) | 时间耗尽，扫全量 (N=2~3, 可配) |

### 3.2 分批逻辑

每轮不发出全量，而是发 `batch_qty`：

```
batch_qty = min(
    remaining,
    max_batch_limit,           // 单轮最大手数（配置，默认=持仓总量）
    depth_aware_limit           // 盘口深度感知: bid1_qty/ask1_qty × depth_ratio
)

depth_aware_limit = best_side_depth × depth_ratio
// depth_ratio: PASSIVE=0.3, MID=0.5, AGGRESSIVE=0.8, SWEEP=∞
```

### 3.3 执行循环伪代码

```cpp
void CloseoutExecutor::onTick(ctx) {
    if (_phase == IDLE) return;

    // Phase 1: drain
    if (_phase == DRAINING) {
        if (_tracker->getOrderCount() == 0 || elapsed > _drain_timeout_ms) {
            _phase = EXECUTING;
        }
        return;  // 等下一 tick
    }

    // Phase 2: assess
    if (_phase == ASSESSING) {
        double net_delta = _portfolio->getNetDelta();
        _remaining = -net_delta / _hedge_ratio;
        _total_to_hedge = std::abs(_remaining);
        if (std::abs(_remaining) < 0.01) {
            complete(ctx);
            return;
        }
        _phase = EXECUTING;
        _exec_start_ts = now_ms;
    }

    // Phase 3: execute (每 tick 跑一轮)
    if (_phase == EXECUTING) {
        // 重算 remaining（上一轮可能成交了一部分）
        double net_delta = _portfolio->getNetDelta();
        _remaining = -net_delta / _hedge_ratio;

        if (std::abs(_remaining) < 0.5) {  // 允许半手误差
            complete(ctx);
            return;
        }

        // 算 urgency
        uint32_t time_left_ms = _close_time_ms - now_ms;
        double fill_rate = estimateFillRate();
        double urgency = std::abs(_remaining) / (time_left_ms * fill_rate + 1e-6);

        // 选价格档
        PriceTier tier = selectTier(urgency, time_left_ms);

        // 分批数量
        double batch_qty = calcBatchQty(_remaining, tier);

        // 下单 (FAK)
        bool is_buy = (_remaining > 0);  // 正=需要买入
        double price = computePrice(tier, is_buy);
        auto res = _order_router->submitBuy/submitSell(
            ctx, _code, price, batch_qty, Source::CLOSEOUT, /*flag=*/1);

        // 记录本轮执行信息用于 fill_rate 估计
        recordRound(res, batch_qty, tier, now_ms);

        // SWEEP 兜底后直接完成
        if (tier == SWEEP) {
            // 等最后一轮成交确认，下一 tick check remaining
            _sweep_done = true;
        }
    }
}
```

### 3.4 fill_rate 估计

```cpp
double estimateFillRate() {
    if (_rounds.empty()) {
        // 初始估计: 假设能在 70% 时间内完成
        return _total_to_hedge / (_planned_ms * 0.7 + 1e-6);
    }
    // 取最近 3 轮平均
    double total_filled = 0;
    uint64_t total_time = 0;
    int n = std::min(3, (int)_rounds.size());
    for (int i = _rounds.size() - n; i < _rounds.size(); i++) {
        total_filled += _rounds[i].filled_qty;
        total_time  += _rounds[i].elapsed_ms;
    }
    return total_filled / (total_time + 1e-6);
}
```

### 3.5 价格计算

```cpp
double computePrice(PriceTier tier, bool is_buy) {
    double bid = _contract_state->bid1;
    double ask = _contract_state->ask1;
    double mid = (bid + ask) / 2.0;
    double tick = _contract_state->price_tick;

    switch (tier) {
        case PASSIVE:         return is_buy ? ask - tick     : bid + tick;      // 同侧+1
        case MID_PASSIVE:     return mid;                                       // 中间价
        case AGGRESSIVE:      return is_buy ? ask             : bid;            // 对手价
        case VERY_AGGRESSIVE: return is_buy ? ask + tick      : bid - tick;     // 越过对手
        case SWEEP:           return is_buy ? ask + tick * _cfg.sweep_ticks : bid - tick * _cfg.sweep_ticks; // 对手价+N tick
    }
}
```

## 4. 配置参数

### 4.1 新增配置（挂载到 UftFutuMmStrategy::Closeout 结构体）

```cpp
struct Closeout {
    // --- 现有 ---
    uint32_t minutes_before;        // 收盘前N分钟触发
    bool flatten_position;
    uint32_t max_retries;
    uint32_t retry_interval_ms;
    uint32_t close_time;            // HHMMSS
    uint32_t night_close_time;      // HHMM, 0=无夜盘
    uint32_t night_minutes_before;

    // --- 新增: CloseoutExecutor 参数 ---
    uint32_t drain_timeout_ms;      // Phase1 drain 超时, 默认 3000
    double    depth_ratio_passive;  // 被动档深度比例, 默认 0.3
    double    depth_ratio_mid;      // 中间档深度比例, 默认 0.5
    double    depth_ratio_aggr;     // 主动档深度比例, 默认 0.8
    uint32_t sweep_threshold_ms;    // 距收盘多少ms进入SWEEP兜底, 默认 5000
    uint32_t sweep_ticks;           // SWEEP档越过对手价tick数, 默认 3
    bool      use_fak;              // 是否使用FAK下单, 默认 true

    Closeout()
        : /* 现有初始化... */
        , drain_timeout_ms(3000)
        , depth_ratio_passive(0.3)
        , depth_ratio_mid(0.5)
        , depth_ratio_aggr(0.8)
        , sweep_threshold_ms(5000)
        , sweep_ticks(3)
        , use_fak(true) {}
};
```

### 4.2 默认值取值依据

| 参数 | 默认值 | 理由 |
|---|---|---|
| drain_timeout_ms | 3000 | SHFE ao Tick 500ms, 6 tick 足够 drain |
| depth_ratio_passive | 0.3 | 不超过盘口 30%，避免吃穿 |
| depth_ratio_mid | 0.5 | 中间价略激进 |
| depth_ratio_aggr | 0.8 | 主动档可以吃到 80% |
| sweep_threshold_ms | 5000 | 最后 5s 必须全清，不再等 |
| sweep_ticks | 3 | SWEEP 对手价+3 tick，保证穿透 |
| use_fak | true | 所有档位均 FAK，防止残余挂单 |

## 5. 状态机

```
            trigger
IDLE ──────────────→ DRAINING
                        │
            getOrderCount()==0 || timeout
                        │
                        ▼
                    ASSESSING
                        │
                  read net_delta
                        │
               ┌────────┴────────┐
               │                 │
         |rem|<0.5         |rem|>=0.5
               │                 │
               ▼                 ▼
          COMPLETED        EXECUTING ←─────┐
                              │            │
                     each tick:            │
                     re-assess remaining   │
                     → submit FAK batch    │
                              │            │
                    ┌─────────┴────────┐   │
                    │                  │   │
              |rem|<0.5          sweep_done │
              +verify OK          +verify   │
                    │                  │   │
                    ▼                  ▼   │
              COMPLETED          COMPLETED  │
                                   or      │
                               FAILED ─────┘
                              (retry)
```

## 6. 成本估算对比

以 ao 主力合约、净敞口 50 手为例：

| 策略 | 预估滑点 | 预估成本 |
|---|---|---|
| 现状: 单笔全量对手价 | 5-8 tick | ~2000-3200 元 |
| SWEEP 涨跌停（最差） | 10+ tick | ~4000+ 元 |
| urgency 模型 (3min 窗口) | 1-3 tick | ~400-1200 元 |
| urgency 模型 (5min 窗口) | 0.5-2 tick | ~200-800 元 |

成本节约主要来自：
- **被动档** (urgency<0.5): 在盘口同侧挂单，0-1 tick 滑点
- **分批**: 避免一次吃穿多档深度
- **FAK**: 不留残余挂单，每轮干净退出

## 7. 接口设计 (C++ 类)

### 7.1 CloseoutExecutor.h

```cpp
#pragma once
#include <cstdint>
#include <cmath>
#include <vector>
#include "OrderRouter.h"
#include "UnifiedOrderTracker.h"
#include "FutuPortfolio.h"

namespace futu {

/// Closeout 执行阶段
enum class CloseoutPhase : uint8_t {
    IDLE        = 0,
    DRAINING    = 1,   // 撤销 inflight
    ASSESSING   = 2,   // 评估敞口
    EXECUTING   = 3,   // 渐进对冲
    COMPLETED   = 4,
    FAILED      = 5,
};

/// 定价档位
enum class PriceTier : uint8_t {
    PASSIVE         = 0,  // 同侧 +1 tick
    MID_PASSIVE     = 1,  // 中间价
    AGGRESSIVE      = 2,  // 对手价
    VERY_AGGRESSIVE = 3,  // 对手价 +1 tick
    SWEEP           = 4,  // 涨/跌停
};

/// 单轮执行记录（用于 fill_rate 估计）
struct ExecutionRound {
    uint64_t  submit_ts   = 0;
    double    submitted_qty = 0;
    double    filled_qty    = 0;   // 下一 tick 时从持仓变化推算
    uint64_t  fill_ts     = 0;
    uint64_t  elapsed_ms  = 0;
    PriceTier tier        = PriceTier::PASSIVE;
};

/// CloseoutExecutor 配置
struct CloseoutExecConfig {
    uint32_t drain_timeout_ms     = 3000;
    double   depth_ratio_passive  = 0.3;
    double   depth_ratio_mid      = 0.5;
    double   depth_ratio_aggr     = 0.8;
    uint32_t sweep_threshold_ms   = 5000;
    uint32_t sweep_ticks          = 3;
    bool     use_fak              = true;
};

/// 渐进式收盘对冲执行器
///
/// 调用方式: 每次 on_tick 调用 run()，内部状态机自动推进。
/// 直到 getPhase()==COMPLETED 或 FAILED。
///
/// 线程安全: 非线程安全，仅主线程调用（与 FutuQuoter/OrderRouter 一致）。
class CloseoutExecutor
{
public:
    CloseoutExecutor();

    /// 注入依赖（与 StrategyCoordinator 的 setter 风格一致）
    void setOrderRouter(OrderRouter* router) { _router = router; }
    void setOrderTracker(UnifiedOrderTracker* tracker) { _tracker = tracker; }
    void setPortfolio(FutuPortfolio* portfolio) { _portfolio = portfolio; }
    void setConfig(const CloseoutExecConfig& cfg) { _cfg = cfg; }

    /// 启动收盘对冲序列
    /// @param ctx          策略上下文
    /// @param code         锚定合约代码
    /// @param close_time_ms 收盘时间（ms 时间戳）
    /// @param hedge_ratio  对冲比率
    void start(wtp::IUftStraCtx* ctx,
               const char* code,
               uint64_t close_time_ms,
               double hedge_ratio);

    /// 每 tick 调用，推进状态机
    void run(wtp::IUftStraCtx* ctx);

    /// 重置（新 session 时调用）
    void reset();

    /// 查询当前阶段
    CloseoutPhase getPhase() const { return _phase; }

    /// 是否已完成
    bool isCompleted() const { return _phase == CloseoutPhase::COMPLETED; }
    bool isFailed() const    { return _phase == CloseoutPhase::FAILED; }

    /// 获取剩余手数
    double getRemaining() const { return _remaining; }

    /// 获取已成交手数
    double getFilled() const { return _total_filled; }

private:
    /// Phase 1: 检查 drain 是否完成
    bool checkDrainComplete();

    /// Phase 3: 执行一轮对冲
    void executeRound(wtp::IUftStraCtx* ctx);

    /// 计算 urgency
    double computeUrgency(uint64_t now_ms) const;

    /// 根据 urgency 选择价格档
    PriceTier selectTier(double urgency, uint32_t time_left_ms) const;

    /// 计算分批数量
    double calcBatchQty(double remaining, PriceTier tier) const;

    /// 计算执行价格
    double computePrice(PriceTier tier, bool is_buy) const;

    /// 估计成交速率 (手/ms)
    double estimateFillRate() const;

    /// 完成处理
    void complete();

    // --- 依赖 ---
    OrderRouter*           _router     = nullptr;
    UnifiedOrderTracker*   _tracker    = nullptr;
    FutuPortfolio*         _portfolio  = nullptr;
    CloseoutExecConfig     _cfg;

    // --- 状态 ---
    CloseoutPhase          _phase        = CloseoutPhase::IDLE;
    char                   _code[32]     = {};
    uint64_t               _close_time_ms = 0;
    double                 _hedge_ratio   = 1.0;
    double                 _remaining     = 0;    // 剩余手数（正=需买，负=需卖）
    double                 _total_to_hedge = 0;
    double                 _total_filled   = 0;
    uint64_t               _start_ts       = 0;
    uint64_t               _drain_start_ts = 0;
    bool                   _sweep_done     = false;

    // --- 执行历史（用于 fill_rate） ---
    std::vector<ExecutionRound> _rounds;

    // --- 涨跌停价（从 ContractState 获取，start 时缓存） ---
    double                 _limit_up   = 0;
    double                 _limit_down = 0;
    double                 _price_tick = 0;
};

} // namespace futu
```

### 7.2 与 UftFutuMmStrategy 的集成

```cpp
// UftFutuMmStrategy.h 新增成员:
CloseoutExecutor _closeout_executor;

// on_init 中注入依赖:
_closeout_executor.setOrderRouter(_order_router.get());
_closeout_executor.setOrderTracker(_tracker.get());
_closeout_executor.setPortfolio(_portfolio.get());
_closeout_executor.setConfig(/* 从 _config.closeout 映射 */);

// 改造 executeCloseoutHedge:
void UftFutuMmStrategy::executeCloseoutHedge(IUftStraCtx* ctx) {
    if (!_closeout_executor.isCompleted() && !_closeout_executor.isFailed()) {
        // 首次调用：启动
        if (_closeout_executor.getPhase() == CloseoutPhase::IDLE) {
            const auto* cs = _portfolio->getContract(_config.anchor_code);
            uint64_t close_ms = /* 从 _config.closeout.close_time 计算 */;
            _closeout_executor.start(ctx, _config.anchor_code.c_str(),
                                     close_ms, cs->hedge_ratio);
        }
        // 每 tick 推进
        _closeout_executor.run(ctx);

        // 检查结果
        if (_closeout_executor.isCompleted()) {
            uint64_t now = ctx->stra_get_date() * 1000000ULL + ...;
            _risk_monitor->markCloseoutCompleted(now);
        } else if (_closeout_executor.isFailed()) {
            uint64_t now = ctx->stra_get_date() * 1000000ULL + ...;
            _risk_monitor->markCloseoutFailed(now);
        }
    }
}

// on_session_begin 中重置:
_closeout_executor.reset();
```

### 7.3 对现有 inflight drain 逻辑的影响

**改造前（当前代码 1406-1436行）：**
```
trigger → halt + cancelAll → 等N ticks → executeCloseoutHedge (单笔全量)
```

**改造后：**
```
trigger → halt + cancelAll → 等N ticks → _closeout_executor.start()
     → [executor内部 Phase 1] drain 二次确认 (getOrderCount==0 || timeout)
     → [executor内部 Phase 2-4] 渐进对冲
```

现有的 `_closeout_hedge_pending` + `_closeout_hedge_wait_ticks` 延迟机制保留不动，
它解决的是 cancelAll 回执回流问题。CloseoutExecutor 的 Phase 1 是第二层 drain 确认，
双保险。

## 8. 风险与注意事项

### 8.1 FAK 可用性

- 上期所(SHFE): 支持 FAK
- DCE/CZCE/CFFEX: 支持 FAK
- UftMocker 回测框架: 已确认支持 FAK 撮合

### 8.2 与 P0 Bug 链的关系

- **B1 (drain)**: CloseoutExecutor Phase 1 是 drain 判据的终极实现
- **B2 (FAK 防御)**: 所有档位统一使用 FAK，天然解决残余挂单
- **B5 (AUTO REDUCE 退化)**: 独立改造项，与 CloseoutExecutor 正交
  - AUTO REDUCE 仅管理**正常报价阶段**：净持仓达到 max_position 时触发减仓
  - CloseoutExecutor 管理**收盘对冲阶段**：收盘前清零全部敞口
  - 两者时段互斥：closeout 触发后 halt MM，AUTO REDUCE 不再运行

### 8.3 极端场景

| 场景 | 处理 |
|---|---|
| 流动性枯竭（bid/ask 价差极大） | urgency 快速升高 → 自动升档 → SWEEP |
| 涨跌停封单 | SWEEP 也无法成交 → FAILED → retry |
| drain 超时（订单卡住） | timeout 后强行进入 ASSESSING，inflight 量从 net_delta 推算时已被包含 |
| 夜盘/白盘切换 | on_session_begin → reset()，状态机归零 |

## 9. 实施计划

| 步骤 | 内容 | 预估改动量 |
|---|---|---|
| 1 | CloseoutExecutor.h/.cpp 新增 | ~300行 |
| 2 | UftFutuMmStrategy 集成（成员+注入+改造 executeCloseoutHedge） | ~40行改动 |
| 3 | Closeout 配置结构体扩展 | ~15行 |
| 4 | UftMocker FAK 撮合验证 | 按需 |
| **总计** | | **~350行新增，~55行改动** |

## 10. 已拍板决策

| # | 问题 | 决策 |
|---|---|---|
| Q1 | drain_timeout_ms=3000 是否足够？ | **够**，保留默认 3000ms |
| Q2 | UftMocker 是否已实现 FAK 撮合？ | **是**，已确认支持 |
| Q3 | max_batch_size 要不要限制？ | **不需要**，删除该参数，让 depth_aware_limit 做唯一约束 |
| Q4 | SWEEP 用涨跌停价还是对手价+N tick？ | **对手价+N tick** (sweep_ticks=3)，不用涨跌停 |
| Q5 | 收盘对冲与 AUTO REDUCE 的关系？ | **正交且时段互斥**：AUTO REDUCE 管正常报价阶段(净持仓达 max_position 触发减仓)，CloseoutExecutor 管收盘阶段(清零全部敞口)；closeout halt 后 AUTO REDUCE 不再运行 |
