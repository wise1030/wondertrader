# WtFutuCore 深度分析报告 V6

> **分析日期**: 2026-07-20  
> **分析范围**: 全量代码（98 个文件，约 30,000 行 C++17）  
> **分析维度**: 架构 / 业务逻辑 / 代码优化 / 性能提升  
> **前序报告**: DEEP_ANALYSIS_V5.md（已修复的问题不再重复）

---

## 目录

- [一、项目架构分析](#一项目架构分析)
- [二、业务逻辑分析](#二业务逻辑分析)
- [三、代码优化分析](#三代码优化分析)
- [四、性能提升分析](#四性能提升分析)
- [五、问题汇总与优先级](#五问题汇总与优先级)

---

## 一、项目架构分析

### 1.1 整体架构评价

WtFutuCore 采用**分层编排 + 策略插件**架构，经过多轮迭代已形成清晰的模块边界：

```
┌─────────────────────────────────────────────────────────┐
│                    UftFutuMmStrategy                     │ ← UFT 回调入口
│                   (策略主体 / 2223 行)                    │
├──────────────┬──────────────┬───────────┬───────────────┤
│  Coordinator │  Portfolio   │   Risk    │   Quoter      │
│  (编排器)     │  (组合管理)   │  Monitor  │  (报价器)      │
│   1482 行     │   806 行     │  1780 行  │   1005 行     │
├──────────────┼──────────────┼───────────┼───────────────┤
│  SpreadArb   │  ArbBridge   │ AsyncArb  │  OrderRouter  │
│  Manager     │  (执行桥)     │  Executor │  (订单路由)    │
│   1935 行     │   490 行     │  1044 行  │   648 行      │
├──────────────┴──────────────┴───────────┴───────────────┤
│         基础设施层 (LockFreeQueue / SpinLock / etc)       │
└─────────────────────────────────────────────────────────┘
```

**架构亮点**：
- 分层清晰：Portfolio=数据源, RiskMonitor=风控, Coordinator=编排, Strategy=入口
- 策略插件化：`ISpreadStrategy` 接口支持 4 种套利策略热插拔
- 信号源可组合：`ISignalSource` + `SignalAggregator` 实现 6 种 Alpha 信号融合
- 零堆分配热路径：`_violations_buf` 复用、LockFreeRingBuffer 固定容量

### 1.2 架构问题

#### A1. 双状态系统同步负担 [中危-架构]

**现状**：`TradingState`（单线程写入，非原子 enum）与 `FutuRiskMonitor` 的 4 个 `std::atomic` 标志（`_trading_halted`/`_quoting_paused`/`_long_blocked`/`_short_blocked`）是**两套并行状态**。

**问题**：恢复时需多处手动同步，极易遗漏：
- `StrategyCoordinator::checkRisk` (cpp:919-926)
- `UftFutuMmStrategy::on_trade` (cpp:1653-1659)
- `UftFutuMmStrategy::on_channel_ready` (cpp:1845)

`on_trade` 恢复时清 TradingState + RiskMonitor atomic + `_blocked_contracts.clear()`，但 Coordinator 的 `_risk_spread_mult` 未在此处重置（只在 Coordinator checkRisk:929 重置）。恢复后 spread_mult 仍被放大。

**建议**：引入 `TradingStateGuard` RAII 封装，统一状态转换接口：
```cpp
class TradingStateGuard {
    void enterRiskHalted(RiskReason reason);
    void resumeAll();  // 原子地清所有标志 + spread_mult + blocked_contracts
};
```

#### A2. 异步套利线程的线程安全边界模糊 [高危-架构]

**现状**：`AsyncArbitrageExecutor` 运行独立 arb 线程，通过 SPSC 队列与主线程通信。但 arb 线程通过裸指针访问多个主线程对象：

| 访问对象 | 访问位置 | 同步方式 |
|---------|---------|---------|
| `_portfolio_ptr->getTotalUnrealizedPnL()` | SpreadArbitrageManager.cpp:424 | **无锁** |
| `_portfolio_ptr->getTotalPnL()` | SpreadArbitrageManager.cpp:425 | **无锁** |
| `_risk_manager->calculatePortfolioRisk()` | SpreadArbitrageManager.cpp:1143-1156 | **无锁** |
| `_mm_enhancer->calculateAdjustment()` | SpreadArbitrageManager.cpp:596 | **无锁** |

主线程同时通过 `markToMarket`/`updatePosition`/`addRealizedPnL` 修改 `_contracts` 元素，形成**确认的 data race**。

**建议**：
1. 短期：将 PnL 快照通过 SPSC 队列从主线程推送到 arb 线程（周期性快照）
2. 中期：`SpreadRiskManager` 内部加 spinlock，所有公开 API 持锁访问
3. 长期：引入 `PortfolioSnapshot` 不可变对象，主线程 publish，arb 线程 read-only 引用

#### A3. 配置校验分散且不完整 [中危-架构]

**现状**：配置校验分布在 3 处，且互相重复：
- `FutuConfigLoader.cpp:185-239` — 内联校验（部分参数）
- `FutuConfigValidator.h` — 集中校验工具（**从未被调用**，死代码）
- 各策略内部的隐式校验

**关键缺失**：
- `ci.max_position/max_delta/target_position` 默认 -1.0（未配置），运行时无"必须填充"断言 → 下游用 -1.0 做持仓限制 → **风控失效**
- `ci.tick_size` 默认 -1.0，若基础数据模块未填充，除零风险
- `cooldown_ms/max_trades_per_sec` 等流控参数无范围校验

**建议**：启用 `FutuConfigValidator`，在 `init()` 末尾强制调用，校验失败拒绝启动。

#### A4. 组件工厂未兑现配置驱动承诺 [低危-架构]

`FutuComponentFactory::createPerformanceAnalyzer` (cpp:98-107) 完全忽略 config 参数，返回默认配置。`createAsyncArbitrageExecutor` (cpp:137-147) 同样硬编码 `enabled=true`，不读 yaml。

**建议**：统一所有 create 方法从 yaml 读取配置，或显式文档标注"使用默认配置"。

---

## 二、业务逻辑分析

### 2.1 风控模块

#### B1. FLATTEN_POSITION 在默认配置下不可达 [高危-逻辑]

**位置**：`FutuRiskMonitor.cpp:388-397`

`determineActionWithCategory` 要求 `breachCount >= flatten_threshold`（默认 3）才返回 FLATTEN_POSITION。但 `checkRiskLimits` 最多产生 2 个 BREACH（1 个 EXPOSURE + 1 个 POSITION_NET），所以 `breachCount` 永远 ≤ 2 < 3，**FLATTEN_POSITION 永远不可达**。

**影响**：最严厉的风控动作（强平）形同虚设。

**修复方案**：
- 方案 A：将 `flatten_threshold` 默认改为 2
- 方案 B：让 `getPositionBreachedContract` 返回所有违规合约（而非首个），每个产生独立 violation

#### B2. WIDEN_SPREAD 倍数注释与代码严重不一致 [中危-文档]

**实际代码**：L1→×1.2，L2→×1.5（StrategyCoordinator.cpp:741, 854）

**5 处注释都说** L1→×1.5、L2→×2.0：
- FutuRiskMonitor.h:91, 115-116
- FutuRiskMonitor.cpp:295, 297
- StrategyCoordinator.cpp:848

**影响**：误导运维调参和审计。

#### B3. Delta Rate 监控双恢复路径冲突 [中危-逻辑]

`checkRiskLimits` (cpp:127-128) 内部调 `checkAndHandleDeltaRateBreach`，该函数直接 `pauseQuoting()`/`resumeQuoting()`。同时 Coordinator `checkRisk` (cpp:724-729) 又单独检查并设 RISK_HALTED。

**两条恢复路径并发作用**：delta rate breach 时 RiskMonitor 内部 pauseQuoting（设 `_quoting_paused`），Coordinator 又 setQuotingPhase(RISK_HALTED)。恢复时 RiskMonitor 只清 `_quoting_paused`，但 qphase 仍是 RISK_HALTED → **状态卡死或闪烁**。

### 2.2 套利模块

#### B4. Welford 统计是累积而非滚动 [高危-逻辑]

**位置**：`SpreadCalculator.cpp:136-216`

`_welford_m`/`_welford_s`/`_welford_n` **永远累积**，不随 RingBuffer 滚动。文档声称 "Rolling statistics"，实际是累积统计。

**影响**：对非平稳价差序列（趋势漂移），Z-score 被陈旧样本稀释，**信号失效**。运行时间越长，统计越迟钝。

**修复方案**：实现滚动 Welford 变种（维护移出样本修正项），或改用衰减加权（EWMA）统计。

#### B5. 残腿对冲上限使用被拒腿数量 [高危-逻辑]

**位置**：`ArbExecutionBridge.cpp:166-167, 178-179`

```cpp
_deps.order_router->cancelByPair(ctx, order.pair_id);
markLegRejected(order.pair_id, order.qty);  // order.qty = 被拒腿数量
```

若 leg2=BUY 5 手被流控拒，leg1=SELL 5 手已成交 3 手。`markLegRejected(pair, 5)` 用 5 作为对冲上限，但实际只需对冲 leg1 的 3 手。**过度对冲 2 手**。

**修复**：从 OrderRouter/UnifiedOrderTracker 查询已成交量作为对冲上限。

#### B6. Orphan 队列满时静默丢弃 [高危-逻辑]

**位置**：`AsyncArbitrageExecutor.cpp:541-546`

```cpp
_orphan_legs_from_arb.tryPush({...});
WTSLogger::warn("...");
```

若 64 深队列已满，tryPush 失败但**未检查返回值**。该 orphan leg 永不被对冲 → **裸腿风险敞口**。

**修复**：队列满时同步调 `onArbSignalDropped` 让 arb 重新发对冲信号，或扩容队列 + 告警升级。

#### B7. Close 信号低 confidence 不释放 in_flight [中危-逻辑]

**位置**：`AsyncArbitrageExecutor.cpp:294-298`

只对 OPEN 类信号调 `onArbSignalDropped`。若 STOP_LOSS/TIMEOUT 信号 confidence < 0.5（applyB3Gate 已设 close_in_flight），不释放 → **卡 5s 超时**。

#### B8. 自成交检查只检查首个匹配订单 [中危-逻辑]

**位置**：`AsyncArbitrageExecutor.cpp:366-378`

`break` 后不再检查其它订单。若 MM 有多档卖单梯度，调整后价格可能仍 <= 另一个卖单 → **自成交未完全规避**。

**修复**：循环检查直到无冲突。

### 2.3 做市模块

#### B9. SpreadOptimizer EMA 状态在 const 方法无锁修改 [中危-线程安全]

**位置**：`SpreadOptimizer.cpp:63-90`

`_smoothed_spread_mult` / `_last_output_spread_mult` 是 `mutable` 成员，在 `const computeOptimalQuote` 中被修改，**不加锁**。若同一 SpreadOptimizer 实例被多线程调用（未来扩展），数据竞争。

且 `_last_output_spread_mult < 0.5` 作为"首 tick 门"判断，若 smoothed_spread_mult 因极端参数掉到 <0.5，会反复触发首 tick 初始化，**绕过变化率限制**。

#### B10. MarketMakingEnhancer suppress 语义错配 [中危-逻辑]

**位置**：`MarketMakingEnhancer.cpp:158-165`

```cpp
if (std::abs(_quoting_state.bid_skew) < 0.05) _quoting_state.suppress_bid = false;
```

suppress 由 z-score 触发（calculateAdjustment:129），但衰减时按 skew 大小清 suppress。z-score 持续高位但 skew 衰减变小 → suppress 被错误解除。

### 2.4 策略插件

#### B11. MeanReversionStrategy 加仓覆盖平仓信号 [高危-逻辑]

**位置**：`MeanReversionStrategy.cpp:87-124`

当 spread_position>0 且 zscore 满足 CLOSE_LONG 条件时，`signal.type` 被设为 CLOSE_LONG。紧接着的加仓块（L107-115，无 `else if`）若条件满足会**覆盖为 OPEN_LONG_SPREAD**。

**在同一 tick 内从 CLOSE 切到 ADD，违反状态机语义**。

#### B12. TrendFollowingStrategy entry_price 永不赋值 [高危-逻辑]

**位置**：`TrendFollowingStrategy.cpp:191-208`

`_trend_state.entry_price` 在策略内部从未被赋值（全文件无赋值语句），但用于判断止损。`entry_price > 0` 永远为 false → **止损分支永远不触发**。

#### B13. StatisticalArbStrategy 自适应权重学习失效 [中危-逻辑]

**位置**：`StatisticalArbStrategy.cpp:197, 226`

```cpp
scaled_pnl = pnl / (base_qty + 1);  // pnl 单位是货币，如 10000
weight += 0.1 * scaled_pnl * features.zscore;  // → 0.1 * 10000 * zscore
```

一次更新就让 weight 远超 0.50 上限 → **实际只有上限钳制在生效，自适应学习无效**。

**修复**：对 scaled_pnl 做 tanh 或 sign 归一化。

### 2.5 时间戳处理

#### B14. 全项目级时间戳格式不一致 [高危-系统性]

项目中存在 3 种时间表示，各模块直接用减法比较跨格式时间，在边界条件下全部失败：

| 格式 | 来源 | 受影响位置 |
|------|------|-----------|
| **Epoch ms** | `TimeUtils::getLocalTimeNow()` | 部分模块 |
| **HHMMSSmmm** | `tick->actiontime()` | TickTransactionInferer:405, MarketDataContext:198,258 |
| **压缩日期+时间** | `tick->actiondate()*1e9+actiontime()` | UftFutuMmStrategy:1267 |

**具体 Bug**：
- `SelfTradeCalibrator::timestampToMs` (cpp:11-20) — 解析公式**完全错误**，用 `ts/1000000` 提取"日期"实际是 HHM
- `TickTransactionInferer::pruneHistory` (cpp:403-405) — `HHMMSSmmm - 5000` 跨秒/分边界产生非法时间戳
- `MarketDataContext::onTickInference/onTransaction` (cpp:196,256) — 同上
- `SelfTradeCalibrator::analyzeFills/pruneHistory` (cpp:210,314) — HHMMSSmmm 与 ms 阈值直接比较

**建议**：引入统一 `Timestamp` 类型，在数据入口处转换为 epoch ns，禁止跨格式算术。

### 2.6 信号源

#### B15. SignalAggregator reset() 严重不完整 [高危-逻辑]

**位置**：`SignalAggregator.h:214-218`

`reset()` 未重置 `_prev_alpha`、`_tick_counter`、`_mid_history_for_ic`、`_mid_ma_short/long`、`_weight_framework`、`_dynamic_weights` 等关键状态。

**session 切换后**：IC 跟踪器包含上个 session 的回报、scale_trackers 还是旧分布 → **alpha 系统性偏差**。

#### B16. TradeFlowSignalSource reset() 不清交易历史 [中危-逻辑]

**位置**：`TradeFlowSignalSource.h:110-119`

`reset()` 仅重置累加器，不清空 `_trade_history`。下次启动时累加器为 0，但 `_trade_history` 仍有旧数据，`_net_flow -= old_signed` 变成负值。

#### B17. LeadLagSignalSource 成员未初始化 [中危-UB]

**位置**：`LeadLagSignalSource.h:62-67`

`_current_mid` 和 `_current_timestamp` 没有在构造函数初始化列表中初始化，首 tick 使用未定义值 → **未定义行为**。

---

## 三、代码优化分析

### 3.1 无锁数据结构

#### C1. LockFreeQueue::pushOverwrite 完全损坏 [严重-Bug]

**位置**：`LockFreeQueue.hpp:116-135`

`pushOverwrite` 在满时把 `_tail` 推到 `_head`，**破坏了 SPSC 队列"head==tail 即空"的核心不变量**。

**推演**（Capacity=4）：
1. 队列满：`head=0, tail=3`
2. `pushOverwrite(D)` → 写 buffer[3]=D, `_tail.store(0)`
3. 结果：`head=0, tail=0` → 消费者认为空 → **D 永久丢失，队列卡死**
4. 后续 `tryPush(E)` 覆盖 A，B/C/D 全部丢失且乱序

`_drop_count` 机制无法修复：消费者看到空队列就跳过 0 个元素。

**修复方案**：
- 重新设计 pushOverwrite：覆盖最旧元素（推进 head），而非破坏 tail
- 或直接移除 pushOverwrite，改用 tryPush + 调用方处理失败

#### C2. PerformanceMonitor "lock-free" 声名与实现不符 [中危]

**位置**：`PerformanceMonitor.h:125`

注释声称 "lock-free counters"，但：
- `_tick_to_quote_history` 等 RingBuffer **非线程安全**（RingBuffer.hpp:9 明确标注）
- `_throughput.ticks_processed` 等 **非原子** uint64_t

若 `recordTickToQuote` 从生产者线程、`getLatencyStats` 从监控线程调用，数据竞争。

#### C3. SpinLockGuard 缺少退避策略 [低危-性能]

**位置**：`SpinLockGuard.h:15-21`

仅用 `_mm_pause()`，无渐进式退避。高争用下浪费 CPU。

**建议**：实现三级退避：pause×N → yield → sleep(1µs)

### 3.2 死代码与冗余

#### C4. _portfolio_ctx_dirty 死代码 [低危-维护]

**位置**：`UftFutuMmStrategy.h:419-420` + `StrategyCoordinator.h:271`

`_portfolio_ctx_dirty` 在 cpp 中 3 处设 true，**全文从未读取**。这套"P0-2.3 lazy update 缓存"机制从未接线。

同样，`_global_portfolio_ctx.related` 每 tick `clear()` 但从不填充，`_portfolio_ctx_dirty` 从未被检查跳过重建。

#### C5. _closeout_pending_ids 永远为空 [低危-死代码]

**位置**：`CloseoutOrchestrator.cpp:83`

只有 `erase` 和 `clear`，**没有 insert**。集合永远为空，实际识别完全依赖 `OrderRouter::isOrderFromSource`。

#### C6. SpreadArbitrageManager::combineSignals 无调用者 [低危-死代码]

**位置**：`SpreadArbitrageManager.cpp:1178-1252`

搜索显示该函数定义但无任何调用。可清理。

#### C7. FutuConfigValidator 整个类未被调用 [低危-死代码]

`FutuConfigValidator` 提供集中校验，但**在代码中没有任何地方被调用**。

#### C8. SpreadRiskManager::liquidity_score 恒为 0 [中危-占位代码]

**位置**：`SpreadRiskManager.cpp:154, 202`

```cpp
double liquidity_sum = 0;  // 从未累加
summary.liquidity_score = liquidity_sum / std::max(1u, summary.active_pairs);
```

下游若依赖 liquidity_score 做决策（如 RiskAlert::LIQUIDITY_LOW 永不触发），逻辑失效。

#### C9. PerformanceAnalyzer::determineMarketCondition 桩函数 [低危]

**位置**：`PerformanceAnalyzer.cpp:324-331`

永远返回 `NORMAL`。`_condition_perf` 除 NORMAL 外的所有桶永远为空。

### 3.3 类型安全

#### C10. getPositionReductionToLimit 返回 double 被截断为 int32_t [高危-数据截断]

**位置**：`UftFutuMmStrategy.cpp:1870`

```cpp
int32_t reduction = _portfolio->getPositionReductionToLimit(*breached);
```

`getPositionReductionToLimit` 返回 `double`（注释明确"避免 int32_t 截断"），调用方恰恰做了截断。若 reduction=0.5 截断为 0，`if (reduction != 0)` 跳过平仓，**超限持仓不被修正**。

### 3.4 注释与代码矛盾

#### C11. toxic_detected 注释描述旧 bug 行为 [中危-误导]

**位置**：`StrategyCoordinator.cpp:674-679`

```cpp
// toxic_detected每tick重算，不复位锁存
// 与should_pause相同的锁存: 只设true不复位false
// 导致toxic_detected一旦被设就永久锁死
mutable_sig_ctx.toxicity.toxic_detected = false;  // 实际代码明明复位了！
```

代码 `= false` 是正确的，但 3 行注释描述的是旧 bug 行为（永久锁死），完全相反。

#### C12. 函数局部 static 变量跨实例共享 [中危]

**位置**：`StrategyCoordinator.cpp:312`

```cpp
static uint64_t _last_perf_ms = 0;
```

所有 StrategyCoordinator 实例共享，多实例互相干扰。应改为成员变量。

类似：`UftFutuMmStrategy.cpp:1259` 的 `static uint64_t ll_dbg_counter = 0`。

---

## 四、性能提升分析

### 4.1 热路径优化

#### P1. 每 tick 全量遍历 _contracts 5-8 次 [中危-性能]

**现状**：以下函数均 O(N) 遍历 `_contracts`：
- `getTotalDelta()` / `getNetDelta()` / `getTotalExposure()` / `getTotalGrossExposure()`
- `getTotalUnrealizedPnL()` / `getTotalPnL()`

一次 `on_tick` 调用链：
```
updateMarketData → getTotalDelta + getTotalExposure (遍历×2)
checkRisk → getPortfolioDeltaUtilization → getNetDelta (遍历×1)
processQuoting → 组合检查 (遍历×1)
checkAndHedge → getTotalDelta (遍历×1)
```

**且 `_global_portfolio_ctx` 缓存形同虚设**（SC-4）：每 tick 无条件重建，`_portfolio_ctx_dirty` 从不被检查。

**优化方案**：
1. **增量维护组合指标**：在 `onTick`/`onPositionUpdate`/`onTrade` 时增量更新 `_cached_total_delta`/`_cached_total_exposure`/`_cached_pnl`，热路径直接读缓存
2. **启用 dirty flag**：仅当持仓/均价变化时标记 dirty，下次读取时按需重建

```cpp
class FutuPortfolio {
    mutable double _cached_total_delta = 0;
    mutable bool   _delta_dirty = true;
    
    double getTotalDelta() const {
        if (!_delta_dirty) return _cached_total_delta;
        _cached_total_delta = recompute();
        _delta_dirty = false;
        return _cached_total_delta;
    }
    void onPositionUpdate(...) { _delta_dirty = true; }
};
```

#### P2. 每 tick 3 次取 SpreadArbitrageManager spinlock [中危-性能]

**位置**：`StrategyCoordinator.cpp:1053, 1071, 1074`

`getArbCloseDirection` + `getAggregateZscore` + `getQuotingAdjustmentForLeg` 每 tick 3 次取 `_pair_states_spin`。

**且 MarketMakingEnhancer 块是 observe-only**（L1067-1069 注释）— 算了 adjustment 但不注入，**纯开销的计算**。

**优化方案**：
1. 将 3 次查询合并为 1 次 `getArbSnapshot(pair_id)` 批量返回
2. observe-only 的 MarketMakingEnhancer 调用移到低频路径（每 N tick 一次）

#### P3. checkAndHedge 每 tick 全量同步持仓 [中危-性能]

**位置**：`StrategyCoordinator.cpp:1242-1253`

每次都遍历 `_portfolio->getAllContracts()`，对每个合约调 `stra_get_local_position` + 比较 + 可能的 onPositionUpdate。**即使不需要对冲也做全量同步**。

**优化方案**：
1. 仅在 `needsHedging()` 为 true 时做同步
2. 或改为事件驱动（on_position 回调时同步，而非每 tick 轮询）

#### P4. SpreadRiskManager::updateAlerts 每 tick 全量重建 [中危-性能]

**位置**：`SpreadRiskManager.cpp:316-378`

每次 `updatePairState` 都 `_active_alerts.clear()` + 重新遍历所有 pair 生成 alerts。N=10 pairs × 3 alert 类型 = 30 个 push_back / tick。

**优化方案**：增量更新 — 仅对变化的 pair 重新评估 alerts。

#### P5. shouldCancelDueToRate O(n²) [中危-性能]

**位置**：`UnifiedOrderTracker.cpp:528-542`

`vector::erase` 在头部删除需移动后续所有元素。高撤单率场景下 `_cancel_timestamps` 可累积上千条，每次调用 O(n)，总体 O(n²)。

**优化方案**：改用 `std::deque` 或 remove-erase 模式。

#### P6. cancelByPair O(P×A) [低危-性能]

**位置**：`OrderRouter.cpp:228-272`

两层嵌套循环遍历 `_oid_to_pair` + `_active_orders`。

**优化方案**：增加 `pair_id → vector<localid>` 反向索引。

### 4.2 内存分配优化

#### P7. handleBilateralQuote 每 tick 堆分配 [低危-性能]

**位置**：`FutuQuoter.cpp:182, 190`

```cpp
bid_level.order_ids = {bidId};  // 每次构造新 vector（initializer_list）
```

**优化方案**：改为 `bid_level.order_ids.assign(&bidId, &bidId+1)` 或预分配 + push_back。

#### P8. PerformanceMonitor::calculateStats 分配 16K 向量 [低危-性能]

**位置**：`PerformanceMonitor.cpp:97-150`

每次 `getLatencyStats` 分配 16K 元素 vector 并排序。

**优化方案**：预分配复用 buffer + 增量统计（P2 维护而非全量排序）。

### 4.3 低延迟架构建议

#### P9. arbThreadFunc 自旋轮询优化 [中危-能耗]

**位置**：`AsyncArbitrageExecutor.cpp:188-265`

自适应自旋（pause/yield/sleep 三级），但低 tick 频率下空转耗 CPU。

**优化方案**：
- 高频场景：引入 `futex` 或 `std::condition_variable` + 超时混合模式
- 保持 SPSC 队列无锁设计，但空转超过阈值时切换到 CV 等待

#### P10. 缓存行伪共享检查

**已良好处理**：
- `LockFreeQueue` 的 `_head`/`_tail`/`_drop_count`/`_buffer` 各自 `alignas(64)`
- `SpinLockGuard` 使用 `atomic_flag`

**待优化**：
- `FutuRiskMonitor` 的 `_order_times`/`_cancel_times`/`_trade_times` 三个 RingBuffer 虽各自 64 字节对齐，但 `_delta_snapshots` 数组与 `_delta_snapshot_head` 可能共享缓存行
- `ContractState` 结构体（h:59-176）约 200+ 字节，vector 中相邻元素跨缓存行；热路径遍历时 prefetch 效率低

#### P11. 批量化 API 设计 [架构-扩展性]

**建议**：为高频调用路径引入批量接口：

```cpp
// 当前：每 tick 3 次锁
double dir = arb_mgr.getArbCloseDirection(code);
double zsc = arb_mgr.getAggregateZscore(code);
auto adj   = arb_mgr.getQuotingAdjustmentForLeg(code);

// 建议：1 次锁获取完整快照
ArbSnapshot snap = arb_mgr.getArbSnapshot(code);  
// 包含 direction + zscore + adjustment + close_intent
```

### 4.4 数值计算优化

#### P12. SpreadCalculator correlation/beta 计算优化

**位置**：`SpreadCalculator.cpp:218-341`

每 10 个新样本全扫描 256 历史，O(N) 每 10 tick。

**优化方案**：维护增量相关系数（在线回归算法），避免全量重算。

#### P13. pow 优化

**位置**：`FutuQuoter.h:275`

`_level_qtys` 预计算已覆盖正常路径，但 fallback 分支仍用 `pow`。应确保 fallback 路径不触发，或改用 `std::exp(std::log(...))`（略快）。

---

## 五、问题汇总与优先级

### P0 — 必须立即修复（数据正确性 / 资金安全）

| 编号 | 问题 | 位置 | 影响 |
|------|------|------|------|
| C1 | LockFreeQueue::pushOverwrite 破坏不变量 | LockFreeQueue.hpp:116 | 数据丢失+队列卡死 |
| B1 | FLATTEN_POSITION 死代码 | FutuRiskMonitor.cpp:388 | 强平风控失效 |
| C10 | double→int32_t 截断 | UftFutuMmStrategy.cpp:1870 | 超限持仓不被修正 |
| B4 | Welford 累积非滚动 | SpreadCalculator.cpp:136 | Z-score 信号失效 |
| B5 | 残腿对冲上限用错 | ArbExecutionBridge.cpp:166 | 过度对冲 |
| B6 | Orphan 队列满静默丢弃 | AsyncArbitrageExecutor.cpp:541 | 裸腿风险 |
| A2 | arb 线程读 portfolio data race | SpreadArbitrageManager.cpp:424 | UB |
| B14 | 时间戳解析完全错误 | SelfTradeCalibrator.cpp:11 | 冷却计算失效 |
| B11 | 加仓覆盖平仓信号 | MeanReversionStrategy.cpp:107 | 信号丢失 |
| B12 | entry_price 永不赋值 | TrendFollowingStrategy.cpp:191 | 止损失效 |

### P1 — 高优先级（逻辑正确性 / 线程安全）

| 编号 | 问题 | 位置 | 影响 |
|------|------|------|------|
| B7 | close 信号不释放 in_flight | AsyncArbitrageExecutor.cpp:294 | 卡 5s 超时 |
| B8 | 自成交检查不完整 | AsyncArbitrageExecutor.cpp:366 | 自成交风险 |
| B3 | delta rate 双恢复路径冲突 | FutuRiskMonitor.cpp:127 | 状态卡死 |
| B9 | SpreadOptimizer EMA 无锁 | SpreadOptimizer.cpp:63 | 数据竞争 |
| B15 | SignalAggregator reset 不完整 | SignalAggregator.h:214 | session 间状态泄漏 |
| B13 | 自适应权重学习失效 | StatisticalArbStrategy.cpp:197 | 功能无效 |
| B10 | suppress 语义错配 | MarketMakingEnhancer.cpp:158 | 误解除抑制 |
| C2 | PerformanceMonitor 非线程安全 | PerformanceMonitor.h:125 | 数据竞争 |
| A1 | 双状态系统同步负担 | 多处 | 恢复遗漏 |
| B16 | TradeFlow reset 不清历史 | TradeFlowSignalSource.h:110 | 负值 net_flow |
| B17 | LeadLag 成员未初始化 | LeadLagSignalSource.h:62 | UB |

### P2 — 中优先级（性能 / 文档 / 死代码）

| 编号 | 问题 | 位置 |
|------|------|------|
| P1 | 每 tick 遍历 _contracts 5-8 次 | FutuPortfolio |
| P2 | 每 tick 3 次 arb spinlock | StrategyCoordinator |
| P3 | checkAndHedge 每 tick 全量同步 | StrategyCoordinator |
| P4 | updateAlerts 每 tick 全量重建 | SpreadRiskManager |
| P5 | shouldCancelDueToRate O(n²) | UnifiedOrderTracker |
| B2 | WIDEN_SPREAD 倍数注释不一致 | 5 处 |
| C11 | toxic_detected 注释矛盾 | StrategyCoordinator |
| C12 | static 局部变量跨实例 | StrategyCoordinator:312 |
| C8 | liquidity_score 恒 0 | SpreadRiskManager |

### P3 — 低优先级（清理 / 优化）

| 编号 | 问题 | 位置 |
|------|------|------|
| C3-C9 | 各种死代码 | 多处 |
| C4 | _portfolio_ctx_dirty 死代码 | UftFutuMmStrategy |
| C5 | _closeout_pending_ids 死代码 | CloseoutOrchestrator |
| C6 | combineSignals 死代码 | SpreadArbitrageManager |
| C7 | FutuConfigValidator 未接线 | 全局 |
| P6-P13 | 各种性能优化 | 多处 |

---

## 附录：代码量统计

| 模块分类 | 文件数 | 总行数 | 占比 |
|---------|--------|--------|------|
| 策略主体 (UftFutuMmStrategy) | 2 | 2,682 | 9% |
| 协调器 (Coordinator) | 2 | 1,775 | 6% |
| 风控 (RiskMonitor) | 2 | 1,780 | 6% |
| 套利 (ArbManager + Bridge + Async) | 6 | 3,969 | 13% |
| 报价 (Quoter + Optimizer + Enhancer) | 6 | 1,642 | 5% |
| 组合 (Portfolio) | 2 | 806 | 3% |
| 信号源 (6 sources + Aggregator) | 8 | 1,887 | 6% |
| 策略插件 (4 strategies) | 8 | 1,800 | 6% |
| 基础设施 (LockFree/SpinLock/Tracker/Router) | 10 | 3,200 | 11% |
| 信号/毒性 (Toxicity/Inferer/Calibrator) | 8 | 2,200 | 7% |
| 平仓 (Closeout + Orchestrator) | 4 | 933 | 3% |
| 配置 (Config/Loader/Validator/Factory/HotParam) | 10 | 1,100 | 4% |
| 监控 (PerformanceMonitor/Analyzer) | 4 | 1,018 | 3% |
| 其它 | ~30 | 6,500 | 18% |
| **合计** | **~98** | **~30,000** | 100% |

---

> **注**: 本报告基于 2026-07-20 的代码快照。V5 报告中的问题（配置参数清理、风控修复、分级平仓等）已修复，不再重复。本报告聚焦于 V5 之后的新发现。
