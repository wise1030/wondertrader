# WtFutuCore 优化改造详细路径
> 2026-06-14 | 逐项精确到文件/行号/改动内容/验证方法
> 2026-06-18 | Phase 2 ✅ 全部完成
>             见 docs/PHASE2_CLOSEOUT_AUDIT.md (审计)
>             见 docs/PAUSE_RESUME_UNIFICATION_AUDIT.md (U1 统一恢复入口)
>             见 docs/ERROR_STATE_MACHINE_AUDIT.md (ERROR 状态机审计)
>             见 docs/ERROR_STATE_MACHINE.md (最终设计 SSOT)

---

## 依赖关系图

```
P0-5(编译验证) ──────────────────────────────────────┐
                                                      ▼
P0-1(AUTO REDUCE语义)     P0-3(柜台错误解绑)     P0-4(条件恢复)
      │                        │                      │
      ▼                        │                      │
P0-2(熔断动作补全) ◄──────────┘                      │
      │                                               │
      ▼                                               ▼
P1-5(删死字段) ◄───────────────────────────── P1-1(状态机统一)
      │                                               │
      │                                        P1-2(持仓SSOT)
      │                                               │
      │                                        P1-3(hedge guard)
      │                                        P1-4(stale价格)
      │                                        P1-6(恢复归一)
      │                                        P1-7(线程安全)
      ▼                                               │
Phase 3(代码质量) ◄──────────────────────────────────┘
      │
Phase 4(业务增强)
```

---

## Phase 0: CloseoutExecutor 收尾验证

### 0.1 MSVC 编译

```
改动文件: 无代码改动
操作: Windows VS 打开 src/build_all/WtFutuCore.sln → 编译 WtFutuCore
验证: 0 errors, 0 warnings (允许 ftime deprecation)
```

潜在编译风险点:
- CloseoutExecutor.h: `depthRatio()` switch 未覆盖所有 enum case → 加 default
- CloseoutExecutor.cpp: `fmt::ptr(this)` 已改为 `(void*)this`，确认无残留
- UftFutuMmStrategy.cpp: `make_unique<CloseoutExecutor>()` 需要 C++14+，确认 CMake -std=c++17

### 0.2 回测验证

```
操作:
  cd dist/WtBtFutu
  LD_LIBRARY_PATH=./uft:$LD_LIBRARY_PATH ./uft/WtBtRunner -c config_ao.yaml -l logcfgbt.yaml
  LD_LIBRARY_PATH=./uft:$LD_LIBRARY_PATH ./uft/WtBtRunner -c config_ec.yaml -l logcfgbt.yaml

验证点:
  1. closeout 触发日志: "CloseoutExecutor: starting after N ticks"
  2. DRAINING phase: "CloseoutExecutor: DRAINING, inflight=N"
  3. ASSESSING phase: "CloseoutExecutor: ASSESSING, net_delta=X"
  4. EXECUTING phase: "CloseoutExecutor: EXECUTING tier=X qty=N @ P"
  5. COMPLETED/FAILED 状态转换
  6. 正常做市不受影响: on_tick 日志中 MM 报价正常刷新
```

### 0.3 Commit

```
git add src/WtFutuCore/CloseoutExecutor.h src/WtFutuCore/CloseoutExecutor.cpp
git add src/WtFutuCore/UftFutuMmStrategy.h src/WtFutuCore/UftFutuMmStrategy.cpp
git add docs/CLOSEOUT_EXECUTOR_DESIGN.md
git commit -m "feat(WtFutuCore): CloseoutExecutor — urgency-driven gradual closeout with FAK batches"
```

---

## Phase 1: P0 安全关键

### P0-1: AUTO REDUCE 语义修正

**问题**:
- UftFutuMmStrategy.cpp:2108 `submitSell(code, price, qty, Source::CLOSEOUT)` 
- submitSell 内部调 ctx->stra_sell = 净仓API
- 多头超限要平多(reduction>0)，应用 submitExitLong(平多仓)
- 空头超限要平空(reduction<0)，应用 submitExitShort(平空仓)

**改动 1: UftFutuMmStrategy.cpp:2105-2145 (AUTO REDUCE 段)**

当前:
```cpp
// reduction > 0: 多头超限，卖出平仓
_order_router->submitSell(ctx, breached->code.c_str(), price, std::abs(reduction), Source::CLOSEOUT);
// else: 空头超限，买入平仓
_order_router->submitBuy(ctx, breached->code.c_str(), price, std::abs(reduction), Source::CLOSEOUT);
```

改为:
```cpp
// reduction > 0: 多头超限，平多仓
_order_router->submitExitLong(ctx, breached->code.c_str(), price, std::abs(reduction),
    false /*isToday由框架ActionPolicy判定*/, Source::CLOSEOUT);
// else: 空头超限，平空仓
_order_router->submitExitShort(ctx, breached->code.c_str(), price, std::abs(reduction),
    false, Source::CLOSEOUT);
```

else 分支(stra_sell/stra_buy 直调)也对应改:
```cpp
// 从:
ctx->stra_sell(breached->code.c_str(), price, std::abs(reduction));
ctx->stra_buy(breached->code.c_str(), price, std::abs(reduction));
// 改为:
ctx->stra_exit_long(breached->code.c_str(), price, std::abs(reduction), false);
ctx->stra_exit_short(breached->code.c_str(), price, std::abs(reduction), false);
```

**改动 2: CloseoutExecutor.cpp:415-440 (下单段)**

当前:
```cpp
// isBuy 分支
_router->submitBuy(ctx, _code, price, qty, src, flag);  // flag=1 FAK
// !isBuy 分支
_router->submitSell(ctx, _code, price, qty, src, flag);
// else 直调
ctx->stra_buy(_code, price, qty, flag);
ctx->stra_sell(_code, price, qty, flag);
```

CloseoutExecutor 需要知道是平多还是平空。在 start() 计算 net_delta 时已确定方向:
- net_delta > 0 → 需要卖出 → 可能平多也可能开空
- net_delta < 0 → 需要买入 → 可能平空也可能开多

按 D4 设计，CloseoutExecutor 是平仓语义，用 submitExitLong/submitExitShort:
```cpp
// 方向: 需要卖出时(remaining>0且方向为SELL) → 平多仓
// 方向: 需要买入时(remaining<0且方向为BUY) → 平空仓
if (_sell_direction) {
    // 平多仓
    _router->submitExitLong(ctx, _code, price, qty, false, src, flag);
} else {
    // 平空仓
    _router->submitExitShort(ctx, _code, price, qty, false, src, flag);
}
```

需要在 CloseoutExecutor 中新增 `_sell_direction` 成员，在 start() 时根据 net_delta 符号设置。

**改动 3: OrderRouter.h — 确认 submitExitLong/submitExitShort 已支持 flag=1 (FAK)**

已确认: OrderRouter.h:154-170 有 submitExitLong/submitExitShort，都有 `int flag = 0` 默认参数。FAK 传 flag=1 即可。

**验证**:
```
1. 语法检查: g++ -std=c++17 -fsyntax-only CloseoutExecutor.cpp UftFutuMmStrategy.cpp
2. 回测: ao 1日 → AUTO REDUCE 日志改为 "EXIT_LONG/EXIT_SHORT" 而非 "SELL/BUY"
3. 回测: closeout 日志 → 下单走 submitExitLong/submitExitShort
```

---

### P0-2: 硬熔断兜底动作补全

**问题**:
- StrategyCoordinator.cpp:665-672: HALT_TRADING 分支只调 haltTrading() + halt()
- haltTrading()(FutuRiskMonitor.cpp:428) 只设 _trading_halted=true + broadcastAlert
- 缺失: cancelAll + force flat

**改动: StrategyCoordinator.cpp HALT_TRADING 分支**

当前:
```cpp
case RiskAction::HALT_TRADING:
    _trading_state->halt(TradingState::PauseReason::RISK_LIMIT);
    _risk_monitor->haltTrading(category, _portfolio->getTotalPnL());
    // arb disable...
    break;
```

改为:
```cpp
case RiskAction::HALT_TRADING:
    _trading_state->halt(TradingState::PauseReason::RISK_LIMIT);
    _risk_monitor->haltTrading(category, _portfolio->getTotalPnL());
    
    // P0-2: 熔断后动作补全 — 撤所有做市单
    if (_quoters) {
        for (auto& [code, quoter] : *_quoters) {
            quoter->cancelAll(ctx);
        }
    }
    // 撤所有 OrderRouter 活跃单
    if (_order_router) {
        _order_router->cancelAllBySource(ctx, Source::CLOSEOUT);
        _order_router->cancelAllBySource(ctx, Source::HEDGE);
    }
    
    // IRREVERSIBLE → 强平(市价/对手价)
    if (category == RiskCategory::IRREVERSIBLE) {
        double delta = _portfolio->getTotalDelta();
        if (std::abs(delta) > 0.01 && _order_router) {
            const std::string& anchor = _portfolio->getAnchorContract();
            const ContractState* cs = _portfolio->getContract(anchor);
            if (cs && cs->last_price > 0) {
                double qty = std::abs(delta);
                if (delta > 0) {
                    _order_router->submitExitLong(ctx, anchor.c_str(), cs->bid1, qty, true, Source::CLOSEOUT, 1 /*FAK*/);
                } else {
                    _order_router->submitExitShort(ctx, anchor.c_str(), cs->ask1, qty, true, Source::CLOSEOUT, 1 /*FAK*/);
                }
                WTSLogger::error("[RISK] FORCE FLAT: delta={:.1f}, anchor={}, qty={:.0f} @ {}", 
                    delta, anchor, qty, delta > 0 ? cs->bid1 : cs->ask1);
            }
        }
    }
    // arb disable...
    break;
```

注意: force flat 只在 IRREVERSIBLE 时触发(REVERSIBLE 的 HALT 可能自动恢复，不需要强平)。

**前置条件**: StrategyCoordinator::processTick 需要访问 ctx 参数。当前签名 `processTick(IUftStraCtx* ctx, ...)` 已有 ctx。但 HALT_TRADING 分支在 processTick 内部，ctx 可用。

**改动: on_channel_lost 也需要检查**

当前 on_channel_lost(:2180-2215) 已经有 cancelAll + halt，是正确的。确认不需要改。

**验证**:
```
1. 构造 maxDailyLoss 触发场景(配置 maxDailyLoss 很小)
2. 回测日志应出现: "TRADING_HALTED (IRREVERSIBLE)" + "FORCE FLAT" + cancelAll
3. 确认 force flat 后 total delta 趋近 0
```

---

### P0-3: 柜台错误分类抽象

**问题**:
- UftFutuMmStrategy.cpp:2496-2633 (138行) on_entrust
- 6 组 GBK 字节序列匹配 CTP 错误文本
- 强耦合 CTP + GBK，换柜台立刻全部失效

**设计**:

新建文件 `IErrorClassifier.h`:
```cpp
#pragma once
#include <string>

namespace futu {

/// 订单错误语义类别（策略层只关心语义，不关心柜台原始文本）
enum class OrderErrorCategory : uint8_t {
    SUCCESS,              // 成功
    POSITION_SYNC,        // 持仓同步问题（可恢复，需重同步）
    RATE_LIMIT,           // 交易所流控（可恢复，需冷却）
    CLOSE_INSUFFICIENT,   // 可平仓位不足（非关键）
    UNKNOWN_RECOVERABLE,  // 未知但可恢复
    FATAL                 // 不可恢复，需人工介入
};

/// 柜台错误分类器接口
class IErrorClassifier {
public:
    virtual ~IErrorClassifier() = default;
    /// 将柜台返回的原始错误文本分类为语义类别
    virtual OrderErrorCategory classify(bool bSuccess, const char* message) const = 0;
};

} // namespace futu
```

新建文件 `DefaultErrorClassifier.h/.cpp`（通用 fallback）:
```cpp
/// 通用错误分类器: 只区分成功/失败，失败一律 UNKNOWN_RECOVERABLE
class DefaultErrorClassifier : public IErrorClassifier {
public:
    OrderErrorCategory classify(bool bSuccess, const char* message) const override {
        if (bSuccess) return OrderErrorCategory::SUCCESS;
        return OrderErrorCategory::UNKNOWN_RECOVERABLE;
    }
};
```

新建文件 `CtpErrorClassifier.h/.cpp`（CTP 特定）:
- 把 on_entrust 中现有的 GBK 匹配逻辑搬过来
- 返回 OrderErrorCategory 而非直接做行为决策
- 通过配置文件选择 classifier: `error_classifier: "ctp"` / `"default"`

**改动: UftFutuMmStrategy.h — 新增成员**:
```cpp
#include "IErrorClassifier.h"
// ...
std::unique_ptr<IErrorClassifier> _error_classifier;
```

**改动: UftFutuMmStrategy.cpp on_init — 初始化 classifier**:
```cpp
// 根据 config 选择 classifier
std::string clf_name = readString(cfg, "errorClassifier", "default");
if (clf_name == "ctp") {
    _error_classifier = std::make_unique<CtpErrorClassifier>();
} else {
    _error_classifier = std::make_unique<DefaultErrorClassifier>();
}
```

**改动: UftFutuMmStrategy.cpp on_entrust — 重写为分类驱动**:

当前 138 行 → 重构为约 40 行:
```cpp
void UftFutuMmStrategy::on_entrust(uint32_t localid, bool bSuccess, const char* message) {
    if (bSuccess) {
        _order_error_count = 0;
        if (_trading_state.quoting_paused && !_trading_state.trading_halted) {
            _trading_state.resumeQuoting();
            _quoting_paused_since = 0;
        }
        return;
    }
    
    auto cat = _error_classifier->classify(false, message);
    std::string msg = message ? message : "";
    
    switch (cat) {
    case OrderErrorCategory::CLOSE_INSUFFICIENT:
        WTSLogger::warn("UftFutuMmStrategy[{}] CTP close position error (non-critical): {}", id(), msg);
        return;  // 不计入错误计数
        
    case OrderErrorCategory::POSITION_SYNC:
        WTSLogger::warn("UftFutuMmStrategy[{}] Position sync error: {}, will re-sync", id(), msg);
        _order_error_count = 0;
        _portfolio_ctx_dirty = true;
        _trading_state.pauseQuoting(TradingState::PauseReason::ORDER_ERROR);
        _quoting_paused_since = TimeUtils::getLocalTimeNow();
        return;
        
    case OrderErrorCategory::RATE_LIMIT:
        WTSLogger::warn("UftFutuMmStrategy[{}] Exchange rate limit: {}, cooling down", id(), msg);
        _order_error_count = 0;
        _trading_state.pauseQuoting(TradingState::PauseReason::ORDER_ERROR);
        _quoting_paused_since = TimeUtils::getLocalTimeNow();
        return;
        
    case OrderErrorCategory::FATAL:
        _order_error_count = _config.order_control.order_error_threshold;
        break;  // 走下面的 halt 逻辑
        
    default:  // UNKNOWN_RECOVERABLE
        _order_error_count++;
        break;
    }
    
    // 统一的错误计数处理 (与当前逻辑相同)
    if (_order_error_count >= _config.order_control.order_error_threshold) {
        _trading_state.pauseQuoting(TradingState::PauseReason::ORDER_ERROR);
        _trading_state.halt(TradingState::PauseReason::ORDER_ERROR);
        // ... cancelAll 等
    } else {
        _trading_state.pauseQuoting(TradingState::PauseReason::ORDER_ERROR);
        _quoting_paused_since = TimeUtils::getLocalTimeNow();
    }
}
```

**验证**:
```
1. 新文件语法检查
2. 回测: on_entrust 正常触发 → 日志中应看到语义类别而非 GBK 匹配
3. 确认 DefaultErrorClassifier 回退不 crash
```

---

### P0-4: 定时器恢复改为条件恢复

**问题**:
- UftFutuMmStrategy.cpp:1301-1310: 10 秒固定超时 auto-resume
- CTP 流控期间形成 10 秒周期报错循环

**改动: UftFutuMmStrategy.cpp:1301-1310**

当前:
```cpp
if (paused_ms > 10000) {
    _trading_state.resumeQuoting();
    _quoting_paused_since = 0;
    _order_error_count = 0;
}
```

改为渐进恢复(三段式):
```cpp
if (paused_ms > 10000) {
    uint32_t current_errors = _order_error_count;
    if (current_errors == 0) {
        // 暂停期间没有新错误 → 安全恢复
        _trading_state.resumeQuoting();
        _quoting_paused_since = 0;
    } else {
        // 仍有错误 → 指数退避
        uint64_t next_wait = std::min(paused_ms * 2, static_cast<uint64_t>(60000)); // 最长60秒
        WTSLogger::warn("UftFutuMmStrategy[{}] Quoting still paused ({} errors), "
            "extending wait to {}ms", id(), current_errors, next_wait);
        _quoting_paused_since = TimeUtils::getLocalTimeNow() - paused_ms + next_wait;
        // 不 reset error_count，让计数自然衰减
    }
}
```

**改动: on_entrust 成功路径 — 重置 _quoting_paused_since**

确认 on_entrust 成功路径(:2499-2510)已 reset _order_error_count=0，这样如果暂停期间有报单成功回报，下次 tick 检查时 errors=0 可安全恢复。

**验证**:
```
1. 回测构造流控场景: 频繁报单触发 RATE_LIMIT
2. 日志应显示 "extending wait" 而非周期恢复-报错
3. 报单成功后下次 tick 恢复
```

---

### P1-5: 删 closeout_mode 死字段

**改动: TradingState.h:28**

删除: `bool closeout_mode = false;`

**改动: UftFutuMmStrategy.cpp:1301**

当前: `&& !_trading_state.closeout_mode &&`
改为: 删除 `!_trading_state.closeout_mode` 条件

或替换为: `&& _closeout_executor && !_closeout_executor->isActive()`

这是更好的语义：报价恢复前检查 CloseoutExecutor 是否在运行。

**验证**: 编译通过 + grep 无残留 closeout_mode 引用

---

## Phase 2: 状态机统一 + 持仓一致性

### P1-1: 状态机统一

**前置条件**: 需用户拍板 Q1-Q6（方案选型/drain判据/超时兜底等）

此步详细方案见已落盘的 TradingState v4 重构报告。核心改动:

1. TradingState.h: 删除所有扁平 bool，替换为 `MmPhase phase` + 正交 `bool long_blocked/short_blocked`
2. 新增 transitionTo() 真状态机
3. CloseoutState(RiskMonitor) 和 CloseoutExecutor::CloseoutPhase 迁入 MmPhase
4. 全部 31+30 处读写点改为读 phase / 调 transitionTo()

改动文件:
- TradingState.h (大改)
- FutuRiskMonitor.h/.cpp (瘦身: 删 CloseoutState 枚举，保留数据)
- CloseoutExecutor.h/.cpp (phase 映射到 MmPhase)
- UftFutuMmStrategy.cpp (~30 处读写改)
- StrategyCoordinator.cpp (~31 处读写改)

预估: 1.5-2 天（含编译调试+回归）

---

### P1-2: 持仓决策一致性

**问题**:
- FutuPortfolio::getTotalDelta() 直接遍历 _contracts 读 c.position
- c.position 在 on_trade(:1707) 和 on_position(:1986) 时同步
- 两次同步间隙可能滞后 WT 引擎真实持仓

**设计决策**: FutuPortfolio 在关键决策前从 ctx 重读策略持仓

**改动: FutuPortfolio.h — 新增 syncFromCtx 方法**

```cpp
/// 决策前从策略引擎同步持仓（确保 delta 计算基于最新策略持仓）
/// 注意: 同步的是 stra_get_local_position（策略持仓），不是账户持仓
void syncFromCtx(wtp::IUftStraCtx* ctx) {
    if (!ctx) return;
    for (auto& c : _contracts) {
        double actual = ctx->stra_get_local_position(c.code.c_str());
        if (std::abs(c.position - actual) > 0.01) {
            WTSLogger::trace("Portfolio sync: {} {}->{} (ctx)", c.code, c.position, actual);
            c.position = actual;
        }
    }
}
```

**改动: StrategyCoordinator.cpp — 在 delta 决策前调用**

在 checkAndHedge()(:982) 开头:
```cpp
bool StrategyCoordinator::checkAndHedge(wtp::IUftStraCtx* ctx) {
    // P1-2: 决策前同步策略持仓
    if (_portfolio) _portfolio->syncFromCtx(ctx);
    // ... 后续逻辑不变
}
```

在 processCloseout()(:301) 中 executor.start() 前:
```cpp
// CloseoutExecutor 启动前同步
_portfolio->syncFromCtx(ctx);
```

**注意**: 不在 processQuoting 前同步（太频繁，报价层用 Portfolio 持仓做 skew 即可，微小滞后不影响）。只在对冲/收盘等"大决策"前同步。

**验证**:
```
1. 回测日志中应看到 "Portfolio sync: code X->Y (ctx)" 出现在 hedge/closeout 前
2. closeout net_delta 应与 stra_get_local_position 一致
```

---

### P1-3: computeHedge 持仓上限 guard

**改动: FutuPortfolio.cpp:150 computeHedge()**

在 `action.qty = std::round(hedgeQty);` 之后新增:
```cpp
// P1-3: anchor 持仓上限 guard — 防止 hedge 导致 anchor 反向超限
const ContractState* anchor = getContract(_anchor_code);
if (anchor && anchor->max_position > 0) {
    double anchor_pos = anchor->position;
    double projected = anchor_pos + action.qty;  // hedge 后 anchor 预期持仓
    double max_pos = anchor->max_position;
    
    if (projected > max_pos) {
        action.qty = max_pos - anchor_pos;  // 截断到上限
        action.is_urgent = true;
        WTSLogger::warn("computeHedge: clamped to anchor max_position (pos={:.0f}, raw_qty={:.0f}, clamped_qty={:.0f})",
            anchor_pos, std::round(hedgeQty), action.qty);
    } else if (projected < -max_pos) {
        action.qty = -max_pos - anchor_pos;
        action.is_urgent = true;
        WTSLogger::warn("computeHedge: clamped to anchor -max_position (pos={:.0f}, raw_qty={:.0f}, clamped_qty={:.0f})",
            anchor_pos, std::round(hedgeQty), action.qty);
    }
}
```

**验证**:
```
1. 回测构造 delta 超限场景，anchor 已接近 maxPosition
2. 日志应出现 "clamped to anchor max_position"
3. anchor 持仓不超 maxPosition
```

---

### P1-4: channel_ready stale 价格保护

**改动: UftFutuMmStrategy.h — 新增成员**:
```cpp
bool _price_stale = false;  ///< 价格过期标志（channel恢复后到首tick之间）
```

**改动: UftFutuMmStrategy.cpp on_channel_ready:1993**:
```cpp
_channel_ready = true;
_price_stale = true;  // P1-4: 标记价格过期，直到收到首个 tick
```

**改动: UftFutuMmStrategy.cpp on_tick 开头(:1325 后)**:
```cpp
// P1-4: 收到首个 tick 后清除 stale 标志
if (_price_stale) {
    _price_stale = false;
    WTSLogger::info("UftFutuMmStrategy[{}] Price recovered (first tick after channel ready)", id());
}
```

**改动: on_tick closeout/coordinator 调用前(:1392 前检查)**:
```cpp
// P1-4: 价格过期时跳过 closeout/hedge 决策
if (!_price_stale) {
    _coordinator->processTick(ctx, stdCode, tick);
}
```

注意: markToMarket(:1330) 仍需要执行，它更新 _last_mid，是恢复价格的前提。

**改动: on_tick AUTO REDUCE 段(:2081 前检查)**:
```cpp
if (!_price_stale && !has_position_breach) {
    // AUTO REDUCE 逻辑
}
```

**验证**:
```
1. 构造 channel lost → ready 场景
2. ready 后到首 tick 之间不应有 hedge/closeout/AUTO REDUCE 动作
3. 首 tick 后 "Price recovered" 日志
```

---

### P1-6 + P1-7: 风控恢复归一 + 线程安全

这两个项与 P1-1 状态机统一强耦合，建议在 Phase 2 一起做:
- P1-6: 6 条 resume 路径在 transitionTo() 中归一
- P1-7: MmPhase 用 std::atomic 或加线程约束注释

预估: 0.5 天（搭 P1-1 的便车）

---

## Phase 3: 代码质量

### P2-1: 函数拆分

**on_tick (343行) → 拆为 5 个子函数**:

```
void on_tick(ctx, stdCode, tick) {
    if (!_channel_ready || !tick) return;
    handleQuotingAutoResume();        // 10行
    handleMarketDataUpdate(ctx, stdCode, tick);  // 60行 (markToMarket/correlation/hedge_ratio)
    handleCloseout(ctx, stdCode, tick);          // 80行 (closeout executor 驱动)
    handleSpreadArbitrage(ctx, stdCode, tick);    // 30行
    // coordinator processTick 调用
}
```

**initBusinessModules (768行) → builder pattern**:

```
void initBusinessModules(ctx) {
    initCoordinator();     // ~50行
    initPortfolio();       // ~50行
    initQuoters();         // ~100行 (循环体提取)
    initOptimizers();      // ~50行
    initOrderTracker();    // ~30行
    initOrderRouter();     // ~30行
    initRiskMonitor();     // ~50行
    initCloseoutExecutor();// ~30行
    initArbitrage();       // ~80行
    initSignalAggregators();// ~80行
    initMarketData();      // ~30行
}
```

预估: 1.5 天

---

### P2-3: FIX/BUG 标记审计

153 处分类:
- 已修复的历史标记(如 "FIX P0-7")→ 确认修复已生效后删除标记
- 未修复的真实 TODO → 转入路线图跟踪
- 描述性注释(如 "FIX: market_paused must be cleared") → 保留或改写为正常注释

操作: 逐文件 grep + 人工判定，分批 commit。

预估: 0.5 天

---

## Phase 4: 业务增强

### P1-8: 跨期同步报价组

设计: 在 QuoterConfig 中新增 `sync_group` 配置项，同组合约共享报价时序。

```yaml
quoting:
  sync_group: "ec_main"  # ec2607/ec2608/ec2609 同组
```

效果: 同组合约在同一 tick 窗口内同步刷新报价，避免非同步报价导致的 adverse selection。

预估: 1 天

### P2-4: SpreadOptimizer 参数调优

当前已有 toxicity_spread_factor(SpreadOptimizer.h:44)。需要:
1. 回测不同参数组合(1.2/1.5/2.0)对比 adverse selection 指标
2. 确认 ec 成交时 my_spread 中位数(当前 1.0)在调参后是否改善

预估: 0.5 天

---

## 工期总览

| Phase | 工期 | Gate |
|-------|------|------|
| Phase 0 | 1-2天 | 编译+回测通过 |
| Phase 1 (P0) | 3-4天 | 5项P0全清零 |
| Phase 2 (P1) | 4-5天 | 状态机统一+持仓SSOT |
| Phase 3 (P2) | 3-4天 | 函数拆分+FIX清理 |
| Phase 4 (P1-8/P2-4) | 1.5天 | 业务增强 |
| **总计** | **~17天** | |

每项改动后都需要: 语法检查 → ao 1日回测 → 确认无回归 → commit。
