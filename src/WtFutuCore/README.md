# WtFutuCore — 期货高频做市引擎

基于 WonderTrader UFT 框架的期货高频做市 + 跨期价差套利引擎，采用 GLFT+Alpha 信号架构。

## 项目概览

| 项目 | 说明 |
|------|------|
| 语言 | C++17 |
| 框架 | WonderTrader UFT (Ultra-Fast Trading) |
| 编译产物 | `libWtFutuCore.so` (动态策略库) |
| 源文件 | 113 个 .h/.cpp/.hpp (含拆分组件), 约 3.3 万行 |
| 命名空间 | `futu` |
| 工厂名 | `FutuStraFact.FutuMM` |

## 架构总览 (v7.7 - 含 V6/V7 深度分析修复 + 批次1-4 修复 + 5A 重构 + 并发精细化 + v7.7 诊断复核)

```
UftFutuMmStrategy (入口策略壳, 847行: 回调锁+转发+on_tick主循环+初始化编排)
├── FutuModuleAssembler     (模块装配+合约信息加载, 5A-3 拆出)
├── FutuRuntimeOps          (成交/订单/报单/通道/会话事件处理, 5A-3 拆出)
├── FutuConfigLoader        (配置解析+校验, 拆分组件)
├── FutuHotParamManager     (26热参数注册+分发, 拆分组件)
├── CloseoutOrchestrator    (收盘平仓编排, 拆分组件)
├── ArbExecutionBridge      (套利执行桥+残腿防护, 拆分组件)
├── StrategyCoordinator (做市流水线, 1228行)
│   ├── SessionPhaseManager  (会话阶段统一判定: 交易时段/休息窗口/closeout窗口, 5A-1)
│   ├── QuotePolicyChain     (报价决策链: RiskWiden→ArbCloseSync→Toxicity
│   │                         →LimitPrice→ColdStart→FillRetreat, 5A-2)
│   ├── FutuPortfolio        (组合/持仓/Delta/敞口/对冲; 分向成本簿+聚合缓存)
│   ├── FutuRiskMonitor      (风控状态机: 5级响应+EventNotifier告警+四道闸恢复+滑窗读侧剔除)
│   ├── ToxicFlowDetector    (毒性检测门面)
│   │   ├── PredictiveToxicity  (VPIN + OFI + Alpha, warmup 期 alpha 通道保留)
│   │   ├── RealizedToxicity    (自成交校准)
│   │   └── SyntheticSignalFusion (hasAnySource 门控, 无源时跳过)
│   ├── SignalAggregator     (per合约, 6源信号聚合, SignalSlot 表驱动)
│   │   ├── ICWeightTracker        (三层权重: 基础×市场状态×IC, cap 归一化后施加)
│   │   ├── OFISignalSource         (订单流不平衡)
│   │   ├── TradeFlowSignalSource   (交易流, 滑窗衰减)
│   │   ├── BookImbalanceSignalSource (簿不平衡)
│   │   ├── MomentumSignalSource    (动量 O(1) 增量 log 收益)
│   │   ├── LeadLagSignalSource     (跨合约领先滞后)
│   │   └── VolatilitySignalSource  (波动率, 辅助)
│   ├── SpreadOptimizer      (per合约, GLFT价差模型, seqlock 参数读取)
│   ├── FutuQuoter           (per合约, 多档双边报价)
│   ├── OrderRouter          (套利/对冲/平仓统一下单, <500ns/order)
│   ├── UnifiedOrderTracker  (订单状态单一真相源, original_qty 部分成交跟踪,
│   │                         REJECTED 终结态, pending 全源增量维护)
│   ├── RiskLiquidator       (统一强平/减仓原语: 对手价FAK+三级价格校验+qty clamp, P0-1)
│   ├── CorrelationManager   (跨合约相关性与beta, 预建索引)
│   ├── PerformanceMonitor   (无锁延迟/吞吐监控 + TscClock rdtsc 埋点)
│   └── SelfTradeCalibrator  (自成交校准)
├── SpreadArbitrageManager (跨期套利协调器)
│   ├── ISpreadStrategy 注册表 (插件化, 新增策略 1 行注册)
│   ├── SpreadCalculator / SpreadRiskManager
│   ├── MarketMakingEnhancer
│   └── 策略实例: MeanReversion / TrendFollowing / PairsTrading / StatisticalArb
├── AsyncArbitrageExecutor (独立线程, 无锁SPSC队列, 跨线程安全)
│   配置: useAsyncArbThread=true(实盘异步) / false(回测同步)
├── MonitorBridge          (WtMonSvr GUI 数据桥: stradata/funds.csv 落盘, 默认关)
├── SelfTradePrevention
├── BilateralQuoteStats / PerformanceAnalyzer
└── FutuConfigValidator (启动时配置校验)
```

## 核心架构决策

### 双路径下单 (Dual-Path Order Routing)

```
┌───────────── 做市路径 (零延迟) ─────────────┐
│  FutuQuoter → ctx API (stra_buy/sell/quote)  │
│  无中间检查, 内联多档报价, 直接下单           │
└──────────────────────────────────────────────┘

┌───────────── 非做市路径 (限速+审计) ──────────┐
│  OrderRouter → ctx API (with guards)          │
│  · 自成交防护 (对抗做市挂单)                   │
│  · 按源独立限速 (各源独立 RateCounter 桶):      │
│       ARBITRAGE / HEDGING / CLOSEOUT           │
│  · 无抢占式调度 (各源并行时按调用顺序执行;      │
│    closeout 在 RateCounter 用尽时仍会被限流)    │
│  · 延迟预算: < 500ns/order                    │
└──────────────────────────────────────────────┘
```

### 统一交易状态 (TradingState)

分层状态机 (HSM)，集中管理做市阶段 + 报价子状态：

```cpp
struct TradingState {
    MmPhase      phase;         // 做市阶段: QUOTING / CLOSEOUT
    QuotingPhase qphase;        // 报价子状态: NORMAL / RISK_HALTED / TOXICITY / MARKET / ERROR
    bool long_blocked;          // 禁止买入
    bool short_blocked;         // 禁止卖出

    // RISK_HALTED -> NORMAL 唯一合法出口 (防误恢复)
    void resumeFromRisk();
    bool setQuotingPhase(QuotingPhase q);  // canTransitionQuoting 校验
    bool tryResumeFrom(QuotingPhase expected); // 条件恢复 (防跨态闪烁)
};
```

> **v7.4 线程契约修正 (P0-2)**: 框架源码核实实盘回调**非单线程**——`on_tick` 系列=CTP MdSpi 线程、`on_trade/on_order/on_entrust`=CTP TdSpi 线程、`on_session_end`(盘中)=RtTicker 定时线程，均同步直达策略无队列（回测单线程无法暴露）。策略层 13 个回调入口由 `_cb_mtx`(recursive_mutex) 统一串行化；TradingState 的单线程 tid 断言经 `setExternalLocking(true)` 停用。
>
> **v7.6 复核与原子化**: 逐变量复核确认两线程共享集合达 15 类（"_exchange_time_ms 由 MdSpi 写/TdSpi 读"等），"行情线程管行情变量"假设不成立。TradingState 全字段 `std::atomic<Enum>`，`tryResumeFrom`→CAS、`setQuotingPhase`→read-check-CAS 循环（canTransition 校验不丢失）；多字段复合操作（reset）为逐字段 store，仅 session 安静期调用。详见 Phase 11。

> **V7 修复 (A1)**: 外部恢复路径 (on_trade / channel_ready) 现通过 `Coordinator::onExternalResumeFromRisk()` 同步重置软风控倍数（v7.5 起由 `RiskWidenPolicy` 持有），避免恢复后报价宽度被永久放大。

### 信号架构 (SignalAggregator + 三层权重框架)

插件式信号源 + 自适应权重框架，配置驱动启用/禁用：

```
alpha = Σ(dynamic_weight_i × normalized_signal_i) / Σ(dynamic_weights)
```

**三层权重模型** (ICWeightTracker.h):

```
最终权重 = 基础逻辑权重 × 市场状态调节 × 在线可信度调节

第一层: 基础逻辑权重 (静态, 按交易逻辑设定)
第二层: 市场状态调节 (波动率/趋势/流动性 regime)
  - OFI: 薄流动性 ×0.5, 深 ×1.5
  - TradeFlow: 高波动 ×1.3, 低 ×0.7
  - Momentum: 趋势 ×1.5, 震荡 ×0.5
  - LeadLag: 跨期品种 ×1.5
第三层: 在线可信度 (滚动 IC + 信号一致性)
  - floor=0.05 / cap=0.50, 不归零不独占
  - IC 低 ≠ 信号无效 (可能是参数不适配)
```

**信号幅度归一化** (RollingScaleTracker):
所有信号在加权前通过滚动 p95 归一化到可比范围，确保权重真正有效。

| 信号源 | 基础权重 | 说明 |
|--------|---------|------|
| OFI | 0.25 | 订单流不平衡 (Order Flow Imbalance) |
| TradeFlow | 0.20 | 净交易流方向 (统计显著性归一化) |
| BookImbalance | 0.20 | 订单簿买卖压力 |
| Momentum | 0.15 | 价格动量 EMA |
| LeadLag | 0.20 | 跨合约领先滞后预测 (scale_factor=3000, bps 缩放) |
| Volatility | 辅助 | 已实现波动率 + 分层 |

### LeadLag 跨合约数据流

```
Anchor合约Tick到达
  → UftFutuMmStrategy::on_tick()
  → 遍历所有非Anchor合约的SignalAggregator
  → SignalAggregator::updateLeadContract(code, mid, ts)
  → LeadLagSignalSource::updateLeadContract()
  → calculateSignal(): signal = tanh(Σ(correlation × mid_change × scale_factor) / Σ(correlation))
```

## 做市流水线 (StrategyCoordinator)

每个 tick 的处理流程（与 `StrategyCoordinator::processTick` 代码一致）：

```
processTick()
  0   processCloseout()      → closeout 状态机驱动 (SessionPhaseManager 窗口判定)
  0.5 processSectionBreak()  → 每节收盘前 N 秒撤单+暂停 (SessionPhaseManager)
  1   preCheck()             → 会话/市场状态/毒性/风控预检
  (组件指针一次性解析: aggregator/book/quoter/spread_opt, 消除重复哈希查找)
  2   updateMarketData()     → 更新 MarketDataContext + 组合聚合缓存
  3   updateSignals()        → SignalAggregator + ToxicFlowDetector (MM only)
  4   checkRisk()            → RiskCoordinator 评估, 执行风控动作
       └ BLOCK_SIDE / HALT 时仍执行 checkTakerReduce (减仓才能恢复) 后 return
  5   processAutoCancel()    → 过时/偏价挂单清理 (先撤旧单, 再报新单)
  5.5 checkTakerReduce()     → 合约util≥takerReduceThreshold(默认1.1)时FAK对手价减仓
  6   processQuoting()       → SpreadOptimizer(GLFT) → QuotePolicyChain
                               (RiskWiden→ArbCloseSync→Toxicity→LimitPrice→ColdStart→FillRetreat)
                               → FutuQuoter.refreshQuotes() (B1条件式重挂, v7.2黏性生效)
  8   updateAdaptiveParams() → 周期性参数微调 (当前为占位 no-op, use_adaptive_params 启动时告警)
```

> **流水线契约**: Stage 5 (撤旧单) 必须先于 Stage 6 (报新单) — 撤单腾出 RateCounter 配额并避免新旧单同价位竞争。checkTakerReduce 在 Stage 4 风控失败早返回路径与 Stage 5.5 主路径各调一次，靠 `_last_taker_reduce` 冷却去重。

成交回调 (on_trade, FutuRuntimeOps::processTradeFill): 分向簿记账 → arb桥 → scout识别(v7.2: 自由层成交撤同侧义务层)
→ 条件式重挂(回测cancelAll仅深度破坏时 / 生产requoteAfterFill)
→ tracker REJECTED/成交终结 → 恢复四道闸(checkAndRecover)

## 风控体系 (FutuRiskMonitor)

### v7.1 连续控制重设计 (2026-07-23)

**核心原则**: 仓位风险由**无状态连续控制**处理, 报价永在线(做市义务); 离散硬动作只保留给
真正的极端情况(日亏损/组合敞口)。仓位 breach **不再触发** BLOCK_SIDE/RISK_HALTED (PAUSE_QUOTING v7.3 已删除)。

**统一利用率口径**: `util = (|pos| + 同向pending) / maxPos` — skew/qty衰减/义务/taker 共用

| 层 | 触发条件 | 动作 | 类型 |
|----|---------|------|------|
| NORMAL | util < 0.8 | 正常报价 (skew + qty衰减连续调节) | 连续 |
| **WIDEN_SPREAD** | 组合delta util ≥ 0.8/0.9 | `spread_mult = 1.2/1.5` (每tick无状态重算, RiskWidenPolicy) | 策略(soft) |
| **skew 穿越授权** | util ≥ 1.0 | 减仓侧允许穿越 mid 最多 `skewCrossMaxTicks`(3), 主动减仓 | 连续 |
| **obligation reduce** | util ≥ 1.0 | 加仓侧=带宽极限价+min qty; 减仓侧=skew攻击性(clamp不覆写) | 连续 |
| **TAKER_REDUCE** | util ≥ 1.3 | FAK对手价平到 0.8×maxPos, 每合约30s限频 | 离散(主动吃单) |
| HALT_TRADING | DAILY_LOSS CRITICAL 或任意 CRITICAL | IRREVERSIBLE + RiskLiquidator 对手价FAK强平 + 全停 | 终极 |

> **v7.3 两头化收口**: PAUSE_QUOTING/FLATTEN_POSITION 已删除——数学不可达死分支
> (breachCount 恒 ≤1, flatten_threshold=2 永不可达; PAUSE 触发阈被 takerReduce 先行拦截)。
> 设计明确为"软连续控制 + 硬 HALT"两头化: 仓位风险全走连续链, 极端情况直落 HALT 强平。

**设计要点**:
- **无状态 = 无死锁**: 仓位调节是 util 的纯函数, 仓位降了 skew 自动缓和, 无需恢复状态机
  (旧设计: PAUSE→撤单→无成交→仓位永不降→永不恢复, 覆盖率1-9% 的结构性根因)
- **T4 clamp 修复**: obligation 减仓侧 `askPrice=min(skew价, 带宽上限)`, 保留 skew 攻击性;
  旧覆写把减仓侧钉死在 mid+10ticks(被动), 最需要减仓时最不积极
- **穿越权限**: GLFT skew 旧 clamp(±half_spread) 使减仓能力在 util=1.0 封顶;
  v7.1 扩展到 half_spread+3ticks, 配合归一化 skew (`util^1.5×gain`, 1.0=贴mid)
- BLOCK_SIDE 保留给 DELTA/EXPOSURE 组合级 breach (罕见); POSITION_NET 仅 alert 上报
- 配置: coordinator.yaml `takerReduceThreshold/TargetUtil/CooldownMs`,
  spreadOptimizer `inventorySkewGain/skewCrossMaxTicks`

### v7.1 配套修复 (2026-07-24, 回测验证通过)

| 修复 | 位置 | 说明 |
|---|---|---|
| **分向成本簿记账** | FutuPortfolio::onTradeFill + ContractState long/short_qty/avg | offset 标志驱动, 替代净额推断。旧净额均价在 MM+arb 共享净头寸时被 arb 腿污染 → daily_pnl 假阳性日亏(引擎真账+34k vs 内部账-3.7M) → IRREVERSIBLE halt 误杀。markToMarket/setReferencePrice/resetDailyPnl 同步分向化 |
| **closeout 窗口禁 arb** | UftFutuMmStrategy::on_tick step 6 | `phase != CLOSEOUT` 才喂 arb tick。旧逻辑 14:45 平仓后 arb 继续开新价差仓到 15:00, 重建 ~50 手 delta 过夜 |
| **成交后立即重挂** | StrategyCoordinator::requoteAfterFill | 单边成交侵蚀深度 < min_valid_qty → 立即撤剩余单按最近 tick 参数重挂, 恢复双边义务; 200ms 限频. 配置 `requoteAfterFillMinIntervalMs` |
| **日界自动清除 IRREVERSIBLE** | FutuRiskMonitor::resetDaily | `autoClearIrreversibleOnReset`(默认 false, 回测开), 模拟隔夜人工复核 |

**5日 EC 回测验证 (6/08-6/12)**:
- 仓位死锁消除: util 达 1.06 (proj 53/50) 仅触发 obligation reduce, 报价不停; TAKER_REDUCE 未触发(仓位被连续控制压在 1.3 以内)
- 日亏假阳性消除: 全 5 天无 LOSS_CRITICAL 误报, 5 次 QUOTING_PAUSED 均为 closeout 正常流程
- 引擎资金曲线: +444,940 (修复前同窗口 +34k 且 Day2-5 被误 halt)
- requoteAfterFill 触发 279 次

### v7.1 第二轮修复 (2026-07-24, 覆盖率根因治理)

覆盖率仍低(<1%) → 二轮深挖定位三个根因, 均在 WtFutuCore 内修复:

| 修复 | 位置 | 说明 |
|---|---|---|
| **双边统计以引擎确认为准** | FutuQuoter::onEntrustAck/onOrder; 删 refreshQuotes末/placement时/onTrade 的统计 | 报单"在场时间"从引擎确认(on_entrust)起算, 不含发出→确认的网络延迟 (建模延迟)。mocker 新单只有 on_entrust 异步(postTask), on_order 仅在成交/撤单/每tick匹配心跳触发; 每笔 on_trade 必伴随 on_order(leftQty) → onTrade 统计冗余已删 |
| **价格保护可配置化** | config `protectTicks`/`priceProtection` | protectTicks 语义=允许比盘口好的最大 tick 数("最大可以穿多少")。远月宽盘口(11-34t)合约须 ≥ obligationMaxSpreadTicks, 否则报价被钳在盘口边缘→双边价差超带宽(ec2609 实测 8%)。priceProtection:false 完全关闭 |
| **session 休息段暂停** | StrategyCoordinator::processSectionBreak + on_tick step6 门控 | 每节收盘前 N 分钟(`sectionBreakMinutesBefore`,默认1)撤全部报价+arb在途单+停报价/套利, 下一节首tick自动恢复。每日最后一节跳过(归 closeout)。注意 stra_get_time 返回 HHMM(4位) |
| **节流统一 replay 时钟** | 全策略决策路径(coordinator/risk/router/orchestrator/bridge/manager) | 回测中墙钟节流随机器速度漂移→订单序列不可复现。统一改 tick actiondate/actiontime 推出的 replay ms, 墙钟仅用于纯日志限频 |
| **诊断计数器** | BilateralQuoteStats 非双边原因分类 | formatString 输出 inv[both/bid/ask/cross/wide] 计数, 定位覆盖率失真根因 |

**5日 EC 回测验证 (6/08-6/12, protectTicks=10)**:
- ec2607/08 双边覆盖率: **75-84%** (vs 修复前 <1%)
- ec2609 双边覆盖率: **8% → 51-56%** (远月盘口宽, protectTicks=10 允许深入盘口)
- SECTION_BREAK 触发 20 次 (2 break/天 × 5天 × 2 合约档), 正常
- 仓位连续控制工作: util 1.06 → obligation reduce, 报价不停
- **Day3 真实日亏 halt** (-953k): 50手 maxDelta × 50乘数 = 2500元/点, 200k 日亏限 ≈ 80点反向; EC 日内常波动 80+点 → **日亏限对 50手 EC 结构性偏紧**, 属业务风控参数决策(非 bug)

**已知外部限制**: 回测仍有轻微不可复现 (WtBtCore/HftMocker.cpp `splitVolume()` 用 srand(time) 随机拆分成交 → 同配置两次运行成交序列有噪声), 评估策略表现需多次运行取均值。详见 `AGENTS.md`。

### 告警外发通道 (R1 接线)

风控告警通过 **EventNotifier**(nanomsg PUB/SUB)外发,供运维侧(钉钉/webhook 等)订阅:

```
FutuRiskMonitor.broadcastAlert (22 处) ──直达──→ EventNotifier.notify("RISK_TYPE", msg)
SpreadArbitrageManager._alert_callback    ──回调──→ UftFutuMmStrategy.handleRiskAlert
                                                         ↓
                                                  EventNotifier.notify("ARB_RISK", msg)
```

**注入链路**(方案 B 策略层转发,保持模块边界):
- `WtUftRunner.initUftStrategies` → `UftStraContext.setEventNotifier(&_notifier)`
- 策略 `on_init` → `dynamic_cast<UftStraContext*>` → `setEventNotifier(getEventNotifier())`
- `setEventNotifier` 内:RiskMonitor 直达 + ArbManager 回调转发(解耦)

**告警类型**:
- RiskMonitor: `EXPOSURE_BREACH` / `LOSS_CRITICAL` / `POSITION_BREACH` / `TRADING_HALTED` / `QUOTING_PAUSED` / `CLOSEOUT_*` / `DELTA_RATE_BREACH` 等 16 类
- SpreadArbitrageManager: `ARB_RISK` topic(含 CORRELATION_BREAK / DIVERGENCE / POSITION_LIMIT / STOP_LOSS / B5 OVERSHOOT)

**yaml 配置**(notifier 节点,已由 WtUftRunner 读取):
```yaml
notifier:
  active: true
  url: "ipc://risk_events.ipc"   # nanomsg PUB 端点 (运维侧 SUB connect)
```

### 自动恢复机制
- **仓位 breach** (v7.1): 无状态连续控制, 无需恢复 — 仓位回落 skew/qty 自动缓和
- **可逆风险** (Delta偏离、EXPOSURE breach、频率超限): 冷却后自动恢复
- **不可逆风险** (日亏损超限): 需人工干预(`clearIrreversible`)
- **BLOCK_SIDE 恢复**(R2.7, 仅 DELTA/EXPOSURE 触发): BLOCK_SIDE_LONG/SHORT 设 `qphase=RISK_HALTED`,走统一恢复路径(`canRecover` + `resumeFromRisk` + `unblockLong/Short`)


### 三层减仓机制 (Position Liquidation Architecture)

仓位风险管理采用三层防线, 从被动到主动到紧急, 覆盖从"启动遗留"到"运行累积"到"极端亏损"的全部场景:

| 维度 | ① AUTO REDUCE | ② TAKER_REDUCE | ③ FORCE FLAT |
|------|---------------|----------------|--------------|
| **调用者** | FutuRuntimeOps (onChannelReady) | StrategyCoordinator (onTick) | StrategyCoordinator (checkRisk) |
| **底层方法** | `RiskLiquidator::reduceContract` | 直接 `submitSell/Buy` | `RiskLiquidator::forceFlatAll` |
| **触发时机** | 策略启动 channel ready 后 (一次性) | 每 tick 持续检查 | 风控检测到 IRREVERSIBLE |
| **触发条件** | `\|pos\| > maxPosition` | `\|pos\|/maxPos >= takerReduceThreshold` (默认1.3) | 日内亏损超 maxDailyLoss |
| **减仓目标** | 减**到** maxPosition (如30) | 减**到** maxPos×targetUtil (默认0.8, 如24) | **全部清零** |
| **Cooldown** | 无 (失败等下笔成交重试) | 30s/合约 (takerReduceCooldownMs) | 无 (一次性) |
| **处理范围** | 一次只处理一个超限合约 (`break`) | 遍历所有超限合约 | 全部合约 |
| **可恢复** | 自动 (REVERSIBLE) | 自动 | 需人工干预 (clearIrreversible) |
| **场景** | 启动时发现遗留超限持仓 | 运行中持仓累积到130% | 日内亏损触顶, 紧急清仓 |

**调用关系**:
```
onChannelReady (启动)
  └─> checkRiskLimits -> POSITION_NET breach?
        └─> RiskLiquidator::reduceContract  ① AUTO REDUCE

onTick (每tick)
  └─> checkTakerReduce                       ② TAKER_REDUCE
  └─> checkRisk -> IRREVERSIBLE?
        └─> RiskLiquidator::forceFlatAll     ③ FORCE FLAT
```

**净持仓口径**: 三个机制均使用 `ContractState.position` (NET position, 正=多 负=空),
而非 gross 多/空分别检查。方向由 net 符号决定: `pos > 0 -> submitSell` (平多),
`pos < 0 -> submitBuy` (平空)。这意味着当 net 为正时, 空头侧不会被减仓机制触碰,
只能通过正常报价的 bid 成交自然消化。

**平仓类型 (v7.7 修复)**: 减仓订单统一使用 `submitSell`/`submitBuy` (走 `stra_sell`/`stra_buy`),
由框架 action policy 自动判断平仓类型 (WOT_CLOSE vs WOT_CLOSETODAY)。
旧实现使用 `submitExitLong`/`submitExitShort` (走 `stra_exit_long`/`stra_exit_short`) 并硬编码
`isToday=true`, 导致对昨日持仓发平今单被 CTP 拒绝 ("平今仓位不足")。
修复后框架根据 `commInfo->getCoverMode()` 自动选择: SHFE/INE/DCE 用 WOT_CLOSE (平仓),
CFFEX 用 WOT_CLOSETODAY (平今)。

### 收盘平仓状态机
```
IDLE → TRIGGERED → DRAINING → ASSESSING → EXECUTING → COMPLETED
                     ↘ FAILED → RETRYING ↗
```
- 窗口判定统一走 SessionPhaseManager (夜盘跨日映射 + 白盘双触发点)
- DRAINING 等活跃态下 checkRisk 复跑 (closeout 不再是风控盲区, 批次2)
- FAK 部分成交/自撤单不误判 FAILED (仅零成交拒单才 markFailed, 批次1)
- 触发源日志: CLOSEOUT_TRIGGERED/DRAINING/COMPLETED/FAILED

## 毒性检测 (ToxicFlowDetector)

门面模式组合三个子模块：

| 子模块 | 触发条件 | 输出 |
|--------|----------|------|
| PredictiveToxicity | 每tick | VPIN/OFI/Alpha预测毒性 |
| RealizedToxicity | 成交事件 | 自成交校准后的已实现毒性 |
| SyntheticSignalFusion | 每tick(内嵌) | 3源融合合成交易数据 |

融合权重: TickTransactionInferer=0.4, DepthImbalance=0.4, SelfTradeCalibration=0.2

## 套利子系统 (SpreadArbitrageManager)

### 支持的策略类型
- **MeanReversion**: 均值回归 (Z-score入场, 加仓安全间距0.75)
- **PairsTrading**: 配对交易 (协整p-value, MacKinnon近似)
- **TrendFollowing**: 趋势跟踪 (止损pct=2%, 最大趋势bar=50)
- **StatisticalArb**: 统计套利 (M-spread特征, volume imbalance)

### 异步执行
- `AsyncArbitrageExecutor`: 独立线程 + 无锁SPSC队列
- 两腿原子提交: req_id fetch_add(2), 连续ID分配
- 自成交检查: 对抗做市挂单快照

## 配置说明

### 配置文件结构

```
src/WtFutuCore/config/         # 配置示例 (参考部署用)
├── config.yaml              # 主策略配置 (身份+业务参数, 不含模块开关)
├── coordinator.yaml         # 模块开关(唯一权威)+模块参数
├── spread_arbitrage.yaml    # 跨期套利配置
└── hotparams.yaml           # 热更新参数(运行时可改)

dist/WtRunnerFutu/             # 实盘部署目录
├── config.yaml              # (同上, 含行情/交易通道配置)
├── coordinator.yaml
├── spread_arbitrage.yaml
├── hotparams.yaml
├── actpolicy.yaml             # 买卖策略
├── logcfg.yaml                # 日志配置
├── mdparsers.yaml             # 行情解析模块
├── tdtraders.yaml             # 交易模块
├── common/                    # 基础数据(合约/品种/节假)
├── uft/                       # UFT框架数据
│   └── libWtFutuCore.so       # 策略动态库
├── Logs/                      # 运行日志
└── generated/outputs/         # 策略输出
```

### 配置分层说明 (v5+ — 单一权威位置)

| 文件 | 职责 | 开关承载 |
|------|------|---------|
| config.yaml | 策略身份 + 业务参数 | **不承载任何开关** |
| coordinator.yaml | 模块开关 + 模块参数 | 策略级7开关(根级) + 模块级4开关(modules.&lt;name&gt;.enabled) |
| spread_arbitrage.yaml | 套利子系统 | pair配置/风控/子策略参数 |
| hotparams.yaml | 热更新参数 | 26个参数运行时生效(共享内存同步) |

> **单一权威原则**: 每个开关有且只有一个书写位置, 多处书写不会被合并/覆盖, 而是直接报错或被忽略。代码内 fallback 仅表示"键缺失时使用编译期默认值", 不构成第二处配置。

### config.yaml 关键配置项

> 权威示例见 `src/WtFutuCore/config/config.yaml`。注意: **config.yaml 不承载任何模块开关**(单一权威原则), 策略级/模块级开关统一在 coordinator.yaml。

```yaml
# 锚定合约(LeadLag信号的领先合约)
anchorCode: SHFE.ag.ag2608
isBacktest: false             # 回测模式 (on_trade 只撤不挂, 避免 _orders 迭代器失效; 生产=false)

# 合约列表
contracts:
  - code: SHFE.ag.ag2608
    maxPosition: 30
    maxDelta: 30
    targetPosition: 0
  - code: SHFE.ag.ag2612
    maxPosition: 30
    maxDelta: 30

# 报价参数 (FutuQuoter)
quoting:
  numLevels: 2               # 报价档位数 (1-10); 2 = L0自由探测层 + L1义务层
  obligationLevel: 1         # 义务层所在档位 (1=义务退居次优层, L0 为 scout 自由探测单)
  scoutQty: 1.0              # 自由探测层手数 (<义务层; 成交即撤同侧义务层)
  baseSpread: 2.0            # 基础价差 (tick 数, 权威来源)
  baseQty: 2.0               # 基础手数
  levelQtyMultiplier: 0.7    # 档位间数量几何衰减
  levelStep: 1.0             # 每档价格步长 (tick)
  stickyThreshold: 1.0       # 粘性更新阈值 (tick)
  improveRetreatRatio: 2.0   # 改进/退让比率
  maxPriceDeviation: 20.0    # 最大价格偏差 (tick)
  useBilateralQuote: false   # 是否使用双边报价接口
  priceProtection: true      # 价格保护开关 (false=完全不钳制)
  protectTicks: 1.0          # 允许比盘口好的最大 tick 数 (宽盘口合约应 >= obligationMaxSpreadTicks)
  qtyDecayFactor: 2.0        # 库存敏感的 qty 指数衰减因子
  obligationMinQty: 10.0     # 做市义务最小手数
  obligationMaxSpreadTicks: 10
  obligationOnlyL0: true
  alwaysObligation: true

# 组合参数 (FutuPortfolio)
portfolio:
  maxDelta: 30               # 组合最大 Delta (软指标)
  hedgeRatio: 1.0            # 对冲比率 (CloseoutExecutor 用)
  # hedgeDeltaThreshold/hedgeCooldownMs 已随做市阶段Hedge删除 (v7.2):
  # 仓位风险由连续控制链处理 (WIDEN→skew→takerReduce→PAUSE→HALT)

# 风控参数 (FutuRiskMonitor)
risk:
  maxExposure: 20000000.0    # 最大敞口 (0=禁用)
  maxDailyLoss: -200000.0    # 日最大亏损 (正负均可, 内部取 abs)
  frequency:                 # 频率/恢复子节点
    maxOrdersPerSec: 50
    maxCancelsPerSec: 30
    maxTradesPerSec: 20
    cooldownMs: 30000
    checkIntervalMs: 5000
    recoveryThreshold: 0.8
    maxDeltaChangePerSec: 50.0
    deltaRateWindowSec: 10
    deltaRateCooldownMs: 5000
    maxRecoveryCount: 3
    pnlRecoveryRatio: 0.5
    maxLossForRecovery: 0
    positionBreachPauseThreshold: 1.2
    positionHardBlockRatio: 1.0
    deltaCriticalMult: 1.5
    deltaWarningMult: 0.8
    positionWarningL1: 0.8   # util≥0.8 → WIDEN_SPREAD ×1.2
    positionWarningL2: 0.9   # util≥0.9 → WIDEN_SPREAD ×1.5
    autoClearIrreversibleOnReset: false  # 日界自动清 IRREVERSIBLE (回测用, 生产必须 false)

# 收盘平仓 (CloseoutOrchestrator + CloseoutExecutor)
closeout:
  minutesBefore: 2           # 收盘前 N 分钟触发
  flattenPosition: true
  maxRetries: 10
  retryIntervalMs: 2000
  nightMinutesBefore: 2      # 夜盘收盘前 N 分钟
  drainTimeoutMs: 3000       # CloseoutExecutor 渐进式: 排空超时
  depthRatioPassive: 0.3     # 被动档深度比例
  depthRatioMid: 0.5         # 中间档深度比例
  depthRatioAggressive: 0.8  # 激进档深度比例
  sweepThresholdMs: 5000     # 扫单触发时间
  sweepTicks: 3              # 扫单穿透 tick 数
  useFak: true               # 使用 FAK 订单

# 性能监控
performance:
  latencyThreshold: 100000
  enabled: false
  logInterval: 1000
  warnThresholdNs: 10000
  criticalThresholdNs: 50000

# 下单控制
orderErrorThreshold: 3
maxOrders: 32
maxPendingPerSide: 30
stpMinPriceGap: 1.0
useStp: false                # 自成交防护 (useSpreadArbitrage=true 时强制 true)

# MonitorBridge (WtMonSvr GUI 数据桥, 默认关)
monitor:
  enabled: true
  flushIntervalMs: 1000

# 外部配置文件引用 (模块开关在此两文件)
coordinatorConfig: ./coordinator.yaml
spreadArbitrageConfig: ./spread_arbitrage.yaml
```

> 注: `closeTime` 字段已移除(收盘时间从合约 session 信息推导); `modules` 顶层节点**不存在**(单一权威原则, 模块开关见 coordinator.yaml)。

### coordinator.yaml 关键配置项

> 权威示例见 `src/WtFutuCore/config/coordinator.yaml`。**所有模块开关唯一权威位置**，全部嵌套在 `coordinator:` 根节点下。

```yaml
coordinator:
  # === 策略级开关 (根级) ===
  useMarketMaking: true
  useSpreadArbitrage: true
  useAsyncArbThread: false       # 实盘=独立arb线程低延迟, 回测必须 false(主线程同步可复现)
  usePerformanceMonitor: false
  usePerformanceAnalyzer: false
  use_signal_aggregator: true

  # === v7.1 仓位连续控制链 (PAUSE_QUOTING/FLATTEN_POSITION 死分支已删除) ===
  takerReduceThreshold: 1.1      # util≥此值触发 FAK 对手价减仓 (0=禁用)
  takerReduceTargetUtil: 0.8     # 减仓目标利用率
  takerReduceCooldownMs: 30000   # 每合约限频
  requoteAfterFillMinIntervalMs: 200   # 成交后立即重挂间隔 (0=禁用)
  sectionBreakSecondsBefore: 10  # 每节收盘前 N 秒撤单停报 (0=禁用)

  # 流水线参数
  pipeline:
    paramUpdateInterval: 100
    alphaSensitivity: 2.0

  # === 模块级开关 + 模块参数 (modules.<name>) ===
  modules:
    # 信号聚合器 (signals/model/volatility 三层分离)
    signalAggregator:
      signals:                   # presence = enabled
        ofi: { window: 50 }
        trade_flow: { window: 100, largeTradeThreshold: 50.0 }
        book_imbalance: { threshold: 0.2 }
        momentum: { window: 50, emaAlpha: 0.1 }
        lead_lag: { window: 50, lagMs: 50 }
      model:
        type: linear             # 仅支持 linear
        weights:                 # Layer 1 base 权重 (经 regime×IC 三层动态调节)
          ofi: 0.35
          trade_flow: 0.25
          book_imbalance: 0.20
          momentum: 0.15
          lead_lag: 0.05
        strongThreshold: 0.7
      volatility:                # 辅助信号 (不参与 alpha 加权, 驱动 should_widen/should_pause)
        window: 100
        elevatedThreshold: 0.002
        extremeThreshold: 0.004
      warmupTicks: 20

    # 毒性检测器 (注意: 键名为 adverseThreshold/vpinThreshold, 不是 toxicityThreshold)
    toxicityDetector:
      enabled: true
      adverseThreshold: 0.25     # 综合毒性触发阈值
      vpinThreshold: 0.30        # VPIN 单独触发阈值
      window: 20
      bucketSize: 50
      minWarmupBuckets: 5
      cooloffMs: 5000
      alphaWeight: 0.5
      bookWeight: 0.3
      selfTradeWeight: 0.4
      extremeSignalWeight: 0.8

    # GLFT 价差优化 (注意: baseSpread 不在此处, 从 config.yaml 的 quoting.baseSpread 加载)
    spreadOptimizer:
      enabled: true
      phi: 0.20
      deltaSkewThreshold: 0.3
      deltaSkewFactor: 1.5
      deltaSkewPower: 1.5
      maxSpreadMult: 3.0
      minSpreadMult: 1.0
      inventorySkewGain: 1.0
      inventorySkewScale: 2.0
      skewCrossMaxTicks: 3.0     # util≥1.0 时减仓侧可穿 mid 的最大 tick 数
      portfolioSkewWeight: 0.5
      contractSkewWeight: 1.0

    selfTradeCalibrator:
      toxicityWindowMs: 5000
      adverseThreshold: 0.6

    selfTradePrevention:
      enabled: true              # useSpreadArbitrage=true 时强制 true
      stpMinPriceGap: 1.0

    autoCancel:
      maxAgeMs: 10000
      priceDeviation: 3.0
      inventoryLimitCooldownMs: 2000

    correlationManager:
      windowSize: 100
      minCorrelation: 0.5

    adaptiveParam:
      enabled: false             # 占位, 未启用
      updateInterval: 100
```

### spread_arbitrage.yaml 关键配置项

> 权威示例见 `src/WtFutuCore/config/spread_arbitrage.yaml`。注意键名为 `entryZScore`/`exitZScore`/`stopLossZ`/`risk_limits`(snake_case), 不是 camelCase 的 entryZThreshold/riskLimits。

```yaml
spread_arbitrage:
  enabled: true
  enhanceMarketMaking: true         # 套利信号增强做市
  primaryStrategy: "mean_reversion" # 主策略: mean_reversion/trend_following/pairs_trading/statistical_arb
  maxTotalPosition: 20
  maxPairs: 3
  minSignalConfidence: 0.3
  signalCooldownMs: 2000
  minProfitThresholdTicks: 1.0      # 开仓最低利润门槛 (ticks)

  # === C0/C1/C2 分级平仓 (ARB_SELF_CLOSE_DESIGN v2.1; enabled=false 纯 B-3 兼容) ===
  arb_close:
    enabled: false
    allow_signals:
      close_long: false             # 永不解禁 (B-3 特性)
      close_short: false
      timeout_exit: false           # C2 阶段启用
      stop_loss: false              # C1 阶段启用 (最高优先级)
    stop_loss_policy: { order_flag: 1, timeout_ms: 1000 }       # FAK
    timeout_policy: { order_flag: 0, timeout_ms: 30000, upgrade_to_taker: true }  # GFD mid
    max_close_size_pct: 0.5
    oversold_protection: true       # B5: 过冲保险丝 (sign-flip 检测)
    overshoot_cooldown_ms: 3600000  # 触发后 pair 冷却 1h

  # 价差对配置 (注意: entryZScore/exitZScore/stopLossZ, 不是 ZThreshold)
  pairs:
    - id: "ag0812"
      leg1: "SHFE.ag.ag2608"
      leg2: "SHFE.ag.ag2612"
      ratio: 1.0
      ratio2: 1.0
      entryZScore: 2.0
      exitZScore: 0.5
      stopLossZ: 4.0
      stopLossPct: 0.02            # 趋势跟踪用
      maxTrendBars: 50
      addSafetyRatio: 0.75
      maxPosition: 20
      windowSize: 200

  # 套利风控 (SpreadRiskManager, 数据源=全组合 PnL)
  risk_limits:
    portfolioStopLoss: 50000.0     # 组合峰值回撤止损 (drawdown>此值 → EMERGENCY)
    maxTotalPosition: 50.0
    maxSinglePair: 20.0
    maxCorrelationBreak: 0.3
    maxDivergenceZscore: 5.0
    maxDivergenceTime: 7200

  # 统计子策略默认参数 (全局默认, per-pair 可覆盖)
  statistical:
    meanReversion:
      halfLife: 100
      entryZThreshold: 2.0
      exitZThreshold: 0.5
      stopLossZ: 3.0
      addSafetyRatio: 0.75
    pairsTrading:
      lookbackWindow: 100
      entryZThreshold: 2.0
      correlationWindow: 100
      minCorrelation: 0.7
    trendFollowing:
      stopLossPct: 0.02
      maxTrendBars: 50
      maPeriod: 20
      breakoutThreshold: 1.5
```

### hotparams.yaml (运行时热更新)

> 权威示例见 `src/WtFutuCore/config/hotparams.yaml`。**键名必须用 snake_case**(与 `FutuHotParamManager::registerParams` 的 `sync_param` 注册名完全一致), 否则共享内存写入**静默失效**。

```yaml
# --- 报价基础参数 (穿透到 FutuQuoter 和 Coordinator) ---
base_spread: 2.0
base_qty: 2.0
level_qty_multiplier: 0.7
level_step: 1.0

# --- 组合参数 ---
max_delta: 30

# --- Alpha 灵敏度 ---
alpha_sensitivity: 2.0

# --- 信号权重 (Layer 1 base, 经 regime×IC 三层调节) ---
ofi_weight: 0.35
trade_weight: 0.25
book_imbalance_weight: 0.20
momentum_weight: 0.15
lead_lag_weight: 0.05
strong_threshold: 0.7

# --- GLFT 价差模型参数 ---
confidence_weight_min: 0.3
confidence_weight_max: 1.0
phi: 0.20
delta_skew_threshold: 0.3
delta_skew_factor: 1.5
max_spread_mult: 3.0
min_spread_mult: 1.0
depth_sensitivity: 0.5
toxicity_spread_factor: 1.0
low_confidence_spread_factor: 2.0

# --- 报价粘性/保护参数 ---
sticky_threshold: 1.0
improve_retreat_ratio: 2.0
protect_ticks: 1.0
max_price_deviation: 20.0
```

热更新通过共享内存同步，`on_params_updated()` 回调生效，无需重启策略。参数穿透到 FutuQuoter（报价参数+重算预计算表）、Coordinator（maxDelta+alphaSensitivity）、AdaptiveWeightFramework（信号权重 Layer1）。

### useAsyncArbThread 配置

```yaml
# coordinator.yaml 根级 (唯一权威位置)
coordinator:
  useAsyncArbThread: true   # 实盘: 独立arb线程(低延迟), 回测: false(主线程同步)
```

异步模式: 主线程 pushTick(~50ns SPSC) → arb 线程 processTick+generateSignals → 订单请求 SPSC → 主线程 processPendingOrders 经 OrderRouter 下单。下单始终在主线程执行（ctx 无线程安全问题）。

## 编译与部署

### 编译环境

- OS: Linux (WSL Ubuntu 22.04 已验证)
- 编译器: g++ 11.4+ (需支持 C++17)
- CMake: 3.22+
- 依赖: WtUftCore, WTSTools, Share, boost_filesystem, pthread, atomic

### 编译命令

```bash
# 从项目根目录
cd /mnt/d/gf_pc/WonderTrader/wondertrader/src/build_all

# 首次或新增/删除源文件后需要重新cmake
cmake .

# 编译
make -j4 WtFutuCore
```

### 编译产物

```
build_all/build_x64/Debug/bin/WtUftRunner/futu/libWtFutuCore.so
```

### 部署

将编译产物复制到运行目录的 `uft/` 下：

```bash
cp build_all/build_x64/Debug/bin/WtUftRunner/futu/libWtFutuCore.so \
   dist/WtRunnerFutu/uft/
```

## 运行启动

### 启动命令

```bash
cd dist/WtRunnerFutu
./WtUftRunner ./config.yaml
```

### 启动流程

```
1. WtUftRunner 加载 config.yaml
2. 动态加载 libWtFutuCore.so (FutuStraFact.FutuMM)
3. UftFutuMmStrategy::init() 读取配置
4. on_init: FutuModuleAssembler::loadContractInfos (合约信息/session缓存/收盘时间推导)
5. FutuModuleAssembler::assemble() 创建并连接所有组件:
   - FutuPortfolio (组合管理)
   - FutuRiskMonitor (风控)
   - SpreadOptimizer (per合约)
   - SignalAggregator (per合约, 含LeadLag配置)
   - FutuQuoter (per合约)
   - ToxicFlowDetector (含内嵌SyntheticSignalFusion)
   - OrderRouter (套利/对冲/平仓)
   - StrategyCoordinator (流水线, 含 SessionPhaseManager/QuotePolicyChain)
   - SpreadArbitrageManager (套利)
   - AsyncArbitrageExecutor (异步执行)
   - PerformanceAnalyzer/Monitor
6. FutuConfigValidator 校验配置参数
7. 注册热更新参数
8. 订阅合约行情
9. 进入tick驱动循环
```

### 启动日志关键信息

```
SignalAggregator: N aggregators initialized (ofi=0.35, trade=0.25, book=0.20, mom=0.15, lead_lag=0.05)
Config validation passed (0 errors, N warnings)
UftFutuMmStrategy[X] session begin: YYYYMMDD
```

### 运行时监控

- **日志**: `dist/WtRunnerFutu/Logs/` 下按日期滚动
- **报价日志**: `[QUOTE]` 前缀, 每tick输出mid/alpha/skew/spread/bid/ask
- **风控日志**: `[RISK]` 前缀, 风险等级变化/动作执行
- **套利日志**: `[SPREAD_ARB]` 前缀, 信号触发/成交
- **绩效日志**: `[PERF]` 前缀, session_end时输出完整绩效报告
- **双边统计**: `[BILATERAL_STATS]` 前缀, session_end时输出

## 热更新参数

运行时可通过修改 `hotparams.yaml` + 共享内存同步更新以下参数，无需重启（键名必须 snake_case）：

| 参数 | 说明 |
|------|------|
| base_spread / base_qty / level_qty_multiplier / level_step | 基础报价参数 |
| max_delta | 最大Delta |
| alpha_sensitivity | Alpha 灵敏度 (冷启动保护) |
| ofi_weight / trade_weight / book_imbalance_weight / momentum_weight / lead_lag_weight | Alpha信号权重 (Layer 1) |
| strong_threshold | 强信号阈值 |
| confidence_weight_min / confidence_weight_max | GLFT 置信度权重区间 |
| phi / delta_skew_threshold / delta_skew_factor | GLFT模型参数 |
| max_spread_mult / min_spread_mult | 价差乘子范围 |
| depth_sensitivity / toxicity_spread_factor / low_confidence_spread_factor | 深度/毒性价差因子 |
| sticky_threshold / improve_retreat_ratio / protect_ticks / max_price_deviation | 报价粘性/保护 |

## 模块清单

### 做市核心
| 模块 | 文件 | 说明 |
|------|------|------|
| UftFutuMmStrategy | .h/.cpp | 入口策略壳 (847行), 回调锁+转发+on_tick主循环 |
| FutuModuleAssembler | .h/.cpp | 模块装配+合约信息加载 (5A-3, friend+别名零改动搬迁) |
| FutuRuntimeOps | .h/.cpp | 成交/订单/报单/通道/会话事件处理 (5A-3) |
| FutuConfigLoader | .h/.cpp | 配置解析+边界校验 (拆分组件) |
| FutuHotParamManager | .h/.cpp | 26热参数注册+分发 (拆分组件) |
| FutuHotParamWatcher | .h/.cpp | hotparams.yaml 文件 mtime 轮询线程 + 共享内存同步 |
| CloseoutOrchestrator | .h/.cpp | 收盘平仓全生命周期编排 (拆分组件) |
| CloseoutTrigger | .h/.cpp | 收盘触发判定 + CloseoutSub 子状态机 (P1.3 Step1) |
| CloseoutExecutor | .h/.cpp | 渐进式收盘执行器 (drain/mid/aggressive/sweep 分档下单) |
| ArbExecutionBridge | .h/.cpp | 套利执行桥+残腿防护 (拆分组件) |
| StrategyCoordinator | .h/.cpp | 做市流水线编排 (1228行) |
| RiskCoordinator | .h/.cpp | coordinator 内风控编排: checkRisk (HALT/TOXICITY 切换+forceFlat+arb 禁用) + checkTakerReduce (P1.3 Step2a) |
| SessionPhaseManager | .h | 会话阶段统一判定: 交易时段/休息窗口/closeout窗口 (5A-1, 纯函数) |
| QuotePolicyChain | .h | 报价决策链 6 policy (5A-2): RiskWiden/ArbCloseSync/Toxicity/LimitPrice/ColdStart/FillRetreat |
| RiskLiquidator | .h | 统一强平/减仓原语 (P0-1): 对手价FAK+三级价格校验+qty clamp |
| FutuQuoter | .h/.cpp | 多档双边报价引擎 |
| SpreadOptimizer | .h/.cpp | GLFT价差优化(公允价+偏斜, seqlock 参数读取) |
| SignalAggregator | .h | 6源信号聚合(SignalSlot表驱动) |
| ICWeightTracker | .h | 三层权重框架 + RollingScaleTracker + IC追踪 |
| OrderRouter | .h/.cpp | 非做市统一下单路由 (按源独立限速: ARBITRAGE/HEDGING/CLOSEOUT) |
| TradingState | .h | 统一交易状态管理 (分层状态机, 外部锁契约) |
| ISpreadStrategy | .h | 套利策略插件接口 + 注册表 |
| PreTradeDecision | .h | 盘前决策类型 (verdict 风控闸门 + strategy 策略输入) |

### 信号源
| 模块 | 文件 | 说明 |
|------|------|------|
| ISignalSource | .h | 信号源插件接口 |
| ISignalCombiner | .h | 信号组合器接口 (线性/非线性组合) |
| OFISignalSource | .h | 订单流不平衡 |
| TradeFlowSignalSource | .h | 交易流分析 |
| BookImbalanceSignalSource | .h | 订单簿不平衡 |
| MomentumSignalSource | .h | 价格动量 |
| LeadLagSignalSource | .h | 跨合约领先滞后 |
| VolatilitySignalSource | .h | 已实现波动率 |

### 风控与毒性
| 模块 | 文件 | 说明 |
|------|------|------|
| FutuRiskMonitor | .h/.cpp | 5级风控+自动恢复+收盘平仓 |
| ToxicFlowDetector | .h/.cpp | 毒性检测门面 |
| PredictiveToxicity | .h/.cpp | VPIN+OFI+Alpha预测毒性 |
| RealizedToxicity | .h/.cpp | 已实现毒性 |
| SyntheticSignalFusion | .h/.cpp | 3源信号融合 |
| TickTransactionInferer | .h | Tick级交易推断 |
| SelfTradeCalibrator | .h/.cpp | 自成交校准 |
| SelfTradePrevention | .h/.cpp | 自成交防护 |

### 组合与持仓
| 模块 | 文件 | 说明 |
|------|------|------|
| FutuPortfolio | .h/.cpp | 组合管理(Delta/敞口/对冲) |
| UnifiedOrderTracker | .h/.cpp | 订单状态单一真相源 |
| CorrelationManager | .h/.cpp | 跨合约相关性与beta |

### 套利
| 模块 | 文件 | 说明 |
|------|------|------|
| SpreadArbitrageManager | .h/.cpp | 跨期套利协调器 |
| SpreadCalculator | .h/.cpp | 价差计算 |
| SpreadRiskManager | .h/.cpp | 套利风控 |
| AsyncArbitrageExecutor | .h/.cpp | 异步套利执行 |
| MeanReversionStrategy | .h/.cpp | 均值回归 |
| TrendFollowingStrategy | .h/.cpp | 趋势跟踪 |
| PairsTradingStrategy | .h/.cpp | 配对交易 |
| StatisticalArbStrategy | .h/.cpp | 统计套利 |
| MarketMakingEnhancer | .h/.cpp | 套利信号增强做市 |
| SpreadArbitrageTypes | .h | 套利类型定义 (RiskAlert / CloseIntent / SpreadCalculator 值对象) |

### 基础设施
| 模块 | 文件 | 说明 |
|------|------|------|
| MarketDataContext | .h/.cpp | 行情深度+交易流门面 |
| FutuConfig | .h/.cpp | 配置读取工具 |
| FutuConfigValidator | .h | 配置校验 |
| FutuComponentFactory | .h/.cpp | 依赖注入工厂 |
| PerformanceMonitor | .h/.cpp | 无锁延迟/吞吐监控 |
| PerformanceAnalyzer | .h/.cpp | 绩效分析 |
| BilateralQuoteStats | .h/.cpp | 双边报价统计 |
| AlphaTypes | .h | Alpha类型定义 |
| FutureTypes | .h | 期货类型定义 |
| FutuDataDefs | .h | 期货数据定义 (PortfolioContext / ContractInfo 等) |
| OrderTypes | .h | 订单类型定义 (Source 枚举: ARBITRAGE/HEDGING/CLOSEOUT) |
| SpinLockGuard | .h | 自旋锁RAII |
| LockFreeQueue | .hpp | SPSC无锁队列(cache line 对齐) |
| TscClock | .h | rdtsc 时钟+10ms校准 (perf 埋点 ~6ns) |
| MonitorBridge | .h/.cpp | WtMonSvr GUI 数据桥 (stradata/funds.csv 落盘, 默认关) |
| SpinLockGuard | .h | 自旋锁RAII + RecursiveSpinLock (owner-tid+计数可重入, v7.6) |
| OrderApiGuard | .h | 下单 API 互斥 (v7.6): 29 处 orderApiCall 包裹点统一调用, 锁序单向 |
| EventDispatcher | .h | 同步事件监听器 (C10: 单写者收敛基础设施, 当前零订阅) |
| IOrderSink | .h | 订单接收接口 (FutuQuoter 绕过, OrderRouter 走) |
| TdSpiOffload | .h | TdSpi 成交路径日志 SPSC 队列 (C11: 延后打印避免阻塞下单) |

## 设计原则

1. **单一真相源**: TradingState管交易状态, UnifiedOrderTracker管订单状态(含original_qty部分成交跟踪), SignalContext管信号状态, FutuPortfolio管持仓状态
2. **双路径下单**: 做市零延迟直调ctx API; 非做市走OrderRouter限速+防自成交+审计
3. **插件架构**: ISignalSource接口(信号源)+ISpreadStrategy接口(套利策略), 注册表驱动, 配置启用/禁用
4. **门面模式**: ToxicFlowDetector(预测+已实现+融合), MarketDataContext(簿+流), SpreadArbitrageManager(计算+风控+策略)
5. **无锁热路径**: 原子计数器限速, 预分配向量, TickContext指针预解析, 内联价格计算, spinlock alignas(64) 隔离 cacheline
6. **状态机安全**: CloseoutState验证转换, TradingState HSM分层, RISK_HALTED唯一恢复出口, 外部恢复路径同步重置软风控倍数
7. **分级风控响应**: NORMAL→WARNING→ELEVATED→HIGH→CRITICAL, 渐进动作+滑窗读侧剔除+自动恢复+delta-rate检测与状态分离
8. **可恢复机制**: 可逆风险冷却后自动恢复(恢复上限per-session); 不可逆风险(日亏)需人工干预; 撤单ack超时强制untrack
9. **热参数更新**: 共享内存同步, on_params_updated()回调, 26个参数穿透到Quoter/Coordinator/权重框架
10. **异步套利**: 独立线程+无锁SPSC队列, ~50ns tick推送, 配置开关useAsyncArbThread(实盘true/回测false), 异常兜底保线程存活
11. **分层配置(单一权威)**: 每个开关/参数只有唯一位置 — config.yaml=身份+业务参数(无开关), coordinator.yaml=模块开关+模块参数, spread_arbitrage.yaml=套利子系统, hotparams.yaml=热更新参数。代码内 fallback 仅用于"键缺失时使用编译期默认", 不构成第二处配置。
12. **O(1)自成交检查**: MM 订单快照预计算 min_sell/max_buy 标量, executeSignal 自检从 O(n) 线性扫描降为 O(1) 比较
13. **回调串行化 (v7.4)**: 实盘回调三线程 (MdSpi/TdSpi/ticker) 直达策略, 13 个回调入口 `_cb_mtx` 统一串行化; 回测无竞争 ~20ns
14. **统一强平原语 (v7.4)**: RiskLiquidator 无状态服务, 所有"对手价平仓"路径 (HALT FORCE FLAT / channel_ready AUTO REDUCE) 单一实现, 三级价格校验 + qty clamp
15. **时间窗口单一事实来源 (v7.5)**: SessionPhaseManager 纯函数判定交易时段/休息窗口/closeout 窗口, closeout 执行状态机与之正交
16. **报价决策链 (v7.5)**: GLFT 后的 6 个调整阶段为 QuotePolicyChain 固定顺序链, 风控倍数/毒性冷却状态由 policy 自持, 调整阈值改动单点化
17. **分层并发防护 (v7.6)**: L0 标量/状态机原子+CAS → L1 结构递归自旋锁 (Tracker/Portfolio/Router/Bridge/Orch/Quoter) → L2 下单 API 互斥 (orderApiMutex); 指针逃逸经快照 API 消除; `FUTU_CALLBACK_LOCK` 编译开关 (1=大锁基线/0=细粒度) 双模式可回退
18. **锁序单向 (v7.6)**: 结构锁 → orderApiMutex 单向不可逆; orderApiMutex 内不取结构锁; RecursiveSpinLock 处理方法嵌套 (checkAutoCancel→untrackOrder)

## 优化历程 (ROADMAP V2)

| Phase | 内容 | 状态 |
|-------|------|------|
| Phase 0 | 基础设施 (12项 P0) | ✅ 完成 |
| Phase 1 | 架构重构 (triple-state-machine, 3-path quotes) | ✅ 完成 |
| Phase 2 | 状态机统一 (tryResumeFrom, ERROR 修复, 线程契约) | ✅ 完成 |
| Phase 3 | 代码质量 (on_tick 拆分, FIX 标记审计) | ✅ 完成 |
| Phase 4 | 信号系统改造 + 评估指标修正 | ✅ 完成 |
| **Phase 5** | **深度分析 v5: 48 项 Bug 修复 + 性能优化 + 架构重构** | **✅ 完成** |
| **Phase 6** | **深度分析 v6: 37 项诊断, 15 项 P0/P1/P2 修复** | **✅ 完成** |
| **Phase 7** | **深度分析 v7: 44 项诊断, 21 项真实修复 + 5 项误判避免** | **✅ 完成** |
| **Phase 8** | **报价黏性 + scout 多层结构 (v7.2)** | **✅ 完成** |
| **Phase 9** | **批次1-4: P0 bug/控制链收口/性能/架构 (v7.3-v7.4)** | **✅ 完成** |
| **Phase 10** | **5A 重构: SessionPhaseManager/策略壳瘦身/QuotePolicyChain (v7.5)** | **✅ 完成** |
| **Phase 11** | **并发精细化: L0原子/L1结构锁/L2下单互斥 (v7.6)** | **✅ 完成** |
| **Phase 12** | **v7.7 诊断复核: A1 stra_*包裹/forceFlatAll/TickContext快照复用/装配完备性校验 等 8 项** | **✅ 完成** |

### Phase 5 关键改造 (详见 docs/DEEP_ANALYSIS_V5.md)

**正确性修复 (35 Bug, P0 全清零)**:
- 时间戳 4 种单位混用统一为 epoch-ms (closeout 重试/套利冷却/毒性校准 全链路)
- halt 后自动恢复路径接线 (checkAndRecover 从死代码复活)
- 日亏损跨日累计修复 (resetDailyPnl)
- 部分成交 original_qty 跟踪 (不再提前 untrack)
- 反手成交 PnL 四情形正确计算
- 套利仓位回填 (updatePosition 从死代码复活) + 残腿防护机制
- 异步套利线程恢复 (5 处跨线程 data race 修复)
- 风控频率滑窗读侧剔除 + 恢复上限修复

**性能优化 (12 项)**:
- RollingScaleTracker 节流 + nth_element (每 tick 省 4 次 500 元素 sort)
- Momentum O(1) 增量 log 收益 (替代每 tick 127 次 std::log)
- TickContext 组件指针预解析 (每 tick 省 ~7 次字符串哈希查找)
- checkRiskLimits out-param 零堆分配
- updateMMOrders 世代号门控 (订单集未变时跳过快照深拷贝)
- 每 tick 墙钟单次读取 + LockFreeQueue cache line 对齐
- CMake 去 -ffast-math (NaN 风控静默失效隐患)

**架构重构 (3 方案)**:
- ISpreadStrategy 公共基类 + 注册表 (新增策略 7 处改动→1 行注册)
- SignalSlot 表驱动 (新增信号源 ~8 处改动→一段注册)
- 上帝类拆分: UftFutuMmStrategy 2924→2112 行, 拆出 ConfigLoader/HotParamManager/CloseoutOrchestrator/ArbExecutionBridge
- yaml 模块化统一 (coordinator.yaml 根级开关解析修复)

### Phase 6 关键改造 (详见 docs/DEEP_ANALYSIS_V6.md + V6_REVIEW.md)

**资金安全/数据正确性 (P0, 4 项)**:
- `flatten_threshold` 默认 3->2 (FLATTEN_POSITION 从不可达变可达; 后于 v7.3 证明数学不可达并删除)
- `getPositionReductionToLimit` int32_t->double 截断修复
- `timestampToMs` 删除 (fill_time 已是 epoch ms)
- TrendFollowing `entry_price` 赋值 (止损从死代码复活)

**逻辑正确性 (P1, 9 项)**:
- Welford->EWMA 衰减方差 (SpreadCalculator 适应 regime 切换)
- orphan 队列满兜底 (onArbSignalDropped)
- PnL 快照 atomic<double> (arb 线程 data race 修复)
- close 信号 in_flight 释放 / 自成交检查全扫描
- 自适应权重 tanh 归一化 / suppress 衰减修正
- SignalAggregator reset() 补全 / delta-rate 恢复路径统一

**清理 (P2, 2 项)**: hedge map 超时清理 / pushOverwrite 死代码删除

### Phase 7 关键改造 (详见 docs/DEEP_ANALYSIS_V7.md)

**资金安全/数学正确性 (P0, 10 项真实修复 + 1 项误报)**:
- STP 过滤 pending_cancel (checkSelfTrade + getConflictingMMOrders) -- 消除高频刷新 ARB 误拒
- `getActiveCountBySource` 过滤 pending_cancel -- 防 closeout inflight guard 卡死
- 撤单 ack 超时强制 untrack (5s) -- 防状态永久残留
- closeout 期间暂停 ARB (isCloseoutTriggered 门控)
- channel_lost 停 arb 线程 (新增 `setEnabled`)
- arb 线程 try/catch 兜底 (异常禁用套利保线程存活)
- `net_exposure` 符号错误修复 (`-` -> `+`, 全对冲价差从虚高 2 倍修正为 ≈0)
- `resetDailyPnl` 重置 avg_cost=0 (触发 pre_close 重设, 修复隔夜浮盈重复计入)
- Sharpe 年化因子修正 (per-trade 用 `sqrt(250)` -> `sqrt(250×日均笔数)`)
- inventory_pnl 填充 (recordTrade 按合约累计真实成交 PnL)
- ~~on_entrust HALT 期间误增 error_count~~ -- **误报** (已有 RISK_HALTED 早退守卫)

**代码质量/性能 (P1, 8 项真实修复 + 3 项误判避免)**:
- pending_adverse 内存泄漏 (30s 墙钟超时 + remove_if 全清理)
- cancelByPair 防御性补撤死代码 (`orders_it==end` -> `if(!sent)`)
- cancelOrder 补 pending_cancel 标记 (一致性)
- beta 截断 [0.7,1.5] 硬编码 -> config 可配 + CorrelationManager 按 expectedBeta 设带宽
- on_trade/channel_ready 漏清 `_risk_spread_mult` (新增 `onExternalResumeFromRisk()`)
- checkAutoCancel 改成员缓冲 (消除每 tick 3 次堆分配)
- getPairsForContract 返回 const ref (消除 6 处 vector 按值拷贝)
- 5 个 spinlock `alignas(64)` + 64B 填充 (消除 false sharing)
- ~~VaR 缺乘数~~ -- **误判** (WEIGHTED 模式 spread_std 已含乘数)
- ~~MarketMakingEnhancer 死代码~~ -- **有意观测模式** (推迟到 C2 阶段)
- ~~TickContext.code 堆分配~~ -- **高估** (合约码 14 字符命中 SSO)

**性能优化 (P2, 3 项)**:
- correlation+beta 合并扫描 (`computeCorrelationAndBeta` 单次 log-return, 消除重复 std::log)
- generateSignal spinlock 合并 (4->2 次/pair)
- executeSignal 自检改预计算标量 (O(n) 扫描 -> O(1), `_mm_buy/sell_orders` vector -> `min_sell/max_buy` 标量)

**避免的有害改动**: B7(误报)、B16(误判)、C1(有意设计)、P1(SSO覆盖)、A1-atomic(当前设计正确)

### Phase 8 (v7.2) 报价黏性 + scout 多层结构 (2026-07-31, 回测验证通过)

**背景**: 复核发现两个机制在 `alwaysObligation=true + numLevels=1` 下"空转"——
黏性(checkStickyUpdate)仅路径 A/B2 调用, 义务路径 B1 每 tick 无脑全撤重挂;
价格保护仅约束非义务单, 全义务配置下从不生效。

**Stage 1 — B1 条件式重挂 + 硬约束防穿**:
- B1 (handleObligationQuote) 双侧独立条件式: need = 无单 || 深度<min_valid_qty || 价格超黏性阈值;
  仅 need 侧撤+重挂, 双侧都不 need 零信息流 (回测实测 STICKY skip 274k 次/5日)
- 新增硬约束防穿盘 (所有层含义务, 不受 priceProtection 开关影响):
  bid ≤ best_ask-tick, ask ≥ best_bid+tick — 修复义务单完全豁免保护、skew 穿 mid 时可穿盘/自成交的缺陷
- 回测成交后处理对齐生产语义: 条件式撤单 (仅义务深度破坏才 cancelAll, 仅 stra_cancel
  避开回调内 stra_buy/sell 的 _orders 迭代风险), 深度满足则保留 (黏性受益)

**Stage 2 — scout 探测层架构** (自由层价格优于义务层):
- L0=自由探测层(scout): 最优价(l0_bid/l0_ask 直用), 小 qty(scoutQty=1), B2 路径
- L1=义务层: 次优价(l0∓levelStep), baseQty 大单(档位衰减不适用), B1 路径
- scout 成交 = 逆向信号 → `onScoutFillCancelObligation` 立即撤同侧义务层 →
  跳过通用重挂(不以旧价立即重挂义务单, 规避逆向成交) → 下一 tick 按新价重挂
- 新配置: `obligationLevel`(义务层档位, 默认0向后兼容), `scoutQty`(探测手数, 默认1.0);
  校验: obligationLevel<numLevels(报错), levelStep>0(报错, 堵价格阶梯倒挂入口), scoutQty≤baseQty(警告)
- 关键时序: UftMocker on_trade 先于 on_order, scout 成交时订单条目尚在, 识别可靠

**复核结论 (Q&A)**:
- **Q1 缺陷(已修)**: force 义务(util≥1.0)时旧 needObligation 对全层返回 true →
  scout 层被拖入 B1, qty 抬到 baseQty 且加仓侧不被 band 钳制 → 打满时 L0 挂 5 手
  最优价激进加仓单(逆向敞口)。修复: 非义务层永不转义务 + 自由分支 force 期间 qty 置零
  (force 时只留义务层, 恢复 v7.2 前语义)。本数据集 force 触发 0 次, 属生产防御
- **Q2 配置矩阵**: numLevels=1 → 仅 obligationLevel=0, 无自由层;
  numLevels=2 + obligationLevel=1 → scout 结构; numLevels=2 + obligationLevel=0 →
  经典"L0义务+L1外层自由单"(外层不享 scoutQty/不触发撤义务, 走 qty衰减+软保护)
- **Q3 l0价归属**: SpreadOptimizer 的 l0_bid/l0_ask(含alpha/skew/spread)报给 **scout 层(L0)**,
  义务层 = l0∓1tick。后果: 义务层较 v7.2 前被动 1 tick, skewCrossMid 激进减仓由
  1手 scout 先行; 减仓不足应调 skewCrossMaxTicks/checkTakerReduce, 而非把义务层拉回 L0
- **Q4 sub-tick 义务空窗评估** (实测): SCOUT 触发 14,800 次 / ~17.6万 tick(5日,500ms/tick)
  → 8.4% 的 tick 出现单侧义务空窗, 单侧空窗占比 ≈4.2%, 单次 ≤1 tick。
  对常规在场率考核(70-90%)可接受; 趋势行情有聚集效应(scout 连续成交 → 义务层连续缺席,
  正是设计意图的逆向避让)。可选改进: 生产端撤义务后立即外移1tick重挂 / scout触发冷却 /
  仅 scout 全成才撤义务层

**回测验证** (_ec_5d.yaml, exit=0): Config validation 0错0警(含新校验), 0 segfault,
0 穿盘报单, SCOUT 14,800 次(日志确认 1手scout成交→撤义务层lvl1),
报单分布 义务5手×17.9k / scout1手×15.4k, 净收益 ¥442,561 (arb ON + srand 噪声, 不与基线直接比)

### Phase 9 (v7.3-v7.4) 批次1-4 修复 (2026-08-03, 逐批回测验证通过)

**批次1 — P0 bug (9项)**:
- C1 单边盘口(锁板) mid=0 污染 markToMarket → 双边>0才接受; last_price 仅正价写入; 强平价 bid1/ask1>0 校验
- C2 recordFill 时间戳墙钟→replay ms (FillRetreat/毒性检测从失效恢复真实样本驱动)
- C3+M3 套利平仓方向与 live_pos 同号 drop; 拒单/空 localids 补 cancelByPair+markLegRejected
- M1+M2 拒单统一幂等 finalizeOrder (tracker REJECTED 终结态 + router onOrderDone)
- M9 closeout FAK 部分成交/自撤单不误判 FAILED (仅零成交拒单)
- 业务#2 on_trade 恢复统一走 checkAndRecover 四道闸 + 对称复活 arb
- 业务#3 delta-rate 停机同步撤单; 业务#4 BLOCK_SIDE 停 arb executor
- F20 SpreadOptimizer GLFTParams mutex→seqlock (版本号翻转+读侧快照)

**批次2 — 控制链收口 (v7.3)**:
- 删除 PAUSE_QUOTING/FLATTEN_POSITION 数学不可达死分支 → "软连续控制+硬HALT"两头化
- closeout 窗口 checkRisk 复跑 (DRAINING 等 4 活跃态, 强平过程不再是风控盲区)
- min_valid_qty 热同步 (=base_qty); checkPreTradePosition 切 pending 全源口径

**批次3 — 性能快赢 (8项, 基线: Tick-FullChain mean=185μs)**:
- P0 埋点: TscClock (rdtsc+10ms校准) + SIGNAL_TO_ORDER/Quote-to-Fill 通道 + 60s [PERF] 摘要
- F1/F2 CorrelationManager 预建索引 (_code_calcs/_pair_index); F4 FutuPortfolio 7值单pass聚合缓存
- F5 chrono 门控; F6/F7 tc.time_hms/session 指针复用; F8 exp 提升; F9 pending 增量维护; F13 TRADE日志降debug

**批次4 — 架构重构 (P0 项)**:
- P0-2 回调线程模型: 框架源码核实实盘三线程回调 (on_tick=MdSpi/on_trade=TdSpi/on_session_end=ticker),
  策略 13 回调 `_cb_mtx` 串行化; TradingState tid 断言改外部锁契约
- P0-1 RiskLiquidator 统一强平原语 (对手价FAK+三级价格校验+qty clamp), 替换 coordinator HALT FORCE FLAT
  与 channel_ready AUTO REDUCE 两处重复漂移实现 (顺带修 m19: 被动挂单+无价格校验)
- 死代码清理: onSpreadTrade (74行) / FutuQuoter computeBid/AskPrice / SpreadCalculator._alpha /
  LockFreeQueue._drop_count; C9 liquidity_score 接线 total_volume; C10 last_update 时间戳;
  C12 calculateDaysBetween 换精确 Gregorian (Hinnant days-from-civil)

### Phase 10 (v7.5) 5A 重构 (2026-08-04, 逐波回测验证通过)

**5A-1 SessionPhaseManager** (纯函数, 时间窗口单一事实来源):
- 统一三处窗口判定: processSectionBreak(休息窗口) / preCheck(交易时段) / checkCloseout(夜盘跨日+白盘双触发)
- 状态机(IDLE→TRIGGERED→DRAINING→...) 仍归 FutuRiskMonitor, manager 只管时间窗口
- 对拍: SECTION_BREAK 1014/1129, CLOSEOUT_TRIGGERED 5天全部 14:45, 与基线逐点一致

**5A-3 策略壳瘦身** (UftFutuMmStrategy 2271→802行, -65%):
- FutuModuleAssembler (832行): initBusinessModules 装配 + 合约信息加载外移
- FutuRuntimeOps (930行): on_trade / on_channel_ready / on_session_begin+end /
  on_entrust 85 / on_order 66 / on_channel_lost 40 / finalizeOrder 40 外移
- 手法: friend + 引用别名, 函数体逐行零改动搬迁; 锁保留在策略壳

**5A-2 QuotePolicyChain** (报价决策链模块化):
- processQuoting 内联 145 行 → 35 行链装配; 6 policy: RiskWiden→ArbCloseSync→Toxicity
  →LimitPrice→ColdStart→FillRetreat (执行顺序与旧实现严格一致)
- 状态迁移: _risk_spread_mult→RiskWidenPolicy (soft覆盖/hard闩锁/恢复清零 4写入点);
  _toxicity_resume_time→ToxicityPolicy (冷却查询/抑制/复位 3处)

**回测验证** (每波 _ec_5d.yaml): 35-36k 成交, dynbalance ¥28-31万 (srand 噪声范围),
closeout 5 COMPLETED/10 TRIGGERED/0 FAILED, 0 segfault, Config validation 0 errors

### Phase 11 (v7.6) 并发精细化三阶段 (2026-08-05, 双模式回测验证通过)

**背景**: v7.4 `_cb_mtx` 大锁是正确性基线但非终态。逐变量复核确认 MdSpi/TdSpi
共享集合 15 类 (其中 4 类崩溃级: `_violations_buf` vector + 3 个 hashmap,
2 类资金正确性级: FutuPortfolio/TradingState)。用户决策: 事件队列方案因
延迟不可接受而排除, 采用分层细粒度 + 编译开关可回退。

**阶段 1 — L0 零锁化 (原子+CAS)**:
- TradingState 全字段 `std::atomic<Enum>`: tryResumeFrom→CAS,
  setQuotingPhase→read-check-CAS 循环 (canTransition 校验与写入原子化)
- 策略 5 标量原子化 (_exchange_time_ms/_order_error_count/_quoting_paused_since/
  _channel_ready/_price_stale) + _portfolio_ctx_dirty
- _last_mid 改 init 定码原子槽 (结构不可变, 消除 hashmap 结构竞态)
- _violations_buf 双缓冲核实 (天然已拆); RiskWidenPolicy/ToxicityPolicy 状态原子化

**阶段 2 — L1 结构锁 + 指针逃逸治理**:
- 新增 RecursiveSpinLock (owner-tid+计数可重入, 处理 checkAutoCancel→untrackOrder
  等公开方法嵌套)
- 6 结构全方法守卫: UnifiedOrderTracker(44方法)/FutuPortfolio(40+)/
  OrderRouter(16)/ArbExecutionBridge(5)/CloseoutOrchestrator(5)/_last_quote_params
- **关键发现**: getContract/getOrderByOrderId 裸指针逃逸使方法级锁变假安全 →
  新增快照 API (getContractSnapshot/getAllContractsSnapshot/
  getPositionBreachedSnapshot/getOrderInfoCopy), 转换 25+ 外部读点;
  hedge_ratio 裸指针直写收编为 smoothUpdateHedgeRatio 锁内方法

**阶段 3 — Quoter + 下单互斥 + 编译开关**:
- FutuQuoter per-quoter 递归守卫 (17方法, 整方法守卫; 已知残留:
  getBilateralStats 外部引用, 统计对象低危已文档化)
- OrderApiGuard: 29 处 orderApiCall 包裹点统一包裹 (orderApiMutex),
  覆盖框架级 UftStraContext 内部容器竞态 (不可越界修框架)
- 锁序单向: 结构锁 → orderApiMutex 不可逆, orderApiMutex 内不取结构锁
- FUTU_CALLBACK_LOCK 编译开关: 1=大锁基线(默认)/0=细粒度, 双模式可回退

**验证** (_ec_5d.yaml 双模式):
- 模式 1 (大锁): 34,643 成交, dynbalance ¥290,923, closeout 5/10/0, 0 crash
- 模式 0 (细粒度): 28,777 成交, 0 crash, 功能等价 (同序列日亏分支设计行为正确)
- 验证插曲: 两次回测 11×CLOSEOUT_FAILED 查证为 srand 序列落入真实日亏分支
  (-323k 触发 LOSS_CRITICAL IRREVERSIBLE), 该极端路径上 RiskLiquidator FORCE FLAT/
  executor HALT 门/日界 auto-clear 全部按设计工作, 非回归

**竞态消除现状**: 复核清单 15 类中, 大锁模式全覆盖; 细粒度模式 14 类由
原子/结构锁/orderApiMutex 覆盖, 仅 BilateralQuoteStats 外部引用为文档化
已知残留 (统计失真级, 非崩溃级)。

## 待定项

### 成交路径事件队列化解耦 (已评估, 驳回)

**状态**: 评估完成, 驳回 (2026-08-06)

**背景**: 外部诊断建议 (性能#7) 将成交路径重活与 on_tick 解耦——on_trade 只做
无锁记账+置标志, 成交事件走 SPSC 队列由独立线程处理, 让 on_tick 永不阻塞。

**驳回依据** (理由修正: 不是"队列延迟高"):
- 两种队列变体延迟本质不同: 主循环下一 tick 消费 (最坏 1 tick=500ms, 不可接受)
  vs 独立自旋消费线程 (~0.1-1μs, 可接受)。性能#7 提的是后者, "延迟高"不成立。
- 真正驳回理由: ①收益≈0——大锁竞争实测概率 ~0.001%/tick (成交 0.5/s ×
  tick 临界区 ~20μs), 每周几次 ~20μs 尾延迟接近噪声; ②CTP 约束在进程外
  (500ms 切片+ms 级交易所往返), 进程内 μs 级优化边际贡献≈0;
  ③复杂度/风险不对称 (回调重排保障/shutdown drain/第三业务线程)。

**重启条件**: 极端行情 tick 风暴导致碰撞率质变 (perf monitor 实盘数据显示
Tick-FullChain p99 恶化) 时, 以"独立自旋消费线程"变体重估。

### v7.7 诊断复核修复 (2026-08-06, 回测验证通过)

外部诊断报告复核: P1 属实 1 项 (修复), 部分误判 2 项 (性能#2 analyze 缓存已解决/
性能#4 cancelAll 频率前提不准), 方向冲突 1 项 (性能#7/#8 事件队列, 见上条驳回)。

| 修复 | 级别 | 说明 |
|---|---|---|
| A1 stra_* 漏网包裹 | P1 | stra_quote(FutuQuoter)/stra_exit_long/short(OrderRouter) 补 orderApiCall — v7.6 批量正则只匹配 buy/sell/cancel 三名字所致; exit 是双线程强平路径, 框架级竞态消除 |
| 业务#2 forceFlatAnchor → forceFlatAll | P2 | HALT IRREVERSIBLE 强平从"仅 anchor×|组合delta|手数"改全组合逐合约实际持仓对手价 FAK (多合约敞口残留修复) |
| 性能#1 TickContext 快照复用 | P2 | processTick 入口一次性 getContractSnapshot 存 tc.cs, preCheck/quoting 3+ 处复用 (等价性: 后续 Stage 仅读不变量字段) |
| A3 装配完备性校验 | P2 | FutuModuleAssembler 收口 [ASSEMBLY] 依赖非空校验, 运行期空指针前移为启动期报错 |
| A4 去 const_cast | P3 | processSectionBreak 改 TickContext& 非 const 签名 |
| C2/C3/C4/C5/A2/性能#3 | P3 | setQuotingPhase 复用 canTransitionQuoting; static 节流变量成员化; RecursiveSpinLock 不变式注释固化; LockFreeQueue 析构 drain (元素含 std::string); 锁序文档补全 (含无环验证结论); ARB-ENH 观测模式运行期开关 (s_observe_enhancer 默认关) |
| 业务#3 adaptive 文档化 | P3 | use_adaptive_params=true 时启动警告 (updateAdaptiveParams 空占位, 功能未激活) |

**回测验证**: 36,333 成交, dynbalance ¥304,358, closeout 5/10/0, ASSEMBLY 校验通过, 0 crash

### Trade-through 毒性检测 (暂缓)

**状态**: 评估完成, 暂缓实现

**设计**: 作为 ISignalSource 实现, 基于 tick 快照 volume 增量检测脉冲毒性 (连续同方向大单).

**价值**: 补充 VPIN 的持续性毒性检测, 提供脉冲毒性 + 方向性检测.

**暂缓原因**:
- 国内期货只有行情切片 (tick snapshot), 无逐笔成交
- volume 增量方向推断有 bid-ask bounce 问题 (TradeFlow IC=-0.83 的同根因)
- tick 快照无法精确区分"大单扫盘" vs "多笔小单累积"
- 当前 VPIN toxicity 实际影响仅 1% 交易时间, 改善空间有限

**重启条件**: 接入有逐笔成交数据的市场 (如加密货币), 或解决 tick 快照方向推断准确性.

### P1-8 跨期同步报价组 (部分覆盖)

Phase 5 的 LeadLag + 权重框架已部分覆盖跨期协调。独立 sync_group 实现需要改 Quoter 架构, 留作后续.

## 深度分析报告

完整的逐行源码审查、Bug 清单与修复记录详见:

- **[docs/DEEP_ANALYSIS_V7.md](docs/DEEP_ANALYSIS_V7.md)** - v7 深度分析 (44 项诊断: 架构/业务逻辑/代码质量/性能四维; 21 项真实修复 + 5 项误判避免; 含复核结论表与分阶段重构方案)
- **[docs/DEEP_ANALYSIS_V6.md](docs/DEEP_ANALYSIS_V6.md)** - v6 深度分析 (37 项诊断 + 修复方案)
- **[docs/DEEP_ANALYSIS_V6_REVIEW.md](docs/DEEP_ANALYSIS_V6_REVIEW.md)** - v6 复核报告 (2 误报 + 3 降级 + 35 确认)
- **[docs/DEEP_ANALYSIS_V5.md](docs/DEEP_ANALYSIS_V5.md)** — v5 深度分析 (35 Bug + 48 Fix + 3 架构方案 + 复核修正)
- **[docs/ARB_SELF_CLOSE_DESIGN.md](docs/ARB_SELF_CLOSE_DESIGN.md)** — 套利分级平仓设计方案 v2.0 (分级执行: CLOSE 保持 B-3 / STOP_LOSS taker 立即 / TIMEOUT maker 挂单; 含成本模型 fee+spread+slippage; Phase A1-A10 + Phase B/C/D; 状态: **部分落地** — spread_arbitrage.yaml 的 `arb_close` 节点(C0/C1/C2 灰度)已接线, enabled=false 默认纯 B-3 兼容)
- **[OPTIMIZATION_REPORT.md](OPTIMIZATION_REPORT.md)** — v4 优化报告 (19 项已修)
