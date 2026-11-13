# MM 软风控 v3 设计文档

**版本**: v3（最终拍板版）
**日期**: 2026-06-08
**作者**: 奶酪 + 用户联合设计
**前置**: 取代 v1（硬 BLOCK + obligation 路径）、v2（含 spread VaR 维度）

---

## 1. 背景

UFT 期货做市策略 `UftFutuMmStrategy` 在多合约回测中暴露三个核心问题：

1. **持仓死锁**：单合约 `max_position` 是硬 BLOCK，打满后 `checkPreTradePosition` 直接 `allow_bid=false` → `bidQty=0` → 无报价 → 持仓永不回归
2. **风控语义不一致**：portfolio_delta 和 contract_delta 都是软（skew 调制），唯独 contract_position 是硬卡
3. **收盘前平仓策略不优**：当前 doCloseout 在各合约自己 `stra_sell` 平仓，流动性差→滑点大→收盘剩裸仓（实测 +79 手）

设计回应：
- 单合约 position 软化为 utilization + qty 衰减 + obligation 报价
- 收盘前不再逐合约平仓，改为"停报 + 主力对冲 net_delta"
- 信任 delta skew 自然抑制 spread 失衡（不引入 spread VaR 监控）

---

## 2. 核心约束（已拍板）

| 项 | 值 | 来源 |
|---|---|---|
| `useBilateralQuote` | 永久 false（实盘也关） | Q1 |
| 做市义务报价 spread | ≤ 10 ticks | Q2 |
| 做市义务报价 qty | ≥ 10 手 | Q2 |
| 跨合约 spread VaR 监控 | 不引入（信任 skew） | 用户决策 |
| 收盘前时间窗 | T-15 停报 / T-10 限价对冲 / T-3 市价对冲 | Q13 |
| closeout 模式 | `hedge_only`（保留 leg 次日继续） | Q14 |
| OrderRouter 优先级 | 常态 HEDGE / 收盘 CLOSEOUT | Q15 |
| skew 权重 | portfolio: 0.5, contract: 1.0（温和） | Q17 |

---

## 3. 架构总览

```
        组合层                              合约层
   portfolio_delta                    contract_delta + position
        │                                     │
   ┌────┴────┐                       ┌────────┴────────┐
   │         │                       │                 │
  skew    anchor                  动态 skew         qty 衰减
  调制    hedge                   (权重1.0)        + obligation
                                                      报价(打满)

                收盘前流程（新）
                      │
       T-15min → QUOTE_STOPPED   (cancelAll + 停 refreshQuotes)
       T-10min → HEDGING          (anchor 限价对冲 net_delta)
       T-3min  → EMERGENCY_HEDGING (anchor 市价穿透)
       |delta|≤tol → COMPLETED    (leg 保留过夜)
       T-0 未平 → FAILED + alert
```

---

## 4. 三大改动

### 4.1 改动 1: 单合约 position 软化（Phase 1）

**FutuRiskMonitor.h - PreTradeResult 扩展**:

```cpp
struct PreTradeResult {
    bool allow_bid = true;          // 不再因 position 设 false
    bool allow_ask = true;
    
    // 新增：utilization（用于 skew 计算）
    double long_utilization = 0.0;    // proj_long / soft_cap
    double short_utilization = 0.0;   // proj_short / soft_cap
    
    // 新增：obligation 标志（util >= 1.0 时 true）
    bool force_ask_obligation = false;  // 多头打满 → ask 必须保持义务报价
    bool force_bid_obligation = false;  // 空头打满 → bid 必须保持义务报价
};
```

**FutuRiskMonitor.cpp:1093-1122 checkPreTradePosition 改造**:

```cpp
PreTradeResult FutuRiskMonitor::checkPreTradePosition(
    const std::string& code,
    double pending_buy, double pending_sell)
{
    PreTradeResult result;
    auto* cs = _portfolio->getContract(code);
    if (!cs || cs->max_position <= 0) return result;
    
    double projected_long  = std::max(0.0, cs->position + pending_buy);
    double projected_short = std::max(0.0, -cs->position + pending_sell);
    
    result.long_utilization  = projected_long  / cs->max_position;
    result.short_utilization = projected_short / cs->max_position;
    
    // 软化：不再 block，只设 obligation 标志
    if (result.long_utilization >= 1.0) {
        result.force_ask_obligation = true;   // 强制减仓侧义务报价
        WTSLogger::warn("[RISK] {} long cap reached: util={:.2f} → ask obligation",
            code, result.long_utilization);
    }
    if (result.short_utilization >= 1.0) {
        result.force_bid_obligation = true;
        WTSLogger::warn("[RISK] {} short cap reached: util={:.2f} → bid obligation",
            code, result.short_utilization);
    }
    
    return result;
}
```

**FutuQuoter.cpp:60-82 refreshQuotes 改造**:

```cpp
// 删除旧的 is_obligation_bid/ask + use_bilateral 整段（67-77）

double bidQty = computeQty(i);
double askQty = computeQty(i);

// (A) 加仓侧 qty 指数衰减
if (cs->position > 0 && long_util > 0) {
    bidQty *= std::exp(-_cfg.qty_decay_factor * long_util);
    if (long_util >= 1.0) bidQty = 0;        // 软关闭加仓侧
}
if (cs->position < 0 && short_util > 0) {
    askQty *= std::exp(-_cfg.qty_decay_factor * short_util);
    if (short_util >= 1.0) askQty = 0;
}

// (B) Obligation 报价（仅 L0）
if (force_ask_obligation && i == 0) {
    askQty = std::max(askQty, _cfg.obligation_min_qty);   // ≥ 10
    double max_ask = mid + _cfg.obligation_max_spread_ticks * _cfg.tick_size;
    askPrice = std::min(askPrice, max_ask);               // ≤ 10 ticks
}
if (force_bid_obligation && i == 0) {
    bidQty = std::max(bidQty, _cfg.obligation_min_qty);
    double min_bid = mid - _cfg.obligation_max_spread_ticks * _cfg.tick_size;
    bidPrice = std::max(bidPrice, min_bid);
}

// (C) 价格保护 + 涨跌停验证（保留原逻辑）
// (D) Sticky 更新判断（保留原逻辑）
```

**qty 衰减曲线（qty_decay_factor=2.0）**:

| util | 衰减系数 | 示例 base=5 |
|---|---|---|
| 0.0 | 1.000 | 5.0 |
| 0.3 | 0.549 | 2.7 |
| 0.5 | 0.368 | 1.8 |
| 0.7 | 0.247 | 1.2 |
| 0.9 | 0.165 | 0.8 |
| 1.0 | 强制 0 | 0 |

---

### 4.2 改动 2: 双维 skew（Phase 1 + Phase 2）

**总 skew 公式**（在 StrategyCoordinator 计算后传给 FutuQuoter）:

```cpp
double total_skew_ticks = 
      _cfg.skew_weight_portfolio * (portfolio_delta / portfolio_max_delta)  // 0.5
    + _cfg.skew_weight_contract  * (contract_delta / contract_max_delta)    // 1.0
    + toxicity_skew;

// 截断到 max_skew_adjustment（沿用 MarketMakingEnhancer.cpp:70）
total_skew_ticks = std::clamp(total_skew_ticks, 
    -_cfg.max_skew_adjustment, _cfg.max_skew_adjustment);
```

**温和值的设计意图（Q17）**：
- portfolio 权重 0.5：组合超载时拉动 skew 不至于过激（多合约自然分散）
- contract 权重 1.0：单合约失衡明显但不过激
- util=0.5 时 skew ≈ 0.5+0.25=0.75 ticks（几乎无感）
- util=1.0 时 skew ≈ 1.0+0.5=1.5 ticks（明显减仓压力）

**常态 anchor_hedge 触发链**:

```cpp
// 在 on_tick 或 on_minute 调度
if (portfolio_delta_util > _cfg.anchor_hedge.trigger_portfolio_delta_util) {  // 0.8
    HedgeAction action = portfolio->computeHedge();
    if (action.qty != 0) {
        // 走 OrderRouter HEDGE 优先级
        orderRouter->submit(action, Source::HEDGE);
    }
}
```

---

### 4.3 改动 3: 收盘前停报 + 主力对冲（Phase 3）

**新增状态机**（替代旧 CloseoutState）:

```cpp
enum class ClosingPhase {
    IDLE,                  // 正常做市
    QUOTE_STOPPED,         // T-15min: 停报 + cancelAll
    HEDGING,               // T-10min: anchor 限价对冲
    EMERGENCY_HEDGING,     // T-3min: anchor 市价穿透
    COMPLETED,             // |net_delta| ≤ tolerance
    FAILED,                // T-0 未平
};
```

**触发逻辑**（StrategyCoordinator::onMinuteBoundary 或 onTick 检查）:

```cpp
uint32_t minutes_to_close = computeMinutesToClose(now, close_time);

switch (_closing_phase) {
case ClosingPhase::IDLE:
    if (minutes_to_close <= _cfg.closeout.quote_stop_minutes) {
        transitionTo(QUOTE_STOPPED);
        for (auto& [code, quoter] : *_quoters) quoter->cancelAll(ctx);
        for (auto& [code, quoter] : *_quoters) quoter->freeze();  // 新增 frozen 模式
    }
    break;

case ClosingPhase::QUOTE_STOPPED:
    if (minutes_to_close <= _cfg.closeout.hedge_start_minutes) {
        transitionTo(HEDGING);
        executeAnchorHedge(/*emergency=*/false);
    }
    break;

case ClosingPhase::HEDGING:
    if (minutes_to_close <= _cfg.closeout.emergency_minutes) {
        transitionTo(EMERGENCY_HEDGING);
        executeAnchorHedge(/*emergency=*/true);
    } else if (std::abs(portfolio->getTotalDelta()) <= _cfg.closeout.net_delta_tolerance) {
        transitionTo(COMPLETED);
    } else {
        // 重新评估 + 追加对冲单
        executeAnchorHedge(/*emergency=*/false);
    }
    break;

case ClosingPhase::EMERGENCY_HEDGING:
    if (std::abs(portfolio->getTotalDelta()) <= _cfg.closeout.net_delta_tolerance) {
        transitionTo(COMPLETED);
    } else if (minutes_to_close == 0) {
        transitionTo(FAILED);
        broadcastAlert("CLOSEOUT_FAILED", "Could not hedge to delta-neutral by close");
    } else {
        executeAnchorHedge(/*emergency=*/true);
    }
    break;
}
```

**executeAnchorHedge 实现**:

```cpp
void StrategyCoordinator::executeAnchorHedge(bool emergency)
{
    double net_delta = _portfolio->getTotalDelta();
    if (std::abs(net_delta) <= _cfg.closeout.net_delta_tolerance) return;
    
    const std::string& anchor = _portfolio->getAnchorContract();
    auto* anchor_cs = _portfolio->getContract(anchor);
    if (!anchor_cs) return;
    
    // 对冲量 = -net_delta / anchor.hedge_ratio
    double hedge_qty = std::round(-net_delta / anchor_cs->hedge_ratio);
    bool is_buy = hedge_qty > 0;
    double abs_qty = std::abs(hedge_qty);
    
    // 计算价格
    auto* md = _market_data[anchor].get();
    double price;
    if (emergency) {
        // 市价穿透：买用 ask+N ticks，卖用 bid-N ticks
        double penetrate_ticks = _cfg.closeout.emergency_penetrate_ticks;  // 默认 3
        price = is_buy 
            ? md->bestAsk() + penetrate_ticks * anchor_cs->tick_size
            : md->bestBid() - penetrate_ticks * anchor_cs->tick_size;
    } else {
        // 限价对价（对手价）
        price = is_buy ? md->bestAsk() : md->bestBid();
    }
    
    // 通过 OrderRouter 提交（CLOSEOUT 优先级）
    auto res = is_buy
        ? _order_router->submitBuy(_ctx, anchor.c_str(), price, abs_qty, Source::CLOSEOUT)
        : _order_router->submitSell(_ctx, anchor.c_str(), price, abs_qty, Source::CLOSEOUT);
    
    if (res.rate_limited || res.self_trade_blocked) {
        WTSLogger::warn("[CLOSEOUT_HEDGE] {} {} {}@{} rejected: rate_lim={} self_trd={}",
            anchor, is_buy ? "BUY" : "SELL", abs_qty, price,
            res.rate_limited, res.self_trade_blocked);
    } else {
        WTSLogger::info("[CLOSEOUT_HEDGE] {} {} {}@{} (emergency={}, net_delta={:.2f})",
            anchor, is_buy ? "BUY" : "SELL", abs_qty, price, emergency, net_delta);
    }
}
```

**FutuQuoter::freeze() / unfreeze() 新增**:

```cpp
void FutuQuoter::freeze() { _frozen = true; }
void FutuQuoter::unfreeze() { _frozen = false; }

// refreshQuotes 开头
void FutuQuoter::refreshQuotes(...) {
    if (_frozen) return;   // 冻结期间不产生任何报价
    // ... 原逻辑
}
```

---

## 5. 完整配置（v3）

```yaml
risk:
  # 单合约软化
  contract_position_soft_cap: 50
  qty_decay_factor: 2.0
  
  # 双维 skew（温和值）
  skew_weights:
    portfolio: 0.5
    contract:  1.0
  
  # 做市义务（Q2 锁定）
  obligation:
    min_qty: 10
    max_spread_ticks: 10
    only_l0: true

# 常态主力对冲（HEDGE 优先级）
anchor_hedge:
  enabled: true
  anchor_codes:
    ao: ao2509          # 主力（待用户填实际主力月份）
    rb: rb2510
  trigger_contract_delta_util: 0.7
  trigger_portfolio_delta_util: 0.8
  hedge_aggressiveness: 1   # 0=被动 1=对价 2=穿透

# 收盘前主力对冲（CLOSEOUT 优先级，替代 flatten）
closeout:
  mode: hedge_only
  quote_stop_minutes: 15
  hedge_start_minutes: 10
  emergency_minutes: 3
  net_delta_tolerance: 2.0
  use_anchor_only: true
  max_hedge_orders: 10
  emergency_penetrate_ticks: 3
  
  # 移仓日 fallback：走旧 flatten
  force_flatten_dates: []   # 如 ["20260615", "20260915"]
```

---

## 6. 改动地图（LOC 估算）

| 模块 | 改动 | LOC | 阶段 |
|---|---|---|---|
| `FutuRiskMonitor.{h,cpp}` | 删硬 BLOCK；PreTradeResult 加 utilization+obligation；ClosingPhase 状态机 | 130 | P1+P3 |
| `FutuQuoter.{h,cpp}` | qty 指数衰减 + obligation 报价 + freeze 模式 | 90 | P1 |
| `FutuPortfolio.cpp` | 已有 anchor 对冲，扩展返回详细 HedgeAction | 30 | P2 |
| `UftFutuMmStrategy.cpp` | 删 doCloseout 旧逻辑；接入 executeAnchorHedge | 100 | P3 |
| `StrategyCoordinator.cpp` | 双维 skew 加权；ClosingPhase 调度 | 60 | P1+P3 |
| `configbt.yaml` + 解析 | 三段新配置 + obligation/anchor_hedge/closeout | 40 | P1+P2+P3 |
| **合计** | | **~450** | |

---

## 7. 分阶段提交

### Phase 1（220 LOC）：单合约软化 + 双维 skew
1. FutuRiskMonitor::PreTradeResult 扩展
2. checkPreTradePosition 删 block，改返回 utilization + obligation
3. FutuQuoter qty 衰减 + obligation 报价（L0 强制 10手/10ticks）
4. StrategyCoordinator 双维 skew 公式（portfolio 0.5 + contract 1.0）
5. configbt.yaml: contract_position_soft_cap / qty_decay_factor / skew_weights / obligation
6. 回测验证：检查持仓打满后 obligation 报价是否出现，util≥1 后加仓侧 qty 是否归零

### Phase 2（90 LOC）：常态主力对冲
1. FutuPortfolio::computeHedge 复用
2. StrategyCoordinator 增加 portfolio_delta_util 监控
3. anchor_hedge 触发链 → OrderRouter HEDGE 优先级
4. configbt.yaml: anchor_hedge 段
5. 回测验证：portfolio_delta_util>0.8 时 anchor 对冲单是否触发

### Phase 3（140 LOC）：收盘前停报 + 主力对冲
1. ClosingPhase 状态机（替代旧 CloseoutState）
2. FutuQuoter::freeze/unfreeze
3. executeAnchorHedge 实现（hedge_only 模式）
4. UftFutuMmStrategy 删旧 doCloseout
5. force_flatten_dates 移仓日 fallback
6. 回测验证：T-15 停报、T-10 限价对冲、T-3 市价转换、|net_delta|→0

---

## 8. 验证基线

### 回测前后对比指标

| 指标 | Baseline（v0 / 当前） | v3 目标 |
|---|---|---|
| 总成交笔数 | 399 | ≥ 400（不降） |
| 收盘剩裸仓 | ao2607 +79 手 | \|net_delta\| ≤ 2 |
| 最大回撤 | 1340 | ≤ 1500（允许小幅波动） |
| 净 PnL | -5747 | ≥ -5500（不显著恶化） |
| POSITION_BREACH 次数 | 0（v0算法修复后） | 0 |
| 持仓打满后无成交时长 | 11:24 后死寂 ~3h | < 5min（obligation 应能成交） |
| 收盘前对冲滑点 | N/A | 单笔 ≤ 5 ticks |

### 关键观测点（埋日志）

```cpp
// 每分钟 dump 一次（便于事后分析）
WTSLogger::info("[MM_STATE] {} pos={} long_util={:.2f} short_util={:.2f} "
                "force_ask_oblig={} force_bid_oblig={} skew_p={:.2f} skew_c={:.2f}",
    code, cs->position, long_util, short_util,
    force_ask_oblig, force_bid_oblig, portfolio_skew, contract_skew);

// closing phase 转换
WTSLogger::warn("[CLOSING] {} → {} (minutes_to_close={}, net_delta={:.2f})",
    phaseToStr(old), phaseToStr(new), minutes_to_close, net_delta);
```

---

## 9. 风险与权衡（v3 精简版）

| 风险 | 严重度 | 缓解 |
|---|---|---|
| 取消硬 BLOCK 后失控加仓 | 高 | qty_decay_factor=2.0；util=1 强制 0；obligation 仅 ask 单侧（不会两侧加仓） |
| contract skew 权重 1.0 仍偏激，正常做市被频繁偏移 | 中 | util<0.3 时 skew<0.3 ticks 几乎无感；只在 util>0.5 时明显起效 |
| 主力对冲后主力自身 contract_delta 累积 | 中 | 主力 contract_max_delta 设大；hedge 单走 HEDGE 优先级即使触发软衰减也能成交 |
| QUOTE_STOPPED 太早损失 MM 收益 15min | 低 | 可配；起步保守，回测稳定后可调小到 10min |
| 移仓日 leg 必须切但 hedge_only 不切 | 高 | `force_flatten_dates` 配置移仓日列表，走旧 flatten 路径 |
| 信任 delta skew 自然防 spread，万一失灵 | 中 | Phase 1+2 部署后加观测埋点：日志每分钟记录各合约 pos 和跨月配对量，回测/实盘验证 1-2 周；若发现配对量异常增长再加 spread 监控 |
| 收盘前 anchor 流动性突然枯竭 | 中 | EMERGENCY_HEDGING 用 penetrate_ticks=3 穿透；T-0 仍未平 → FAILED + alert，人工兜底 |
| net_delta_tolerance=2 太严，反复发对冲单 | 低 | 容忍带 ±2 手；max_hedge_orders=10 上限保护 |

---

## 10. 决策记录

| 决策 | 选择 | 理由 |
|---|---|---|
| Q1 useBilateralQuote | 实盘也关 | UftMocker 不支持；新设计不依赖此路径 |
| Q2 obligation 参数 | 10 手 / 10 ticks | 用户拍板（合规义务） |
| Q3 obligation 范围 | 仅 L0 | 简单实用，多档加总复杂度高 |
| spread VaR 监控 | 砍掉 | 用户信任 delta skew 自然抑制 |
| Q13 时间窗 | 15/10/3 | 限价对冲 10min + 市价 3min + 缓冲 2min |
| Q14 模式 | hedge_only | 保留 leg 次日继续 MM，最大化连续做市收益 |
| Q15 优先级 | HEDGE / CLOSEOUT | 沿用现有 OrderRouter 优先级体系 |
| Q17 skew 权重 | 0.5 / 1.0 温和 | 起步保守，避免过激偏移导致正常做市丢单 |

---

## 11. 移仓日处理（额外说明）

`hedge_only` 模式的隐含假设：leg 可以过夜继续做市。但 **合约到期前**必须强制清空。

**`force_flatten_dates` 配置语义**:
```yaml
closeout:
  mode: hedge_only
  force_flatten_dates:
    - "20260714"   # ao2607 最后交易日前 1 天
    - "20260914"   # ao2609 最后交易日前 1 天
```

**行为**：在 `force_flatten_dates` 列表中的日期，`closeout` 状态机改走旧 flatten 路径（各合约 `stra_sell` 平自己），不走 hedge_only。

**运维要求**：移仓日列表需在合约换月前手动维护，建议同步到 `commodities.json` 的换月日历。

---

## 12. 后续可能的扩展（不在 v3 范围）

- spread VaR 监控（如观测埋点发现配对失衡）
- 自动主力切换（OI 监控 + 滑动窗口）
- hedge_then_close 模式（次日开盘自动平 hedge+leg）
- 跨品种对冲（如 ao 用 al 对冲）

---

**END of v3 设计文档**