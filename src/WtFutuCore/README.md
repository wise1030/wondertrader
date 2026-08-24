# WtFutuCore —— 期货高频做市引擎

> 构建于 WonderTrader `WtUftCore` 之上的做市交易栈（C++17 动态库，策略名 `FutuStraFact.FutuMM`，
> 由 `WtUftRunner` / `WtBtRunner` 动态加载）。
>
> 核心能力：GLFT 库存风险定价 + 五路信号自适应 alpha 集成 + 六段报价策略责任链 + B+ 订单槽
> 状态机（撤单重试/zombie 升级）+ 三层风控漏斗 + 收盘渐进平仓 + 跨期价差套利子系统 +
> 做市义务双边统计。实盘与回测共用代码路径（replay 时钟驱动），支持逐比特 A/B 回归验证。
>
> 本文档描述当前工作树状态（V8-R5/R6 之后，去大锁 WS-A/E/F 已落地），与同目录
> `AGENTS.md`（开发规范与全部方案记录）配套。

## 目录

- [1. 项目定位](#1-项目定位)
- [2. 总体架构](#2-总体架构)
- [3. 目录结构](#3-目录结构)
- [4. 模块详解](#4-模块详解)
  - [4.1 策略壳层 UftFutuMmStrategy](#41-策略壳层-uftfutummstrategy)
  - [4.2 业务逻辑外移点 FutuRuntimeOps](#42-业务逻辑外移点-futuruntimeops)
  - [4.3 主循环编排 StrategyCoordinator](#43-主循环编排-strategycoordinator)
  - [4.4 状态机与时间窗 TradingState / SessionPhaseManager / EventDispatcher](#44-状态机与时间窗-tradingstate--sessionphasemanager--eventdispatcher)
  - [4.5 GLFT 定价模型 SpreadOptimizer](#45-glft-定价模型-spreadoptimizer)
  - [4.6 报价策略责任链 QuotePolicyChain](#46-报价策略责任链-quotepolicychain)
  - [4.7 报价执行器 FutuQuoter](#47-报价执行器-futuquoter)
  - [4.8 订单全生命周期跟踪 UnifiedOrderTracker](#48-订单全生命周期跟踪-unifiedordertracker)
  - [4.9 下单路由与守卫 OrderRouter / OrderApiGuard 等](#49-下单路由与守卫-orderrouter--orderapiguard-等)
  - [4.10 组合管理 FutuPortfolio / CorrelationManager](#410-组合管理-futuportfolio--correlationmanager)
  - [4.11 风控子系统](#411-风控子系统)
  - [4.12 收盘平仓三件套 CloseoutTrigger / Orchestrator / Executor](#412-收盘平仓三件套-closeouttrigger--orchestrator--executor)
  - [4.13 信号因子层 signals/](#413-信号因子层-signals)
  - [4.14 毒性检测 ToxicFlowDetector 族](#414-毒性检测-toxicflowdetector-族)
  - [4.15 套利子系统 arb/](#415-套利子系统-arb)
  - [4.16 观测与统计层](#416-观测与统计层)
  - [4.17 配置与装配层](#417-配置与装配层)
  - [4.18 低延迟基础设施](#418-低延迟基础设施)
- [5. 关键机制专述](#5-关键机制专述)
- [6. 配置体系](#6-配置体系)
- [7. 构建、部署与回测](#7-构建部署与回测)
- [8. 测试体系](#8-测试体系)
- [9. 已知框架层限制与文档索引](#9-已知框架层限制与文档索引)

---

## 1. 项目定位

WtFutuCore 面向**国内期货交易所（CTP 通道）的指定合约做市场景**：在满足交易所双边报价义务的
前提下，通过库存偏移（skew）、毒性流规避、自成交防护和多层风控，实现低延迟、可复现回测、
可持续运行的做市交易；同时内置一套跨期/跨品种价差套利子系统与做市协同。

设计主线：

| 主线 | 说明 |
|---|---|
| **delta / position 语义边界** | 策略逻辑一律用 delta 口径（分母 = `contract_max_delta` 软限）；风控硬闸门一律用 position 口径（`maxPosition`）。禁止混用（2026-08-19 用户裁定） |
| **回测=实盘同一代码路径** | 全部时间判定使用 replay 交易所时钟（tick 的 actiondate/actiontime 合成 `_exchange_time_ms`），arb 执行器经 `setReplayNowUs` 注入；回测不开线程（`useAsyncArbThread:false`）保证逐比特可比 |
| **壳薄核厚** | 策略类只做框架回调适配与生命周期持有；业务逻辑外移到 `FutuRuntimeOps`（成交路径）、`StrategyCoordinator`(行情主循环)、`FutuModuleAssembler`(装配) |
| **fail-fast** | 配置解析边界校验 → 合约/锚定合约校验 → 装配依赖完备性二次校验，缺失即启动报错拒绝空跑 |
| **并发安全显式化** | 回调入口大锁 `_cb_mtx` 兜底 + 每模块小锁（RecursiveSpinLock / atomic_flag）+ 属主域命令通道，跨线程写点清单化管理 |

---

## 2. 总体架构

### 2.1 五层分层

```
┌──────────────────────────────────────────────────────────────────────┐
│ L1 壳层    UftFutuMmStrategy —— 14 个框架回调适配、_cb_mtx 大锁、       │
│            PendingCommand 命令通道、约 20 个业务模块的生命周期持有       │
├──────────────────────────────────────────────────────────────────────┤
│ L2 装配层  FutuConfigLoader(解析+fail-fast) → FutuModuleAssembler      │
│            (assemble 第0~16步创建接线) → FutuComponentFactory(工厂)     │
│            → wireDeps + validateDeps(指针级收口)                        │
├──────────────────────────────────────────────────────────────────────┤
│ L3 协调层  StrategyCoordinator(processTick 七阶段编排)                  │
│              ├ CloseoutTrigger    (收盘平仓触发)                        │
│              ├ RiskCoordinator    (组合级硬风控/taker 减仓)             │
│              └ QuotePolicyChain   (GLFT 之后六段报价调整链)             │
├──────────────────────────────────────────────────────────────────────┤
│ L4 领域层  报价: SpreadOptimizer(GLFT) + FutuQuoter(三路径发单)         │
│            执行: UnifiedOrderTracker(B+ 状态机) + OrderRouter           │
│            风控: FutuRiskMonitor + PreTradeDecision 双层                │
│            平仓: CloseoutOrchestrator + CloseoutExecutor                │
│            组合: FutuPortfolio(SSOT) + CorrelationManager               │
│            信号: SignalAggregator + 五路信号源 + ToxicFlowDetector       │
│            套利: arb/ 子系统 (Manager/Calculator/Executor/Bridge)        │
├──────────────────────────────────────────────────────────────────────┤
│ L5 基础设施 RecursiveSpinLock / LockFreeQueue(SPSC) / TscClock(rdtsc)  │
│            HotParam 共享内存热参 / PerformanceMonitor·Analyzer          │
│            MonitorBridge(GUI) / BilateralQuoteStats(义务统计)           │
└──────────────────────────────────────────────────────────────────────┘
```

### 2.2 线程模型（实盘）

```
MdSpi(CTP 行情)   on_tick/on_transaction/on_order_queue/on_order_detail
                  └→ StrategyCoordinator::processTick 各阶段
                     （信号→alpha→风控→自动撤单→GLFT→政策链→refreshQuotes 发单）
TdSpi(CTP 交易)   on_trade/on_order/on_entrust/on_channel_ready|lost
                  └→ FutuRuntimeOps::processTradeFill / onOrderEvent /
                     onEntrust / onChannelReady（含 B+ 事件驱动补挂）
RtTicker(定时)    on_session_begin/end（发布 Session 命令到命令通道）
arb 线程          AsyncArbitrageExecutor::arbThreadFunc（useAsyncArbThread=true 时）
                  └→ Manager.onTick → generateSignals → executeSignal → SPSC 队列 → MdSpi 下单
watcher 线程      FutuHotParamWatcher 1Hz 解析 hotparams.yaml → 只写共享内存 + 置脏
                  （应用统一由 on_tick 在大锁内 drain）
```

- 回测时以上合并为**单线程**（arb 同步执行、Session 命令自 drain），行为可复现。
- 所有回调入口由 `FUTU_CB_LOCK_GUARD()` 宏串行化：默认 `big` 模式 =
  `std::recursive_mutex _cb_mtx`；`none` 模式（`-DFUTU_CB_LOCK_BIG=0`）下正确性由各模块
  自带小锁承担（详见 §5.3）。

### 2.3 主数据流

**tick-to-quote（MdSpi 热路径）：**

```
on_tick → 命令检查点(~1ns) → 热参数 drain → ERROR 态退避恢复检查
        → replay 时钟推进(_exchange_time_ms 注入 coordinator/router/arb/riskmonitor)
        → markToMarket + CorrelationManager 更新
        → coordinator->processTick:
            Stage 0    CloseoutTrigger.process()        # 收盘窗口触发平仓状态机
            Stage 0.5  processSectionBreak              # 节间休息窗撤单停报
            Stage 1    preCheck(nan/锁板/tick_size 防御)
            Stage 2    updateMarketData(PnL 快照发布给 arb 线程)
            Stage 3    updateSignals(book.onTick → aggregator.update(computeAlpha)
                                     → 毒性三连 → updateToxicity 显式写回)
            Stage 4    RiskCoordinator.checkRisk(HALT/BLOCK/WIDEN/恢复)
                       + checkTakerReduce(util≥阈值 FAK 对手价减仓)
            Stage 5    processAutoCancel(STALE 撤销 + B+ 撤单超时重试 + zombie 升级)
            Stage 6    processQuoting:
                          冷启动检测 → PreTradeDecision 双层(delta 软输入 + position 硬闸门)
                          → SpreadOptimizer.computeOptimalQuote(GLFT 公允价+skew+spread_mult)
                          → QuotePolicyChain.run(RiskWiden→ArbCloseSync→Toxicity→LimitPrice
                                                 →ColdStart→FillRetreat)
                          → CachedQuote 缓存 → refreshQuotes 三路径发单 → recordOrders 频控计数
            perf 打点(TscClock rdtsc, TICK_TO_QUOTE/SIGNAL_TO_ORDER 双通道)
        → CloseoutOrchestrator.onTick → arb_bridge.onTick(套利喂入) → MonitorBridge 落盘
```

**fill 路径（TdSpi）：**

```
on_trade → processTradeFill:
     方向归一(is_buy=(isLong==isOpen)) → SideFillBreaker 同侧熔断(CLOSEOUT 豁免)
     → 影子簿记账(onTradeFill/setShadowFromEngine, 漂移检测→resyncPosition)
     → arb_bridge.onTradeFill(in_flight 扣减 + 残腿对冲 Source::HEDGING FAK)
     → quoter 定位 isMyOrder → onTrade / onScoutFillCancelObligation(scout 成交撤义务层)
     → tracker.recordOrderFill(按 original_qty 判完全成交才 untrack)
     → PerformanceAnalyzer.recordTrade + SelfTradeCalibrator.recordFill(replay 时间戳)
     → 明细日志 SPSC 卸载队列
     → 恢复四道闸(checkRiskLimits→checkAndRecover→resumeFromRisk→软风控复位+arb 复活)
     → requoteAfterFill(实盘同步立即重挂; 回测条件式只撤保可复现)

on_order → onOrderEvent: quoter.onOrder(终态移槽) → B+ live 事件驱动补挂(!backtest)
          → closeout_orch.onOrderEvent → router.onOrderDone + arb pair tag 清理

on_entrust → onEntrust: 成功清零错误计数; 失败死单清理+finalizeOrder,
          达 orderErrorThreshold → qphase=ERROR + haltTrading(REVERSIBLE) + 全撤
```

### 2.4 并发与锁体系速览

| 共享对象 | 保护 | 线程归属 |
|---|---|---|
| 全部回调入口 | `_cb_mtx`(recursive_mutex)，`FUTU_CB_LOCK_BIG` 两态开关 | 策略壳 |
| FutuQuoter 槽位/反查表 | per-quoter `RecursiveSpinLock` 整方法守卫 | Md/Td 双写 |
| UnifiedOrderTracker 账本 | `RecursiveSpinLock` + generation 号快照 | Md/Td |
| OrderRouter / ArbExecutionBridge / CloseoutOrchestrator | 各自 `RecursiveSpinLock _lock` | Md/Td |
| FutuPortfolio | 全方法 `RecursiveSpinLock`（聚合值 dirty 缓存单次扫描） | Md/Td |
| FutuRiskMonitor halt/closeout 状态域 | `atomic<RiskCategory>` + `_halt_domain_lock`（WS-A 收编） | Md/Td/**arb 线程** |
| FutuRiskMonitor 频控环 ×3 | `_rate_lock`(atomic_flag)；`recordOrders(n)` 批量接口 | Md/Td/arb |
| SpreadOptimizer GLFT 参数 | seqlock（奇偶版本号防撕裂） | 热更写 / Md 读 |
| CachedQuote / `_last_mid` | 小锁 / MidSlot atomic（Coordinator 单一属主） | Md 写 / Td 读 |
| TradingState | 全字段 atomic + CAS 转移接口 | 多线程 |
| 框架 stra_* 引擎调用 | 全局 `orderApiMutex`（OrderApiGuard，单向锁序最内层） | Md+Td 发单互斥 |
| SPSC 队列（日志卸载/arb tick/order/orphan） | LockFreeQueue alignas(64) | 单生产者单消费者 |

锁序纪律：结构锁 → `orderApiMutex` 单向；`orderApiMutex` 内不得再取任何结构锁。

---

## 3. 目录结构

```
src/WtFutuCore/
├── AGENTS.md                    开发规范 + 全部方案记录（先规划后改码工作流）
├── README.md                    本文档
├── CMakeLists.txt               SHARED 库目标，产物 → bin/WtUftRunner/futu/
├── config/
│   ├── config.yaml              Runner 主配置（身份/合约/业务参数，不含模块开关）
│   ├── coordinator.yaml         模块开关唯一权威 + 信号/毒性/GLFT/autoCancel 参数
│   ├── hotparams.yaml           27 个热参数运行时文件（watcher 1Hz 轮询）
│   └── spread_arbitrage.yaml    套利子系统配置
├── docs/                        设计文档（DEEP_ANALYSIS_V5~V7、DIAGNOSTIC_REPORT_V8、
│                                ARB_SELF_CLOSE_DESIGN、MM_SOFT_RISK_V3、REFACTOR_ROADMAP…）
├── UftFutuMmStrategy.h/.cpp     策略壳（L1）
├── FutuRuntimeOps.h/.cpp        成交/通道/会话业务逻辑外移点
├── StrategyCoordinator.h/.cpp   行情主循环编排器（L3 核心）
├── QuotePolicyChain.h           六段报价策略责任链
├── SpreadOptimizer.h/.cpp       GLFT 定价模型（旧文档所称 GLFTModel 已并入此处）
├── FutuQuoter.h/.cpp            报价执行器（订单槽位/价格阶梯/三条路径）
├── UnifiedOrderTracker.h/.cpp   订单全生命周期账本（B+ 状态机）
├── OrderRouter.h/.cpp           非 MM 来源下单路由（限速/STP/pair 撤单）
├── OrderTypes.h / IOrderSink.h / OrderApiGuard.h / TdSpiOffload.h
├── FutuPortfolio.h/.cpp         组合持仓 SSOT（UnifiedNetBook 影子簿）
├── CorrelationManager.h/.cpp    跨合约相关性/hedge_ratio
├── FutuRiskMonitor.h/.cpp       风控监控核心（halt 域/频控/平仓状态机 SSOT）
├── RiskCoordinator.h/.cpp       组合级风控协调 + taker 减仓
├── RiskLimitsConfig.h / PreTradeDecision.h / SideFillBreaker.h / RiskLiquidator.h
├── SelfTradePrevention.h/.cpp / SelfTradeCalibrator.h/.cpp
├── CloseoutTrigger.h/.cpp       收盘平仓触发
├── CloseoutOrchestrator.h/.cpp  平仓编排（触发→延迟→驱动→回报跟踪）
├── CloseoutExecutor.h/.cpp      紧迫度分级渐进平仓执行器
├── TradingState.h / SessionPhaseManager.h / EventDispatcher.h
├── BilateralQuoteStats.h        做市义务双边统计（per-quoter）
├── MarketMakingEnhancer.h/.cpp  套利信号增强做市（观测模式半接线）
├── PerformanceMonitor.h/.cpp / PerformanceAnalyzer.h/.cpp / MonitorBridge.h/.cpp
├── ToxicFlowDetector.h/.cpp / PredictiveToxicity.h/.cpp / RealizedToxicity.h/.cpp
├── TickTransactionInferer.h/.cpp
├── AlphaTypes.h / FutureTypes.h / FutuDataDefs.h
├── FutuConfig.h/.cpp / FutuConfigLoader.h/.cpp / FutuConfigValidator.h
├── FutuModuleAssembler.h/.cpp / FutuComponentFactory.h/.cpp
├── FutuHotParamManager.h/.cpp / FutuHotParamWatcher.h/.cpp
├── SpinLockGuard.h / LockFreeQueue.hpp / TscClock.h
├── signals/                     信号因子层
│   ├── ISignalSource.h / ISignalCombiner.h
│   ├── OFISignalSource.h / TradeFlowSignalSource.h / BookImbalanceSignalSource.h
│   ├── MomentumSignalSource.h / LeadLagSignalSource.h
│   ├── VolatilitySignalSource.h/.cpp
│   ├── ICWeightTracker.h        三层自适应权重框架(IC/regime/一致性)
│   ├── SignalAggregator.h       信号聚合器 + computeAlpha
│   └── MarketDataContext.h/.cpp 行情上下文 Facade(OrderBookStateTracker/TradeFlowTracker)
└── arb/                         套利子系统
    ├── SpreadArbitrageTypes.h / ISpreadStrategy.h
    ├── SpreadCalculator.h/.cpp  价差计算(fresh-pairing/OLS beta/EWMA z-score)
    ├── MeanReversionStrategy / PairsTradingStrategy / TrendFollowingStrategy /
    │   StatisticalArbStrategy   四个策略插件
    ├── SpreadArbitrageManager.h/.cpp (+Init.cpp)  总编排(B-3 门/in_flight/B1/B5/B6)
    ├── AsyncArbitrageExecutor.h/.cpp              异步执行器(arb 线程/orphan 对冲)
    ├── ArbExecutionBridge.h/.cpp                  主线程执行桥(MdSpi 下单/回报回流)
    └── SpreadRiskManager.h/.cpp                   套利风险管理(drawdown→EMERGENCY)
```

## 4. 模块详解

### 4.1 策略壳层 UftFutuMmStrategy

**职责**：组合根（Composition Root）+ 框架适配层。实现全部 **14 个框架回调**并持大锁串行化；
持有约 20 个业务模块的生命周期；真正的决策在 Coordinator / RuntimeOps，本类是"壳"。
文件尾部 `FutuStraFact` 工厂以 C 接口导出供 Runner 动态加载。

**配置结构 `FutuMmConfig`**（config.yaml `strategies.uft[].params` 直接映射，默认值见 §6）：
`portfolio`(max_delta/hedge_ratio)、`quoting`（档位/价差/数量/黏性/保护/scout/义务参数）、
`risk`（exposure/daily_loss/frequency 频控/恢复预算/同侧熔断）、`closeout`（分钟提前量/重试/
drain/sweep/FAK）、`perf`、`modules` 开关、`order_control`、`monitor`。
命名空间级 `ContractInfo`：code/multiplier/tick_size/**max_position(风控硬顶)**/
**max_delta(策略软限)**/target_position/收盘时间。

**回调链路**：

| 回调 | 线程 | 转发去向与要点 |
|---|---|---|
| `init(cfg)` | — | 仅委托 `FutuConfigLoader::load`（解析+校验拆分） |
| `on_init(ctx)` | 单线程装配 | TradingState 外部锁定 → EventNotifier 注入 → `loadContractInfos`（session 缓存/收盘时间推导/乘数 tick 回填）→ `FutuModuleAssembler::assemble` → 配置交叉校验（信号权重和/GLFT 范围/maxDelta 与 maxPosition 关系告警）→ 热参数注册 + 启动期 applyAll → 实盘启 `_hot_watcher`(1Hz) → `stra_sub_ticks` 订阅 → MonitorBridge 初始化 |
| `on_tick` | MdSpi | 命令检查点(Session 域) → 热参数 drain(`consumePendingApply`) → handleQuotingAutoResume(ERROR 态指数退避：10s 起 ×2 封顶 ~320s) → replay 时钟推进 → handleMarketDataUpdate(markToMarket；**双边均>0 才接受 mid**，锁板半价置 0 防污染) → handleLeadLagPush(anchor tick 推送非 anchor 聚合器) → handleCoordinatorTick(drainTdSpiLogs → coordinator 空则 FAIL-SAFE 全撤 → processTick → closeout_orch.onTick) → arb 喂入门(phase≠CLOSEOUT 且非休息段才喂，防收盘平仓窗口重建价差仓过夜) |
| `on_transaction` | MdSpi | CorrelationManager.onTick + MarketDataContext.onTransaction(L2 方向分类在其内部完成) + perf recordFillReceived |
| `on_trade` / `on_order` / `on_entrust` | TdSpi | 命令检查点(Channel 域) → `FutuRuntimeOps` 全权处理（on_entrust 无 ctx 入参复用 `_main_ctx`） |
| `on_position` | TdSpi | 本地持仓对账：`stra_get_local_position` 与 Portfolio 差 >0.01 时 onPositionUpdate 同步 |
| `on_channel_ready/lost` | TdSpi | postCommand 后**发布者自 drain**（Channel 属 Td 域，保证行情静默期无滞留） |
| `on_session_begin/end` | RtTicker | postCommand(Session 域)；big 模式同回调内执行；none 模式实盘由下一 on_tick(Md) 消费 |
| `on_params_updated` | MdSpi | 组装 HotParamManager::Targets → applyAll 分发到各模块 |
| `on_order_queue/detail` | MdSpi | L2 数据入口（TODO 占位，S9 双通道前置） |

**PendingCommand 命令通道（WS-E）**：4 类命令 `{ChannelReady, ChannelLost, SessionBegin,
SessionEnd}`；post(锁内 push + release 写标志) → 单飞 claim(`exchange`) 消费 → 按属主域分派。
属主域拆分：**Channel=Td 域**（发布者即 TdSpi 自 drain + on_trade/order/entrust 三检查点兜底）；
**Session=Md 域**（实盘 ticker 只投递、仅 on_tick 消费——Session 序列触达 coordinator 的
resetSession/quote_chain 等 Md 属主状态；回测单线程 tid 相等自 drain ⇒ 与 big 模式逐比特一致）。
快速路径一次 acquire load ≈1ns。已知唯一行为差异（仅 none 实盘）：SessionEnd 从收盘时刻内联
改为次日首 tick 边界执行（收盘后无活动订单与行情，benign）。

**其它要点**：
- `_violations_buf`（RiskViolation 复用缓冲）严格 TdSpi 专属；
- `_exchange_time_ms` 为全引擎 replay 主时钟（tick actiondate/actiontime 合成，跨日单调）；
- `_tdspi_log_queue`（SPSC 256）把成交明细日志卸载到行情线程落盘，满则 dropped 计数；
- `_liquidator`（RiskLiquidator）统一强平原语；
- 大锁宏：`#ifndef FUTU_CB_LOCK_BIG / #define FUTU_CB_LOCK_BIG 1`，默认 big 与历史基线一致。

### 4.2 业务逻辑外移点 FutuRuntimeOps

**模式**：8 个静态函数均以 `(UftFutuMmStrategy& s, IUftStraCtx* ctx, ...)` 为首参，函数体内
用引用别名绑定策略私有成员（friend class），使外移后的函数体与原内联实现逐行等价。
锁不在本类持有——由策略壳大锁保证互斥（这也是 friend 的原因）。

| 函数 | 线程 | 业务内容 |
|---|---|---|
| `processTradeFill` | TdSpi | 见 §2.3 fill 路径全流程。重点：方向归一 `is_buy=(isLong==isOpen)`（框架 isLong 是仓位方向非买卖方向）；SideFillBreaker（adds_inventory 判定=pre-fill 持仓方向与买卖同向才算加库存；CLOSEOUT 豁免）；UnifiedNetBook 影子簿记账与漂移检测（真漂移→markShadowStale+resyncPosition；历史事故：净额推断曾致内部账 -370 万假日亏 IRREVERSIBLE halt）；tracker.recordOrderFill 按 original_qty 判完全成交才 untrack（修部分成交后活单消失缺陷）；恢复四道闸——硬违规(POSITION_NET/EXPOSURE/DAILY_LOSS)/IRREVERSIBLE 不动、closeout 平仓期跳过、否则 checkAndRecover 通过后 resumeFromRisk+双解禁+coordinator 软风控复位(riskWiden.reset，否则宽度永久 ×1.5)+复活 arb executor |
| `onChannelReady` | TdSpi | 价格过期置位（至首 tick 前 P1-4 保护）→ arb 复活 → **B+ 锚点清扫**（clearZombies 返回 id 列表→引擎侧 `stra_cancel_all("")` 全撤 + 广播 quoter.onOrder(id,true,0,0,0) 清孤儿槽）→ clearZombieHalts → 逐合约持仓同步（本地持仓为唯一权威、账户侧只对账）→ 非 IRREVERSIBLE 且无违规走完整恢复链；有 POSITION_NET 违规→AUTO REDUCE（getPositionReductionToLimit + RiskLiquidator.reduceContract 对手价 FAK，一次一个超限合约） |
| `onSessionBegin` | ticker/Md | resetDaily + portfolio.resetDailyPnl（修跨日 realized 累计误判）+ force resetCloseout（防 FLATTENING 卡死）+ TradingState.reset + 错误计数清零 + arb_bridge/orchestrator resetSession + 异步 arb 线程启停 + STP clear + 双边统计 onSessionStart + seedBilateralStatsFromFile 重启续算 |
| `onSessionEnd` | ticker/Md | enterCloseout 进平仓相位 → 停 arb → 全部 quoter cancelAll → 双边统计逐合约日终输出 → PerformanceAnalyzer 会话报告 → finalizeAtSessionEnd 强制收尾 → MonitorBridge 收盘终写 |
| `onEntrust` | TdSpi | RISK_HALTED 期间忽略（H 是更高优先级）；成功→计数清零+双边统计在场起点+tryResumeFrom(ERROR)；失败→计数++ + 死单清理（拒单 localid 已注册进槽位，复用 onOrder(canceled) 移除防统计虚高）+ 套利单被拒 cancelByPair+markLegRejected + finalizeOrder；达 orderErrorThreshold → qphase=ERROR + haltTrading(REVERSIBLE) + 全撤。柜台错误分类（CTP 50 平今不足/51 平昨不足/31 资金不足）属框架层职责，策略层只做通用计数（用户裁定） |
| `onOrderEvent` | TdSpi | 撤单计数（replay 时钟基准，修旧 date×86400000 垃圾时间戳 bug）→ quoter 定位 onOrder（终态移槽+untrack）→ **B+ live 事件驱动补挂**（撤单终态 && !backtest → requoteAfterFill(from_fill=false)，回测维持下一 tick 保可复现）→ STP untrack → closeout_orch.onOrderEvent（必须在 router 抹来源标记前调用）→ 终端态 router.onOrderDone + arb pair tag 清理（撤单先 consumePairTag→onLegCancelled 再 onOrderFinalized，顺序敏感） |
| `onChannelLost` | TdSpi | qphase=RISK_HALTED + paused_since + 全撤 + haltTrading(REVERSIBLE,pnl) + arb setEnabled(false)（防断连中信号塞爆队列静默丢弃）+ 持仓快照日志 |
| `finalizeOrder` | TdSpi | M1/M2 幂等清理：拒单无 on_order 终结回调 → 先 markPendingCancel(REJECTED) 再 untrackOrder（顺序保证 fill_rate 不虚高）+ router.onOrderDone（防 CLOSEOUT inflight 守卫永久卡死仓位过夜）；对不存在 id 均 no-op |

### 4.3 主循环编排 StrategyCoordinator

**职责**：MdSpi 主循环编排中枢（约 1500 行）。自身不拥有模块（全部裸指针借入），通过
`wireDeps(CoordinatorDeps 15 指针)` + `validateDeps()` fail-fast 收口。组合对象：
`CloseoutTrigger`（触发）、`RiskCoordinator`（硬风控/taker 减仓）、`QuotePolicyChain`
（六段报价调整链）、`SessionPhaseManager`（时间窗口单一事实来源）。

**核心数据结构**：

- `TickContext`：单 tick 上下文——价格字段（bid/ask/mid/last/timestamp/涨跌停）、一次性解析的
  组件指针（aggregator/book/quoter/spread_opt，消除每阶段哈希查找）、合约快照 `cs`
  （preCheck 入口一次 getContractSnapshot 各阶段复用）、组合级缓存 total_delta/exposure；
- `CachedQuote`：最近 tick 最终报价参数快照（mid/l0_bid/l0_ask/spread_mult/allow_bid/ask/
  decision/upper/lower/best），小锁保护，MdSpi 写/requoteAfterFill(TdSpi) 读；
- `MidSlot/_last_mid`：per-contract `atomic<double>`（unique_ptr 包装抗 rehash），MdSpi 写/
  acquire 读，V8-R4 起 Coordinator 为唯一属主；
- `CoordinatorConfig` 关键键：takerReduceThreshold(代码默认 1.3/yaml 1.1)/TargetUtil(0.8)/
  CooldownMs(30000)、requoteAfterFillMinIntervalMs(200)、sectionBreakSecondsBefore(10)、
  bilateralStatsLogIntervalSec(300)/LogDir、toxicityCooloffMs(5000)、autoCancelMaxAgeMs(10000)、
  cancelRetryIntervalMs(300)/cancelMaxRetries(3)(B+)、alphaSensitivity(2.0)、
  coldStartConfidenceFactor(0.005)；
- 四张 string-key map（`_halt_quoting_state` Md 专属 / `_last_quote_params` 小锁 /
  `_last_requote_ms` Td 专属 / `_last_bilateral_log_ms` Md 专属）。

**processTick 七阶段**见 §2.3。内部细节：

- `preCheck`：nan/inf 防御（IEEE754 nan≤0 为假会绕过 ≤0 校验；EC 教训：预开盘 nan tick 污染
  leg_history → beta=nan 蔓延全局；每 4096 条节流告警）；bid/ask≤0 锁板跳过（触板残留单告警）；
  交易时段判定 HHMM 口径（曾有 `/100` 误判 23:14→00:23 导致全 skip 事故）；
- `updateMarketData`：portfolio.onTick + setShadowFromEngine + `_last_mid` relaxed store +
  组合 delta/exposure 缓存 + `publishPnLSnapshot`（原子 PnL 发布给 arb 线程，lock-free）；
- `updateSignals`：市场暂停 shouldPause → `setQuotingPhase(MARKET)`（进入被 RISK_HALTED 校验
  拒绝安全）/否则 tryResumeFrom(MARKET) 防跨态闪烁；**MARKET 边沿撤销**（P2-2：进入暂停首 tick
  cancelAll 一次，不再每 tick 空转抢 quoter 锁）；毒性喂入构造 AlphaResult（T1 补填
  ofi_component）与 TradeImbalanceResult（T7 补填 large_trade_ratio）→ VPIN analyze →
  `aggregator->updateToxicity(score,side,valid)` 显式入口写入（A6 单写者恢复）；
- `processQuoting`：canQuote 门 → 冷启动检测（confidence < alpha_sensitivity×0.005≈0.01）→
  PortfolioContext（contract_delta_util 带符号注入）→ PreTradeDecision 双层 → GLFT →
  QuotePolicyChain.run → pending_drain 处理（该侧撤单+allow=false）→ CachedQuote 写入 →
  refreshQuotes 返回实际挂单数 n>0 时 `recordOrders(n)` 纳入 ORDER_RATE 频控（P0-2）；
- `processAutoCancel`：checkAutoCancel 返回动作逐个 stra_cancel；zombie 升级处置 = error 日志 +
  setZombieHalt 合约闩锁 + `stra_cancel_all(fullCode)` 引擎兜底（**必须传 fullCode 两段式**
  "SHFE.ag2608"，传三段式 stdCode 框架永不匹配静默不撤）；每 tick retainZombieHalts(存活集合)
  自动释放已清账合约闩锁；
- `requoteAfterFill`（成交/撤单终态后立即重挂）：守卫链（interval/canQuote/!isTradingHalted/
  quoter 存在）→ 义务检查（双边仍有效直接返回，黏性受益）→ 限频 200ms 合并密集成交 →
  **按最新价重算**（mid 取自 _last_mid 按 mid_delta 平移 l0 价；重跑 PreTradeDecision 刷新
  util/obligation/pending_drain；side_pause 检查防 fill→requote→fill 循环；retreat 取更保守价；
  best_bid/ask 用最新盘口刷新，upper/lower 涨跌停盘内不变保留缓存）→ refreshOrders+计数；
- `processSectionBreak`：秒级休息窗（每节结束前 10s，最后一节归 closeout）：进入沿撤全部报价+
  flush 双边统计+按源撤 ARBITRAGE/HEDGING+停 arb；退出沿复活 arb（HALT 时推迟由风控统一恢复）；
- **双边统计落盘**：周期 live 行改内存积压 `_bilateral_io_backlog`（R6-b，MdSpi 热路径零 I/O），
  section-break/收盘 closeout 定点排空；义务行/section 行不受影响；重启经当日文件 seed 续算。

### 4.4 状态机与时间窗 TradingState / SessionPhaseManager / EventDispatcher

**TradingState.h —— 分层状态机（HSM），全字段 atomic，CAS 转移**

```
顶层 MmPhase(2 态):      QUOTING ──┐
                         CLOSEOUT ←┘ (closeout 触发/夜盘恢复/session 复位)
QUOTING 子态 QuotingPhase(5 态, 高优先级覆盖):
    NORMAL / TOXICITY(VPIN·OFI 定时恢复) / MARKET(vol EXTREME 极端波动)
    / ERROR(下单错误指数退避) / RISK_HALTED(硬风控,需显式 resumeFromRisk)
正交方向级软禁: long_blocked / short_blocked (2 位, 两阶段都适用)
```

关键接口（read-check-CAS 循环实现）：

- `canQuote()` = QUOTING && NORMAL；`canBuy()/canSell()` 叠加方向位；
- `setQuotingPhase(q)`：当前为 RISK_HALTED 时仅允许目标 NORMAL（`canTransitionQuoting`
  单一校验源防漂移），其余拒绝；
- `tryResumeFrom(expected)`：CAS 天然实现"仅当当前==expected 才翻 NORMAL"——防止 HALT 期间
  MARKET/TOXICITY 的恢复分支误翻 NORMAL 造成跨态闪烁；
- `resumeFromRisk()`：RISK_HALTED→NORMAL **唯一合法路径**；
- `enterCloseout/exitToQuoting/reset/blockLong/unblockLong/blockShort/unblockShort`。

DEBUG 写者断言（`_writer_tid`）已随原子化废弃，`setExternalLocking(true)` 恒停用仅为过渡观察保留。

**SessionPhaseManager.h —— 时间窗口单一事实来源（纯函数，不含状态机）**

4 相枚举（优先序 CLOSED > CLOSEOUT_WINDOW > SECTION_BREAK > CONTINUOUS）：

- `CLOSED`（非交易时段）/`CONTINUOUS`/`SECTION_BREAK`（中间节收盘前 N 秒休息窗，v7.9 由分钟改秒级）/`CLOSEOUT_WINDOW`（日终/夜盘收盘前触发窗）。
- 配置：section_break_seconds_before=10（0=禁用）、close_time=150000、closeout_minutes_before=5、night_close_time=0(无夜盘)、night_minutes_before=5。
- 关键修复：`inSectionBreak` 必须用 sections 的**原始墙钟节尾**而非 offsetTime 偏移后值（FN0100 等 offset=300min 品种混用会把窗口后移 5 小时导致永不触发）；夜盘窗口跨日品种时间轴归一（00:00–05:59 加 1440）；含八进制误配检测（0230 写成 230）。
- 使用方：StrategyCoordinator 持实例同步配置；FutuRiskMonitor 直调其三个静态函数做 closeout 触发窗判定。

**EventDispatcher.h —— 同步事件分发器（当前休眠基础设施）**

5 类事件 `{TickReceived, FillReceived, RiskAction, TradingStateTransition, CloseoutPhase}`；
dispatch 空 listener 早退；**subscribe 全项目零调用**，仅两处信息性打点。文件内嵌完整的
TradingState 写点审计矩阵（15 写点/6 类/2 线程）——去大锁 WS-C 的既定计划是把分散写者收敛为
事件投递 → 单一 TradingStateWriter 集中 CAS 执行（待独立会话执行）。

### 4.5 GLFT 定价模型 SpreadOptimizer

**职责**：Guéant–Lehalle–Fernandez-T 库存风险做市定价（每合约一个实例）。输入 mid/合约 delta/
SignalContext，输出 skew/spread_mult/l0_bid/l0_ask（第 0 档锚点，阶梯展开由 FutuQuoter 完成）。
旧文档所称 `GLFTModel.h/.cpp` 已不存在，实现全部在本类。

**公式要点**（引用实际变量）：

1. **基础价差** `computeBaseSpread`：`spread = base_spread × depth_adj + phi × (vol_percentile/
   vol_percentile_scale) × vol_scale`；深度调整 `depth_adj = 1/(1 + (avg_depth/
   depth_normalization)×depth_sensitivity×depth_sensitivity_scale)`，无深度数据用
   no_depth_spread_mult(1.5)；clamp 到 [base_spread×min_spread_mult, ×max_spread_mult]。
2. **spread_mult 统一风险放大**：毒性乘子 `1+toxicity_score×toxicity_spread_factor`(score>0.05
   门槛)；低置信度乘子；EMA 平滑(α=0.30)+变化率限幅（上行 +10%/下行 −15%）。
3. **公允价（alpha 集成）**：`fair = mid + alphaSensitivity × alpha × confidence_weight × tick_size`
   （confidence_weight 在 [min,max]=[0.3,1.0] 内随 confidence 线性；截断 ≤ 半价差）。
4. **库存偏移 skew（delta 口径）**：
   - 单合约：`direction = util>0 ? -1 : +1`（多头压 bid 出货）；归一化
     `norm = util^delta_skew_power × inventory_skew_gain`（power 默认 1.5，fastPow 特化省 pow）；
     util≥1.0 时 cap 放开为 `1+skew_cross_max_ticks/half_spread`——**穿越授权**（减仓侧可穿过
     mid 主动减仓，yaml skewCrossMaxTicks=3 且须 < obligationMaxSpreadTicks）；
   - 组合：`excess = |totalDelta|/portfolio_max_delta - delta_skew_threshold`，
     返回 `-sign(delta)×delta_skew_factor×fastPow(excess,power)`；
   - 合成：`portfolioSkewWeight(0.5)×组合 + contractSkewWeight(1.0)×单合约`，总 skew 截断
     ±half_spread（穿越授权时放宽）；
5. **最终报价**：`skew_price = total_skew × tick_size × spread_mult`（故意放大：防御性加宽与
   进攻性出清库存协同）；二次截断后 `bid = fair - half + skew_price`（floor 对齐）、
   `ask = fair + half + skew_price`（ceil 对齐）；交叉保护 bid≥ask → pause_quoting + 回退对称报价。
6. **热更新安全（F20 seqlock）**：`setParams` 两度递增 `_params_seq`（奇=写中）；
   `snapshotParams()` 读到奇版本或版本变化即重试——热更线程并发写 ~200B 参数结构无撕裂。

配置节 `coordinator.yaml modules.spreadOptimizer`：phi=0.20、deltaSkewThreshold=0.3、
deltaSkewFactor=1.5、deltaSkewPower=1.5、max/minSpreadMult=3.0/1.0、confidenceWeightMin/Max=
0.3/1.0、lowConfidenceThreshold=0.3/Factor=2.0、depthSensitivity=0.5(+Scale 0.2/Normalization
100/noDepth 1.5)、volScale=5.0/volPercentileScale=50.0、pauseSpreadMultRatio=0.9、
toxicitySpreadFactor=1.0、inventorySkewGain=1.0、skewCrossMaxTicks=3.0、portfolioSkewWeight=0.5、
contractSkewWeight=1.0。注意 baseSpread 不在此节点——从 config.yaml quoting.baseSpread 经参数
传入（权威单源）。

### 4.6 报价策略责任链 QuotePolicyChain

**职责**：processQuoting 中 GLFT 之后的 6 个调整阶段对象化。执行顺序严格：

```
GLFT(SpreadOptimizer) → RiskWiden → ArbCloseSync → Toxicity → LimitPrice → ColdStart → FillRetreat → 缓存+发布
```

链上传递：`QuoteState`（可变：skew/spread_mult/l0_bid/l0_ask/allow_bid/allow_ask）+
`QuotePolicyContext`（只读输入：code/mid/tick_size/涨跌停/last_price/cold_start + 服务依赖）。

| 策略 | 触发条件 | 动作 |
|---|---|---|
| **RiskWidenPolicy** | 软风控 WIDEN_SPREAD | `tickSoft` 每 tick 无状态重算目标倍数（util≥L2(0.9)→2.0 / ≥L1(0.8)→1.5 / 否则 1.0），util 回落自动归一；`onHardWiden` CAS max 闩锁保底。apply 时 spread_mult 下游是死写（quoter 用 l0 价承载宽度），故绕 l0 中点对称实拉宽 |
| **ArbCloseSyncPolicy** (B2) | 套利平仓意图协同 | `getArbCloseDirection(code)`：方向冲突(kArbCloseConflict, 1:N 场景)→双侧抑制；>0（arb 正在买该腿回补）→抑制 MM ask；<0→抑制 bid——防自己人吃自己单。附带 B6 观测模式（默认关） |
| **ToxicityPolicy** | 毒性流规避 | `toxicity->analyze()` 得分数；is_toxic → 写冷却截止(timestamp+cooloffMs=5000)，按方向语义停边：toxic_side==1(激进买流，知情方吃我方 ask)→allow_ask=false；==-1→停 bid；0→双停。冷却期内即使分数回落维持双停 |
| **LimitPricePolicy** | 涨跌停四级保护 | L0 last 触板→全停（`_touch_active` 集合做日志跳变节流）；L1 距板≤20 ticks→spread_mult×2 并实拉宽；L2 距板≤10 ticks→block 对应加仓侧；L3 锁板(≤0.5 tick)→双边暂停 |
| **ColdStartPolicy** | 冷启动保守报价 | alpha 无效或 confidence<0.01 时 spread_mult 提至 max，并完全重算 l0（符号约定与 GLFT 一致，修复过冷启动窗口 skew 反转 bug） |
| **FillRetreatPolicy** | 成交后退机制 | SelfTradeCalibrator 取 getFillRetreat：买成交后 bid 不高于(成交价−retreat_ticks)、卖成交后 ask 不低于(+retreat_ticks)，防"即挂即成交"死循环 |

已知顺序依赖（登记在案）：RiskWiden 的 tickSoft 无条件重算会覆盖同 tick 早前 hard 闩锁，
而 hard 闩锁在 soft 之后写入故旧语义得以保留——文档承认的同 tick 覆盖顺序依赖。

### 4.7 报价执行器 FutuQuoter

**职责**：单合约多档双边报价执行器。把上游 l0 价按档位展开成价格阶梯，管理每个槽位的活跃
订单 id 集合，执行顶单(sticky)/churn 判定与撤旧挂新。on_tick 内同步内联使用。

**关键数据结构**：

- `QuoteLevel`（槽位）：price/qty/**order_ids**(活跃订单 id 列表，stra_buy 可拆多子单；
  **非空即挂单门禁关闭**) /level_index/is_bid；
- `QuoterConfig`：num_levels/base_spread(tick)/level_step/base_qty/level_qty_multiplier(几何衰减
  0.7)/tick_size/sticky_threshold/improve_retreat_ratio/max_price_deviation/price_protection+
  protect_ticks/use_bilateral_quote/min_valid_qty/qty_decay_factor/obligation_min_qty/
  obligation_max_spread_ticks/obligation_level/scout_qty(v7.2 scout 探测层)；
- `_level_qtys` 预计算数量表（`base_qty×multiplier^i` 下限 1 手，热更新会刷表）；
- `_order_id_to_level` 订单 id→槽位反查表（isMyOrder O(1)）；
- `_bilateral_stats` per-quoter 值成员（R3 v2，废共享单例 N 倍累加错误）。

**三条报价路径**（refreshQuotes 分发，返回实际挂单数供频控计数）：

| 路径 | 条件 | 逻辑 |
|---|---|---|
| **A handleBilateralQuote** | use_bilateral_quote && level 0（当前生产禁用） | stra_quote 双边接口；sticky 判定**从尾向头扫描**找最新 live 非 pendingCancel 单（B+ P2-4 修复，旧取 order_ids[0] 会撞撤单飞行期残留致 churn 风暴）；撤旧 markPendingCancel 后 id **保留在槽内**等终态移除；单侧成功也登记防孤儿单 |
| **B1 handleObligationQuote** | level==obligation_level | 条件式重挂（替代旧每 tick 全撤重挂）：sideNeed = 无单 ∥ 全侧深度 < obligation_min_qty ∥ 头部 id 死/pendingCancel ∥ sticky 超限；cancel-only 分支（allow 阻断但槽内有单→只撤不挂）；重挂仅当槽空（撤单飞行中本轮只撤，由 live 补挂或下 tick 补齐——B+ 门禁核心） |
| **B2 handleFlexibleQuote** | 其余自由层 | 逐侧独立：qty==0→cancelLevelOrders(INVENTORY_LIMIT)；需更新→撤后槽空才挂新；允许单边 |

**价格生成**：

- 阶梯价：`bid=floor((l0_bid-offset)/tick)*tick`、`ask=ceil(...)`，offset=level×level_step×tick；
- 义务层恒 base_qty 双边不衰减；义务带（交易所最大报宽）由 obligation_max_spread_ticks 收口；
- force_ask/bid_obligation（delta 打满强制义务减仓）把对应侧收进义务带并置义务标记；
- 自由层：义务期间整体撤出；scout 层 qty=min(qty,scout_qty)；delta util 方向衰减
  `exp(-qty_decay_factor×util)`（F8：每 tick 只算 2 次）；block_add 加仓侧 qty=0；
- `applyPriceProtection`：硬性防穿越市场（bid≤best_ask-tick 等）；软保护 protect_ticks（非义务层）；
  validatePrice 校验 NaN/Inf/正数/涨跌停/偏离 mid 上限任一失败 qty=0。

**sticky/churn 判定** `checkStickyUpdate`：不对称黏性——bid 向上(改善)容忍
improve_retreat_ratio×threshold(默认 2.0)，向下敏感 1.0×；ask 镜像。显著降低撤单率。

**B+ PendingCancel 门禁** `cancelLevelOrders`：逐 id `tryMarkPendingCancel` 成功才发 stra_cancel
（原子取得撤单权防双发）；失败静默跳过（超时重试/zombie 由 tracker 接管）。**id 不从槽移除**
——只有 onOrder 终态移除。"发送即遗忘"是 2026-08-19 僵尸单事故根源（已修）。

**回报入口**：`onOrder`（终态→删 id+untrack+recomputeLevelQty；部分成交→updateQty+Post-submit
cancel 检查：订单进入 UnTrd 但该侧被 allow 阻断→补撤）；`onEntrustAck`（在场时间起点，建模网络
延迟）；`onScoutFillCancelObligation`（自由内层成交→撤同侧义务层全部挂单，scout 成交=逆向信号）；
`getValidQuoteSnapshot`（从最优档累计加权到 min_valid_qty，末档截量——双边义务满足度判定依据）。

### 4.8 订单全生命周期跟踪 UnifiedOrderTracker

**职责**：全部订单状态的单一真相源。MM 单与套利单统一跟踪、O(1) 按 id 查询、按合约索引、
自成交检测、STALE 自动撤销、**B+ 撤单超时重试与 zombie 升级**。公开方法统一由
`RecursiveSpinLock _lock` 守卫（Md checkAutoCancel vs Td track/untrack/recordFill 双线程）。

**关键类型**：

- `OrderFlags`：PENDING_CANCEL / IS_BID / IS_ACTIVE / IS_MM_ORDER / IS_ARB_ORDER /
  **IS_ZOMBIE**(撤单重试 K 次仍无 ack：保留跟踪+计入 pending+升级处置)；
- `UnifiedOrderInfo`：定长 char code[32]、price/qty/filled_qty/original_qty（original 供完全成交
  判定不被部分成交改写）、place_mid/place_time/last_check/**cancel_time(B+ 最近撤单发送时刻)**/
  cancel_retry_count；
- 存储布局：连续 vector + free list 空槽复用 + wt_hashmap id→index + generation 号（调用方可据
  此跳过快照深拷贝）+ per-contract 索引（含 MM 买卖分表供 STP O(1) 反查）+ F9 增量 pending 表 +
  zombie 三件套（升级列表/存活集合/去重表）。

**pending 数量统计双口径（刻意区分）**：

- `getPendingBuy/SellQty`：MM 索引口径，只数 isActive && !pendingCancel（实时口径，用于
  STP/报价深度判断）；
- `getPending*QtyAllSources`：全源增量口径，含 MM+arb 全部 ACTIVE 单且 **B+ 起含
  PENDING_CANCEL**（保守风控口径——撤单飞行期仍视为在途防双份敞口），track(+)/untrack(-)/
  updateQty(delta) 增量维护 O(1)。旧口径只看 MM 索引导致 arb 建仓期 util 系统性偏低。

**checkAutoCancel（MdSpi 每 tick）**：

1. B+ 撤单超时重试：active&&pendingCancel 单，`now-cancel_time ≥ 300ms` → retry_count<3 时
   重试动作+刷新 cancel_time+count++；否则 `setZombie()`（永不 force-untrack 活单）+ 首次进入
   升级去重表 + 加入升级列表；
2. 存活 zombie 集合维护：zombie 清零的合约从去重表删除（重新武装升级，同合约第二个 zombie 可
   再次告警）；
3. STALE 自动撤销：age > 10s 且价格偏离 ≤ sticky_threshold×2 ticks 先延长寿命 50%，超扩展期
   push STALE action 就地置 pendingCancel。

**zombie 相关 API**：`getZombieEscalations()`（本次新升级列表）/`getAliveZombieContracts()`
（喂给 retainZombieHalts 释放闩锁）/`clearZombies()`（通道恢复锚点：untrack 全部 zombie、返回被
遗忘 id 列表——调用方须引擎侧补全撤+广播 quoter.onOrder 清孤儿槽，防残留 id 永久关闭门禁）。

**自成交检测** `checkSelfTrade`：arb 买 ↔ 冲突 MM 卖单价 ≤ arb 价（卖对称）；市价恒冲突；
**排除 PENDING_CANCEL**（先撤旧再挂新的 cancel-ack 窗口不误拒 arb）；命中建议 CANCEL_MM_FIRST。

**成交记账** `recordOrderFill`：以 original_qty 判完全成交（防 updateOrderQty 改写导致的提前
untrack），部分成交保持跟踪（残留活单继续参与 STP/在途量/sticky）。

### 4.9 下单路由与守卫 OrderRouter / OrderApiGuard 等

**OrderRouter.h/.cpp —— 非 MM 来源下单路由**

只为 arb/hedge/closeout/taker-reduce 服务（MM 单绕过直连 ctx API 零开销）：自成交防护、
分源限速、pair 粒度撤单、审计追踪。延迟预算 <500ns/单。

- `Source` 枚举（OrderTypes.h）：**ARBITRAGE=0 / HEDGING=1 / CLOSEOUT=2 / RISK_REDUCE=3**
  （V8-R6/P2-3 新增 RISK_REDUCE——taker 减仓不再冒用 CLOSEOUT 口径）；
- `submitBuy/submitSell`：价格校验→滑窗限速（ARBITRAGE/HEDGING 各 30 单/s，CLOSEOUT 不限）→
  STP 检查（委托 tracker.checkSelfTrade）→ orderApiCall(stra_buy/sell)→多子单逐个 recordActive；
  返回 `OrderSubmitResult{localids, rate_limited, self_trade_blocked, rejected}`；
- `submitExitLong/ExitShort` 平仓方向映射 STP 对照侧；`cancelAllBySource(src)` HALT/休息段清扫
  核心（未标 pending_cancel 者置位后撤）；`registerPairOrder/cancelByPair` A7 pair 映射撤单；
- `onOrderDone(localid)` swap-back O(1) 移除；`getActiveCountBySource`（排除 pending_cancel）
  是 CloseoutExecutor "上一批已结清"守卫的依据。

**OrderApiGuard.h —— 框架发单互斥**

框架 UftStraContext 容器非线程安全，Md/Td 两线程并发 stra_* 是框架级竞态。全局函数级静态
`std::recursive_mutex`，模板 `orderApiCall(fn)` 锁内执行一次调用（临界区亚 μs）。锁序规约单向：
各结构锁可获取它，其内不得再取结构锁。

**IOrderSink.h —— 窄接口解耦**：4 方法纯虚接口（submitBuy/submitSell/cancelAllBySource/
cancelByPair），让风控/平仓消费者依赖窄接口便于 Mock 单测。FutuQuoter 有意绕过（直连求最低
延迟）；ArbExecutionBridge 因需 registerPairOrder 保留具体 OrderRouter*。

**TdSpiOffload.h —— TdSpi 日志卸载**：`TdSpiLogEvent`（三个 FixedString24 定长串，static_assert
强制 trivially copyable）经 SPSC LockFreeQueue<256> 把成交回报路径的非紧急日志推迟到行情线程
落盘；requoteAfterFill 与簿记保持同步不卸载。

### 4.10 组合管理 FutuPortfolio / CorrelationManager

**FutuPortfolio.h/.cpp —— 组合持仓 SSOT（由 InventoryManager 合并而来）**

MM+ARB 合并记账的唯一事实源，全方法 `RecursiveSpinLock` 守卫。

- `ContractState`（per-contract，热字段前置首 32 字节防多 cache line 扫描）：position/hedge_ratio/
  multiplier/last_price/code/tick_size、prev_position/unrealized_pnl/realized_pnl、
  **UnifiedNetBook 影子簿**（shadow_net/shadow_realized_pnl/shadow_unrealized_pnl/shadow_stale
  ——只记录不决策，权威是引擎本地净仓与盈亏）、bid1/ask1/daily_pnl/max_position(硬顶)/
  target_position/contract_max_delta(软限)；
- 内联派生：`delta()=position×hedge_ratio`（等效手数口径）、`exposure()`=|pos-target|×乘数×价格
  （扣除目标持仓；last_price≤0 返 0）、`isPositionLimitBreached()`（严格大于才算 breach）；
- 聚合指标（F4 dirty 缓存：mutator 置脏，同 tick 多次 getter 共享一次 O(n) 扫描）：
  getTotalDelta/getNetDelta/getTotalExposure(max(long,short))/getTotalGrossExposure(sum，
  跨品种不可对冲用)/getTotalUnrealizedPnL/getTotalPnL；
- `getRawPortfolioDeltaUtilization`（2026-08-19 语义边界后全链路唯一组合 delta 利用率口径——
  原始持仓 delta / portfolio_max_delta；净口径仅限 closeout 偏离语义）；五级 RiskLevel 分档
  (<50%/<70%/<85%/<95%/≥95%)；
- 持仓更新入口：onTick/markToMarket/updateDailyPnL/resetDailyPnl/onPositionUpdate/updatePosition/
  **onTradeFill**（offset 标志驱动分向成本簿记账，替代净额推断）/resyncPosition（引擎净仓真值再
  同步防漏单漂移）/setShadowFromEngine/markShadowStale/hasStaleCostBasis（成本簿不可信时日亏
  降级 REVERSIBLE 处理的依据）；
- **B5 过冲保险丝回调**：checkOvershootSignFlip 在 onPositionUpdate/updatePosition 两入口检测
  sign-flip(+N→-M)，且该 leg 有活跃 arb 平仓 intent 时 → onOvershootDetected（无 intent 的 flip
  是 MM 正常做市不报）；
- **PnL 快照跨线程通道**：publishPnLSnapshot(Md 每 tick) → atomic<double> → arb 线程 getSnapshot*
  读（lock-free ~10ns），供 SpreadRiskManager drawdown 计算；
- hedge_ratio 冷启动初始化：hedge_ratio_initialized=false 时 on_tick 用纯货值比注入，之后 EMA β。

**CorrelationManager.h/.cpp —— 相关性/hedge_ratio**

- 全配对 addRelation，复用 SpreadCalculator 滚动窗口计算核心（window=100/min_samples=10），
  beta 截断带围绕 expectedBeta 设置 [×0.3, ×3.0]；
- F1/F2 索引优化（code→calculators 预建、双向 pair_index 零堆分配查询）；
- nan/inf 价格防御（脏 tick 丢弃防污染 beta）；
- `getHedgeRatio` 分叉语义：同码/未注册→1.0；**跨期 CROSS_TERM→恒 1.0**（做市 max_position 按
  手数设限，delta 直接等于净手数）；跨品种→货值等价 `(p1×m1×beta)/(p2×m2)`，方向对齐取 1/beta，
  截断 [0.05,20]，非法回退 1.0；
- 消费点：策略层对非锚定合约每 tick 取对 anchor 的 hedge ratio 与相关系数。

### 4.11 风控子系统

呈"三层漏斗 + 一个执行原语"结构：

```
每 tick (MdSpi):
  RiskCoordinator::checkRisk ── 组合级硬风控中枢(HALT/BLOCK/WIDEN + 恢复)
    ├─ FutuRiskMonitor::checkRiskLimits ── 组合级违规(EXPOSURE/DAILY_LOSS/RATE×3/POSITION_NET)
    ├─ RiskWidenPolicy.tickSoft ── 策略性加宽(util 0.8→×1.5 / 0.9→×2.0, 每 tick 无状态重算)
    ├─ RiskCoordinator::checkTakerReduce ── 合约级紧急减仓(util≥阈值 FAK 对手价)
    └─ FutuRiskMonitor::checkPreTradePosition ── 合约级盘前双层决策
         ├─ RiskVerdict   = checkHardPositionRisk      [position 口径风控闸门]
         │    zombie 闩锁 → SideFillBreaker.isPaused → pending_drain → |净仓|>maxPosition halt
         └─ StrategyInputs = computeInventoryStrategyInputs [delta 口径策略输入]
              long/short_delta_util(含 pending 投影) → util≥1.0 强制义务减仓报价
              |delta|≥maxDelta×ratio → block_add(flexible 停加仓)

成交回报 (TdSpi): processTradeFill → SideFillBreaker.onFill(减仓只打断不累计; CLOSEOUT 豁免)
不可逆兜底: IRREVERSIBLE halt → RiskLiquidator.forceFlatAll(对手价 FAK 全组合强平)
```

#### 4.11.1 FutuRiskMonitor.h/.cpp —— 风控监控核心

**枚举**：`RiskLimitType`(POSITION_NET/DELTA/EXPOSURE/DAILY_LOSS/ORDER_RATE/CANCEL_RATE/
TRADE_RATE)；`RiskSeverity`(WARNING/BREACH/CRITICAL)；`RiskCategory`
(**REVERSIBLE** 可自动恢复 / **IRREVERSIBLE** 日亏线需人工 clearIrreversible)；
`RiskAction`(NONE/WARN/WIDEN_SPREAD/BLOCK_SIDE_LONG/SHORT/…/HALT_TRADING；
PAUSE_QUOTING 与 FLATTEN_POSITION 为 v7.3 数学不可达死分支保留占位)。

**频控环**：三环 LockFreeRingBuffer<uint64_t,256> 存时间戳，`_rate_lock`(atomic_flag) 保护
（V8-P0-2 修复 SPSC 多生产者违规）；`recordOrders(n)` 批量接口供 MM 路径计数（closeout/
liquidator **有意不计数**——紧急路径不应被频控 HALT 卡死）；pruneRateWindows 读侧剔除过期样本
（修复停报单后旧时间戳永不过期导致 RATE 误报持续）。

**checkRiskLimits（组合级违规检测）**：Delta 段纯软日志（util≥0.8 warn skew 将调整 / ≥1.0 warn /
≥1.5×critical——真正的 delta 控制交给连续控制层）；EXPOSURE 硬违规（毛暴露>max_exposure）；
DAILY_LOSS（pnl < -max_loss，唯一 IRREVERSIBLE 来源，成本簿 stale 时降级 REVERSIBLE）；三类
RATE；POSITION_NET 单合约 breach 快照。

**determineActionWithCategory（violation→action 分级）**：
DAILY_LOSS·CRITICAL→IRREVERSIBLE HALT（cost_basis_stale 降级 REVERSIBLE）；其余 CRITICAL→
REVERSIBLE HALT；BREACH 的 DELTA/EXPOSURE 按符号→BLOCK_SIDE_LONG/SHORT；breachCount≥1 的
WARNING→WIDEN_SPREAD；纯 WARNING→WARN。

**恢复机制（halt/resume/checkAndRecover/canRecover）**：

- `haltTrading(category,pnl)`：置 halted+category(atomic)+timestamp+pnl 快照+loss 标志；
  **不重置 _recovery_count**（否则每 session 上限形同虚设），计数只由 resetDaily 重置；
- canRecover 条件全集（顺序判定）：IRREVERSIBLE 永不自动恢复 → recovery_count≥3(每 session
  预算) → cooldown 30s → **刻意不查 delta_util**(Fix2：HALT→无法交易→delta 降不下来→永不恢复
  死锁，2026-08-17 实盘曾以 util=1.7 挡住全天 2232 次恢复检查) → exposure_util≤0.8 → 任一合约
  |pos| ≤ maxPos×positionBreachPauseThreshold(1.2)（**放宽门槛**：严格要求 pos≤max 会死锁，恢复
  报价后由义务软限自然减仓）→ loss 型要求亏损回补 50%(pnl_recovery_ratio) → 可选绝对上限
  max_loss_for_recovery；
- checkAndRecover：check_interval(5s) 节流内执行恢复：resumeTrading(count++ 打印 #n/3)+
  resumeQuoting+双解禁；
- resetDaily/auto_clear_irreversible_on_reset 门（默认 false；true 仅回测模拟隔夜人工复核，
  生产必须 false——IRREVERSIBLE 跨日界保持停机）；
- WS-A 收编：`_halt_category` 改 atomic acq_rel；`_halt_timestamp/_pause_timestamp/
  _recovery_count/_halt_pnl_snapshot/_was_loss_triggered/_closeout_state` 收进
  `_halt_domain_lock`(RecursiveSpinLock)，覆盖嵌套链（checkAndRecover→resumeTrading/canRecover、
  mark*→transitionCloseoutSub、resetSession→resetDaily）。写者含 Md(checkRisk)、Td(onEntrust/
  onChannelLost)、**arb 线程(handleRiskAlert EMERGENCY——曾是无锁真竞态 V9-P1-1)**；
  `getCloseoutSubInfo()` 改按值返回锁内拷贝。

**zombie halt 闩锁集合**：`_zombie_halt` map + `_zombie_halt_lock`。setZombieHalt（zombie 升级时）
→ 该合约 halt_quoting 不允许新增敞口（独立于 maxPosition 闸门）；clearZombieHalts（通道恢复）；
retainZombieHalts(alive 集合)（processAutoCancel 每 tick，zombie 清账合约无需等重连自动恢复报价）。

**checkHardPositionRisk（position 口径硬闸门，逐条）**：
① zombie 闩锁命中→halt_quoting；② 未配 max_position 提前返回（闸门失效但不误伤）；③
side_pause 双向镜像同一合约级熔断态（有意为之）；④ pending_drain：该侧 pending >30 手→撤该侧
旧单+跳过本轮报单（流控排水）；⑤ **|净头寸|>max_position 严格大于→halt_quoting**：入口 cancelAll+
不挂新单，每 tick 重估回落即自动恢复。

**computeInventoryStrategyInputs（delta 口径策略输入）**：
投影公式 `projected_long = max(delta,0)+pending_buy×hedge_ratio`，除以 contract_max_delta 得
long/short_delta_util；util≥1.0 → force_ask/bid_obligation（多头打满 ask 侧义务减仓报价）；
|delta| ≥ maxDelta×positionHardBlockRatio(1.0) → block_add_long/short（flexible 停加仓——策略
库存管理行为非风控措施）。未配软限时返回默认不误伤。

**Delta 速率监控**：时间加权累积变化算法（窗口内相邻快照 |Δdelta| 累积 ÷ max(跨度,window/2)，
修旧端点差把单笔 5 手放大成 5000/s 永久卡死）；breach 标志 detection-only（B3：TradingState
转移统一由 Coordinator 管理，避免双恢复路径）。

**收盘平仓状态机（SSOT）**：`CloseoutSub` 枚举 IDLE→TRIGGERED→DRAINING→ASSESSING→EXECUTING→
COMPLETED/FAILED→RETRYING，合法转移表 canTransitionTo 白名单校验；checkCloseout 双触发点
（夜盘窗口 inNightCloseoutWindow / 白盘窗口 inDayCloseoutWindow，均只有下界防重复靠状态门）；
markCloseoutTriggered 附带 pauseQuoting；FAILED retry 预算耗尽告警升级"manual intervention"；
resetCloseout(force) session_begin 专用硬清。

**告警体系**：broadcastAlert = WTSLogger.warn("[RISK]") + EventNotifier 外发（TRADING_HALTED/
RESUMED、LOSS_CRITICAL、EXPOSURE_BREACH、POSITION_BREACH、DELTA_RATE_BREACH、CLOSEOUT_*、
IRREVERSIBLE_CLEARED 等，关键告警 1s/5s 节流防刷屏）。另注意：`setAlertCallback` 属
SpreadArbitrageManager（arb 线程回调），EMERGENCY→haltTrading(IRREVERSIBLE)+arb disable（见 §4.15）。

#### 4.11.2 RiskCoordinator.h/.cpp —— 组合级风控协调

- `checkTakerReduce`（taker 紧急减仓）：门 threshold(yaml 1.1/代码默认 1.3，0=禁用)；
  Phase1 锁内 forEachContractState 收集（util=|pos|/max_position≥threshold 且 cooldown 到期，
  target=maxPos×0.8，qty=floor(|pos|-target) 经 clampReduceQty 截断不开反向仓）；Phase2 锁外
  Source::RISK_REDUCE 提交（对手价 bid1/ask1）；rate_limited/self_trade_blocked warn 下轮重试。
  由 coordinator 每 tick 调用（HALT 时恰是最需要强平的时点，故 Stage4 失败分支仍执行它）；
- `checkRisk`（每 tick，返回 qphase!=RISK_HALTED）：先前已 halt 分支（closeout 窗口内禁止自动
  恢复 T2）→ checkAndRecover；delta-rate breach → RISK_HALTED+全撤（价格急动时场上旧义务单最
  危险）；tickSoft 主 widen 通道；hard check violations 非空→动作分发：HALT_TRADING 清扫清单 =
  qphase→RISK_HALTED + risk_monitor->haltTrading + cancel_all_quotes + Router 按源撤
  CLOSEOUT/HEDGING/ARBITRAGE/RISK_REDUCE 四类 + IRREVERSIBLE→forceFlatAll（closeout 窗口内
  defer 防超卖）+ arb disable；BLOCK_SIDE→blockLong/Short+pauseQuoting+arb disable；violations
  空→统一恢复路径（仅当 qphase==RISK_HALTED；delta-rate 期间禁止恢复）resumeFromRisk+unblock+
  resumeQuoting+riskWiden.reset+arb re-enable；toxicity cooloff → setQuotingPhase(TOXICITY)+
  calibrator.decayCalibration。

#### 4.11.3 配套组件

- **RiskLimitsConfig.h**：`RiskRateLimits` 单一配置来源（消除两套重复定义）。字段与默认值：
  max_orders/cancels/trades_per_sec=50/30/20、max_delta_change_per_sec(代码 3.0/yaml 50)、
  delta_rate_window_sec/cooldown_ms、position_breach_pause_threshold=1.2（恢复门槛）、
  delta_critical/warning_mult=1.5/0.8、position_warning_l1/l2=0.8/0.9(WIDEN 两级)、
  position_hard_block_ratio=1.0（键名保留兼容，语义已改 delta 口径）、widen_threshold=1、
  同侧熔断三参（代码默认 5/yaml 3、3000ms、5000ms）；
- **PreTradeDecision.h**：双层结构逐字段含义见上文漏斗图。分层准则：RiskVerdict 回答"能不能做"
  （position vs maxPosition），StrategyInputs 回答"怎么做"（projected delta utilization，
  分母 contract_max_delta）。pending 从 tracker AllSources 口径取分别喂两层；不按 max_position
  早退（硬顶未配不得误伤库存调控输入）；
- **SideFillBreaker.h**：同侧连续成交熔断器（背景事故：6 秒 20 笔同向买入循环 delta 11→49 触发
  HALT）。onFill 逻辑链：禁用门→暂停期内不累计（到期整体重置）→反侧成交打断当前序列→
  **adds_inventory=false（减仓成交）只打断不累计**（Fix3：熔断正确回补交易形成棘轮）→窗口过期
  重启→count≥阈值(3)→pause_until=now+5000ms 返回 true（调用方立即撤该合约全部报价）。
  内部 atomic_flag 自旋锁（Td 写/Md 读）；CLOSEOUT 活跃期整体豁免；
- **RiskLiquidator.h**：统一强平原语（消灭"对手价平仓"重复实现）。`clampReduceQty` 自由函数
  （所有减仓路径必须经过——旧截断到组合净 delta 曾致"平仓又开仓"事故）；forceFlatAll 逐合约
  实际持仓（修旧只平 anchor）；reduceContract 三级价格校验(last/bid1/ask1>0)+FAK 对手价+
  Source::CLOSEOUT 审计；
- **SelfTradePrevention.h/.cpp**：UnifiedOrderTracker 的轻量包装层（legacy API 兼容外壳），覆盖
  MM 双边限价单 vs 套利单互撞。StpConfig{enabled/stpMinPriceGap=1.0 tick/allowSamePrice=false/
  priceAdjustTicks}；strategy 枚举(REJECT_ARB/CANCEL_MM/ADJUST_ARB_PRICE)未配置驱动，真实处置
  由 tracker 固定给 CANCEL_MM_FIRST。OrderRouter submit 内还有第二道 STP 门（self_trade_blocked）；
- **SelfTradeCalibrator.h/.cpp**：以自家成交为 ground truth 的毒性标定器 + fill retreat 机制。
  recordFill(Td)→RingBuffer<SelfFillRecord,128>+retreat 状态；onTick(Md)→analyzeFills（惰性缓存）
  ：adverse 判定=买后 mid 跌超 move_threshold_ticks(1.0)（卖对称），聚合 toxicity_level/
  direction_bias/high_toxicity(>adverse_threshold 0.6)/confidence(sample/(min_samples×2))；
  decayCalibration（毒性冷却窗口持续淘汰旧成交自然归零）；getFillRetreat：冷却期内
  bid_retreat=last_buy_price−retreat_ticks×tick(配置 3 ticks)、ask 对称，冷却到期清零记录重新起算。
  V8-R6 收官：`RecursiveSpinLock _lock` 全方法收编（recordFill=Td / onTick=Md /
  **getFillRetreat=Md+Td 双调用点**）。

### 4.12 收盘平仓三件套 CloseoutTrigger / Orchestrator / Executor

分工：Trigger 负责"何时触发"，Orchestrator 负责"衔接编排"，Executor 负责"怎么执行"，
FutuRiskMonitor 负责"状态机白名单校验"（CloseoutSub SSOT）。

- **CloseoutTrigger.h/.cpp**：processTick Stage0 调用；内部调 risk_monitor.checkCloseout(current,
  closeTime)（夜盘/白盘双触发点）触发时：markCloseoutTriggered（TRIGGERED+pauseQuoting+广播）+
  cancelAll 回调（延迟 N=2 个 tick 后启动 executor，让在途单先 settle）+ flush 双边统计 lambda。
  活跃平仓子态(DRAINING/ASSESSING/EXECUTING/RETRYING)下仍跑 checkRisk——平仓窗口不是风控盲区(T2)。
- **CloseoutOrchestrator.h/.cpp**：Deps 注入十项(executor/risk_monitor/portfolio/trading_state/
  order_router/quoters/anchor_code/close_time/flatten_position/strategy_id)。onTick：触发判断→
  延迟启动计数→executor.run→状态同步；executeHedge 一次性启动；onOrderEvent 跟踪 closeout 订单
  回报（在 router 抹来源标记前调用）；finalizeAtSessionEnd 非终态强制收尾(markCloseoutFailed+
  清守卫)；resetSession 复位。`RecursiveSpinLock _lock`（Md/Td/ticker 三线程）。
- **CloseoutExecutor.h/.cpp**：紧迫度驱动的渐进平仓执行器，四阶段：
  - Phase1 DRAIN：等 inflight 结算（drain_timeout_ms=3000 或 router 无 CLOSEOUT 活跃单）；
  - Phase2 ASSESS：读 net delta 算 remaining（clampReduceQty 截断）；
  - Phase3 EXECUTE：迭代 FAK 批次，紧迫度 `urgency = |remaining|/(time_left×fill_rate)` 选价格档：
    PASSIVE(同侧+1tick, depth≤30%) → MID(mid, ≤50%) → AGGRESSIVE(对手价, ≤80%) →
    VERY_AGGRESSIVE(对手+1tick) → SWEEP(对手+N ticks, 剩余<5s)；批量 qty=min(depth×ratio,
    max_batch_size=20)；fill_rate 从近期 round 估计，连续零成交轮追价；
  - Phase4 VERIFY：确认 flat → COMPLETED；超时/预算耗尽 → FAILED（retry_interval 后 RETRYING，
    maxRetries=10/retryIntervalMs=2000）。
  use_fak=true 全部 FAK；涨跌停保护（P2-2 upper/lower 校验）。

端到端时序：session_end（或白盘 closeout_window）→ TRIGGERED → 撤全部报价 → 等 2 tick →
start(close_time_ms,hedge_ratio) → run 循环至 COMPLETED/FAILED → finalizeAtSessionEnd 兜底 →
session_begin force reset。closeout 平仓单走 Source::CLOSEOUT（Fix4：豁免 REVERSIBLE halt——
收盘减仓不被暂停锁死）；bridge 在 DRAINING/ASSESSING/EXECUTING 期间冻结套利新开仓(A12)，
COMPLETED/FAILED 后解冻孤儿腿对冲。

### 4.13 信号因子层 signals/

架构：**单数据源(MarketDataContext) + 插件式信号源 + 槽位表驱动聚合**。

```
WTSTickData
 ├─ MarketDataContext (Facade, 唯一数据源)
 │   ├─ OrderBookStateTracker ← onTick(): 静态盘口(5档/失衡/深度/流动性)
 │   └─ TradeFlowTracker ← onTickInference()/onTransaction(): 动态成交流
 │        └─ TickTransactionInferer (Lee-Ready/tick-rule 方向推断)
 └─ SignalAggregator (每合约一个)
      ├─ 五路加权信号: OFI / TradeFlow / BookImbalance / Momentum / LeadLag
      ├─ 辅助信号: VolatilitySignalSource (不参与加权 → widen/pause 分档)
      └─ AdaptiveWeightFramework (三层动态权重)
ToxicFlowDetector = Predictive(VPIN+OFI+Alpha 每 tick) + Realized(自成交校准 成交驱动)
   → SignalAggregator::updateToxicity 显式回写
```

#### 4.13.1 ISignalSource.h —— 接口与 Result 结构

枚举 `SignalType`(OFI/VOLATILITY/TRADE_FLOW/BOOK_IMBALANCE/ALPHA/MARKET_STATE/TOXICITY/
MOMENTUM/LEAD_LAG/CUSTOM)、`VolTier`(LOW/NORMAL/ELEVATED/EXTREME)、`MarketState`。
全部 Result 继承 `SignalResult{type,confidence,timestamp,valid}`：

- `OFISignalResult`: ofi([-1,1])/bid_pressure/ask_pressure(互补 [0,1])/cumulative_ofi；
- `VolatilitySignalResult`: realized_vol/composite_vol/vol_tier/vol_percentile([0,100])；
- `TradeFlowSignalResult`: net_flow/net_flow_normalized/buy·sell_volume/large_trade_ratio/
  avg_trade_size；
- `BookImbalanceSignalResult`: simple_imbalance/depth_imbalance(距离加权)/pressure_intensity/
  bid·ask_dominant/depth；
- `AlphaSignalResult`: alpha + 五分量(ofi/trade/book_imbalance/momentum/lead_lag_component) +
  is_strong_signal；
- `ToxicitySignalResult`: toxicity_score/toxic_detected/vpin/toxic_side。

`SignalContext` 单一真相源分四段：基础行情快照 / 盘口信号 / 七个 Result 子结构 / 便捷判定
（shouldPause()=market_state 或 toxicity；shouldWiden()；getEffectiveVol()）。
`updateToxicity(score,side,detected)` 为显式写入口（V8-A6，恢复单写者——替代非 const getContext
反向裸写）。

#### 4.13.2 SignalAggregator.h —— 聚合器

每合约一个。update(book) 主流程：tick 计数→拷贝行情进 ctx→各信号源 update→extractSignalResults
→computeAlpha→computeMarketState（should_widen=realized_vol>elevated；**should_pause=
vol_tier==EXTREME 每 tick 重算非锁存**）。is_ready = tick_count≥warmupTicks(yaml 20)。

**computeAlpha 核心流程**（R4b S1/S2 收拢后 ~110 行线性）：

1. **槽位提取**：五路 SignalSlot 表驱动（source 裸指针缓存 + 静态权重成员指针 + normalize 开关 +
   extract/set_component lambda）；LEAD_LAG normalize=false（LL 大部分 tick 是重复值、anchor tick
   低频，p95 归一会爆炸放大噪声）；
2. **IC 簿记 + regime 检测 + 动态权重**：单入口下沉至权重框架 processTick(slot_vals, enabled[5],
   mid, vol_percentile, avg_depth, is_cross_term, out_weights)；
3. **加权聚合**：normalize? RollingScaleTracker p95 幅度归一（窗口 500/每 20 tick O(n) nth_element，
   防 Mom/LL 小幅信号被 OFI 饱和信号淹没）：norm=clamp(raw/p95,±1)；w=动态权重（无效回退静态）；
4. **Fallback**：weight_sum≤0 且 book valid → EWMA(prev×0.7 + imbalance×0.3) 防 alpha 瞬跳；
5. **confidence** = consistency(方向一致占比) × strength(加权均值绝对值) × warmup_factor；
6. is_strong_signal = |alpha| > strongThreshold(0.7)；[SIGNAL_DECOMP] debug 日志 %50 降采样。

配置解析（coordinator.yaml modules.signalAggregator）：signals.* presence 即开关；model.type 经
SignalCombinerRegistry 校验（仅 "linear"，失败整个 aggregator 不初始化）；orphan weight 告警；
warmupTicks=20。

#### 4.13.3 ICWeightTracker.h —— 三层自适应权重框架

设计哲学："IC 低 ≠ 信号失效"——权重有地板(0.05)有天花板(0.50)，永不归零也永不独大。

- **Layer1** 基础逻辑权重（yaml model.weights 注入：ofi .35/trade .25/book .20/mom .15/ll .05）；
- **Layer2** regime 因子 `MarketRegime::detect(vol_percentile, short_ma20, long_ma60, avg_depth)`：
  vol <20 LOW/<60 NORMAL/<80 HIGH/≥80 EXTREME；trend |MA 比值-1|>0.002 TRENDING；liquidity
  depth>50 DEEP/>10 NORMAL/THIN。getRegimeFactor 表：OFI THIN→0.5/DEEP→1.5；TRADE vol 高位
  1.3/低位 0.7；BOOK DEEP→1.3/THIN→0.7(Config 可配)；MOMENTUM TRENDING→1.5/RANGING→0.5；
  LEAD_LAG 跨期→1.5/单合约→0.3；
- **Layer3** 在线 IC/IR：RollingIC(window=2000, horizon=5) recordSignal/recordReturn 配对，
  update 全窗口 Pearson 相关 + 切 10 段算 IR=mean_ic/std_ic；ic_factor=clamp(1+tanh(IR×2), 0.3, 2.0)；
- **computeWeights**：一致性 consistency=|Σsign(si)·wi|/Σwi（|si|>0.01 参与无则 0.5）→
  w_raw = base × regime × ic × consistency_boost(同向多数派 0.8+0.4c∈[0.8,1.2]) → floor(0.05) →
  归一化(enabled 掩码排除未启用项) → cap(0.50) 归一后施加；
- **processTick 统一入口**（V8-S1/S2 从 Aggregator 迁入）：IC 簿记+未来收益配对(horizon=5)+
  每 50 tick updateIC + MA 增量维护+detect+computeWeights；resetTickState 复位。

#### 4.13.4 五路信号源算法

| 信号源 | 算法要点 | 配置 |
|---|---|---|
| **OFISignalSource** | 标准 OFI 逐档比较（价升全量/价减负量/同价增量；ask 符号取反），RingBuffer 窗口衰减累积；自适应归一化 cum_scale=avg_abs×n×0.5 clamp[5,500] → tanh 映射线性区；bid/ask_pressure=0.5×(1±ofi) 线性互补(S5 修复阶跃退化) | window=50 |
| **TradeFlowSignalSource** | 双路径：内部 onTrade 记账（当前无调用者属登记在案死扩展点）/回退 book.getTradeFlowAnalysis()（生产实际路径）——统计显著性归一 net_flow/(avg_size×√n) → tanh(sig/3)，p95≈±0.7 不饱和；大单判定 qty≥threshold | window=100, largeTradeThreshold=50 |
| **BookImbalanceSignalSource** | 纯转发型：simple_imbalance 一档失衡、depth_imbalance 距离加权 Σ(vol/distance_ticks)；dominant_threshold 判定 bid/ask_dominant；距离权重的 tick_size 由装配期 setContract 注入(P0-4 修复恒 0.2 致 EC 偏差 2.5 倍) | threshold=0.2 |
| **MomentumSignalSource** | 增量对数收益滚动和 O(1)，最近 min(window,128) 收益均值 ×1000 → tanh；EMA 平滑(α=0.1)；样本<9 invalid | window=50(S6 接线生效), emaAlpha=0.1 |
| **LeadLagSignalSource** | anchor 合约 mid_change 推送(updateLeadContract)：每 lead RingBuffer<64> 线性新近加权平滑 → Σ(mid_change×|corr|)/Σ|corr| × scale_factor(3000 bps 口径) → tanh；result 在非 anchor tick 持久化上次值；ctor 显式初始化修 UB(S2) | window=50 |
| **VolatilitySignalSource**(辅助) | realized_vol：价格收益 RingBuffer(窗口 min(window,256)) running_sum/sum_sq Bessel 校正；分档直接阈值比较 ≥extreme→EXTREME / ≥elevated→ELEVATED；vol_percentile 线性映射 [0,extreme]→[0,85] 封顶 100（供 GLFT sigma_sq 与 regime 用）；statsLogInterval>0 时分布埋点（S10 阈值标定工具，默认关） | window=100, elevated=0.0005, extreme=0.0017(实测 p95/p99.5 标定固化) |

#### 4.13.5 MarketDataContext.h/.cpp —— 行情上下文 Facade

- `OrderBookStateTracker`：setContract(code,tickSize,depthLevels=5) 装配；onTick 抽取最多 5 档
  price>0&&vol>0 → updateDerivedMetrics：**单边盘口 mid/spread 清零**(S3 锁板对齐防半价污染)、
  全档深度求和、imbalance=(B−A)/(B+A)、depth_imbalance 距离加权对称归一、estimateLiquidity=
  min_depth/(spread_ticks×10) 封顶 1；
- `TradeFlowTracker`：滑窗双条件衰减（超 5s 或超 100 条从队首对称扣减——修复无衰减致 session 内
  单调累积 TradeFlow IC=-0.83 反向 bug）；onTickInference 每 tick 走 TickTransactionInferer 推断，
  confidence≥0.3 && volume>0 才记账；onTransaction(L2 有则走此通道方向直取 side)；getAnalysis
  输出 buy/sell_pressure、large_trade_ratio、avg_trade_size 与显著性归一 net_flow_normalized；
- Facade：onTick 首帧校验 getCode().empty()（工厂漏调 setContract）一次性 warn；十余个零开销
  inline 转发访问器；onOrderQueue/onOrderDetail L2 入口 TODO 占位（S9 双通道计重前置）。

**TickTransactionInferer.h/.cpp**（内嵌于 TradeFlowTracker）：无 L2 逐笔时从 tick 快照推断成交
方向与量。detectMethod 优先级：Lee-Ready 报价法(last≥ask1−0.1tick→买发起) → 相邻 tick 价差穿越 →
Tick Rule(|Δ|>0.5tick) → 盘口消耗(bid 下移=卖方消耗等) → UNKNOWN。confidence 分方法计算
(SPREAD_CROSS 量加权 clamp(0.2+max_depletion/30, 0.2, 0.9)——旧固定 0.9 是 IC 反向事故根因；
DEPLETION 与价格变化同向 +0.2 boost)。volume=max(bid_depletion, ask_depletion) 双侧取和。
InferenceRecord{signed_volume(×confidence), volume(原始量 R3 泄漏修复), confidence, is_large,
timestamp}。大单桶按原始 volume 对称增减（修复 add 全量/prune 加权导致残留单调膨胀稀释
large_trade_ratio）。

**ISignalCombiner.h**：typeName 虚接口 + LinearCombiner("linear") + Registry 单例；当前仅用于
model.type 校验，linear 组合仍内联于 computeAlpha（非线性融合预留扩展点）。

### 4.14 毒性检测 ToxicFlowDetector 族

**ToxicFlowDetector.h/.cpp —— 门面(Facade)**

统一 Predictive(事前每 tick) 与 Realized(事后成交事件) 两通道，产出 ToxicityMetrics 做触发裁决。

- `ToxicityParams`（ctor 默认 vs yaml 生效值）：adverse_threshold 0.10→**0.75**(R2 标定固化)、
  vpin_threshold 0.10→**0.60**、vpin_window 50→20、vpin_bucket_size 1000→50、
  vpin_min_warmup_buckets 5、alpha_weight 0.3→0.5、book_weight 0.3、self_trade_weight 0.4、
  extreme_signal_weight 0.8、extreme_signal_threshold→**0.9**(可配置，旧硬编码 0.6 低于 OFI 归一器
  常态区会恒触发)、vpin_weight 0.5(T6 新增)。fromVariant 含加载期边界校验越界回落默认；
- `setParams` 权重通道内归一（T6）：ofi_weight=alpha/(alpha+book)、trade_weight=book/(alpha+book)
  （原样透传和=0.8 使尺度压缩漂移）；
- `updateCache()` 合成流程：pred.analyze() → toxic_score=combined_score；realized total_fills≥3 时
  按 self_trade_weight 加权混入(decayed_score)；**extreme 硬保护层在 realized 加权之后**
  max(toxic_score, extreme×0.8)（顺序修复：旧版先混入被稀释）；is_toxic OR 条件(T4)：
  score>adverse || (vpin_ready && vpin>vpin_threshold)；is_toxic 时 toxic_side=pred.toxic_side
  （R2 方向归属与触发分支解耦）；
- 数据入口：updateMarketAlpha(Md tick)/onSelfTradeCalibration(Td 成交)/onTrade/onTickVolume(VPIN)。
  R3 已删 SyntheticSignalFusion 整链（710 行死代码）。

**PredictiveToxicity.h/.cpp —— VPIN + Alpha 事前毒性**

- VPIN 桶归一算法（T3 经典口径）：按方向累入 current bucket 至 bucket_size 关桶，关桶 push
  |buy−sell|/total ∈[0,1] 进窗口(vpin_window 个桶)，vpin=均值严格有界（旧口径单边流可达 1.2 无界
  高估）；余量滚入下桶不丢弃；onTickVolume 无 L2 场景方向推断（price≥ask1→buy 等，无法判定拆半）；
- analyze()：vpin_ready=buckets≥min_warmup（预热门：未达时 vpin 报 0 但 alpha 通道照常）；
  ofi_toxicity=|ofi_component|；trade_toxicity=|imbalance_ratio|×(0.5+0.5×large_trade_ratio)(T7
  满幅)；alpha_toxicity=归一加权和；combined_score=vpin_weight×vpin+(1−w)×alpha_toxicity；
  toxic_side=ofi 与 imbalance 同号判定(+1/-1/0)；extreme_signal=max(ofi,trade)>extreme_threshold。

**RealizedToxicity.h/.cpp —— 已实现毒性（事后）**

数据唯一来源 SelfTradeCalibrator 的 CalibrationResult。analyze()：adverse_ratio=toxicity_level、
decayed_score=adverse_ratio×confidence（T2：删除内部 ×weight——原内外双重乘有效权重 0.16 稀释
2.5 倍）；avg_adverse_move 启发式。R3 清理 decay_factor/book 死链后仅剩 weight/min_samples(3)。

### 4.15 套利子系统 arb/

22 个文件。定位：跨期/跨品种价差套利，与做市共享账户仓位。核心哲学（Scheme B-3）：
**FutuPortfolio 是仓位唯一事实源**（MM+ARB 合并记账），套利"意图"来自 z-score 纯函数，"实际
仓位"每 tick 从 Portfolio 回填推导；CLOSE 类信号默认永久抑制（让做市 maker 单自然消耗价差仓位，
赚 spread 免手续费），仅保留止损/超时的分级主动平仓灰度通道（C1/C2，出厂关闭）。

**端到端数据流**：

```
MdSpi: arb_bridge.onTick
  ├ closeout 活跃平仓期早退(A12) → async_arb->pushTick ──SPSC(1024)──┐
  ├ refreshPositionsFromPortfolio(Portfolio SSOT→腿持仓回填)
  ├ MM 快照 generation 增量同步(预计算 max_buy/min_sell 标量)
  ├ processPendingOrders 回调: 同价去重 → B3 第二层精判(方向校验/过冲 clamp/
  │   FAK 对手价替换) → OrderRouter.submit(Source::ARBITRAGE) → tagOrderPair+
  │   registerPairOrder+trackArbOrder; 失败兜底 cancelByPair+markLegRejected
  ├ processOrphanLegs 回调: leg2 盘口定价 → Source::HEDGING → 受理状态返回触发重试
  └ popTimedOutPairs/popOvershootPairs → cancelByPair
Arb线程: popAll ticks → Manager.onTick(Calculator fresh-pairing→EWMA z-score→策略 update)
  → 每 5 tick 或 5ms generateSignals: PnL 快照→drawdown→冷却检查→策略插件 generateSignal
    → leg 元数据补全 → confidence 门(0.5) → applyB3Gate → executeSignal:
      STP 调价 → 利润门槛(minProfitThresholdTicks) → 双槽位原子提交 SPSC(256)
      (leg2 失败 → enqueueOrphanLeg)
TdSpi: on_trade → consumePairTag → onArbOrderFilled(in_flight 扣减) + 残腿对冲(HEDGING FAK)
       on_order(撤终态) → onLegCancelled(只清实际在途通道)
```

#### 4.15.1 类型与接口

- **SpreadArbitrageTypes.h**：SpreadType(SIMPLE_DIFF/RATIO/LOG_DIFF/WEIGHTED/BASIS)、
  ArbitrageStrategy(6 种)、SpreadSignalType(10 种含 OPEN/CLOSE_LONG_SHORT/STOP_LOSS/TIMEOUT_EXIT
  等)、ArbIntent(NONE/WANT_LONG/WANT_SHORT/WANT_FLAT)；is_close_signal(t) 把 CLOSE×2/STOP_LOSS/
  TIMEOUT_EXIT/REBALANCE 归为平仓类（in_flight 双通道分流依据）；
- `SpreadPairConfig`：pair_id/leg1_code(近月)/leg2_code(远月)/spread_type/**leg1·leg2_multiplier
  (默认 1.0，跨品种必须显式配)**/leg1·leg2_ratio/entry_z(2.0)/exit_z(0.5)/stop_loss_z(4.0)/
  trend 参数(stop_loss_pct 0.02/max_trend_bars 50)/add_safety_ratio(0.75)/max_spread_position(20)/
  max_single_leg(30)/lookback_window(200)/primary_strategy(mean_reversion)/enhance_quoting；
- `PairArbState`(B-3 核心)：intent/in_flight_qty(open 通道两腿未成交总量)+direction+set_time/
  close_in_flight_qty(close 专用通道)+set_time；头部注释完整列出武装点/扣减点状态表(S-5)；
- **ISpreadStrategy.h**：generateSignal/update/configure/reset/typeName 纯虚接口 +
  SpreadStrategyRegistry 工厂注册表；线程契约：由套利执行线程调用实现类无需加锁，Manager 负责
  外部串行化；四个内置策略经静态注册器注册。

#### 4.15.2 SpreadCalculator.h/.cpp —— 价差计算器

每 pair 一个实例。核心机制：

- **fresh-pairing 同步配对**：leg1 tick 到达仅当 leg2 也 fresh 才推进样本（消费对方标记）——修
  旧"任一腿 tick 都 push 另一腿用旧价"log_return=0 稀释 beta 问题；RingBuffer 256；
- **统计引擎**：EWMA z-score（α=0.005 半衰期约 138 tick，std≤1e-10 保护）；OLS beta+相关性
  每 10 tick 单次扫描同算（消除重复 std::log），smoothed_beta EMA 平滑截断 [beta_min,beta_max]
  =[0.7,1.5]；OU 半衰期每 50 tick（θ≥0 非均值回归返 0；A11 零方差守卫 denom<1e-12 返 0）；
- calculateSpread 五式：WEIGHTED=r1·P1·m1 − r2·P2·m2（乘数经 setLegMultipliers 装配，V8-A1
  死亡链修复）；getState 写 **current_price=current_spread**（A2：此前恒 0 使 TrendFollowing
  价格止损是死分支）。

#### 4.15.3 四个策略插件（ISpreadStrategy 实现）

| 策略 | 入场 | 出场（优先级序） | 特点 |
|---|---|---|---|
| **MeanReversionStrategy**(默认主策略) | z>+entry_z(2.0)→开空价差；z<−entry→开多；confidence=min(0.5+(|z|/entry−1)×0.5,1) | ①\|z\|>stop_loss_z(4.0) STOP_LOSS(conf 1.0)；②持仓超 convergence_timeout(3600s) TIMEOUT_EXIT(0.8)；③回归退出 z 过 ±exit_z×0.3(比 exit 更靠近 0 防过早平) CLOSE(0.9) | 加仓：z∈(−SL×ratio, −entry×0.5) 区间且远离止损区（addSafetyRatio=0.75 语义）；半衰期过滤 max_half_life 500；update 空操作无内部状态 |
| **PairsTradingStrategy** | residual z-score 同构阈值 | 同构 | 私有价格历史独立 OLS **价格水平回归** P1=α+β·P2（区别 calculator 的 log-return 回归）每 100 tick 重估 β EMA 平滑；配对有效性检查(\|corr\|≥0.7)；**A4 修复：update 按 state.last_update(fresh-pairing 时间戳)去重**防陈旧价污染 β；min_samples=100 |
| **TrendFollowingStrategy** | 快慢 MA(20/60) 交叉+强度(近 5 点回归斜率/slow_ma≥0.001)+confirmation_bars(3) | ①**百分比止损 pnl_pct=(current_price−entry_price)/entry_price 超 ±stop_loss_pct(0.02) STOP_LOSS return 早退(A2 打通)**；②趋势反转退出(0.9)；③耗竭退出 bars>max_trend_bars(50) 且自身衰减(0.7) | 私有 spread 历史 RingBuffer 128 |
| **StatisticalArbStrategy** | composite=Σ wi·fi 四因子(zscore/zscore_momentum/volatility_ratio/correlation_trend 各归一 [-1,1]，权重 0.30/0.20/0.15/0.20 先归一) | \|composite\|>stop_loss_threshold(1.5)止损 / >exit(0.3)出场 / timeout 5400s | entry 0.7；signal_confidence=\|composite\|×feature_stability(1−0.5×|momentum|)；R3/A3 后 6 因子缩至 4（死因子及自适应链已删）；volatility_ratio NaN 守卫 A11 |

#### 4.15.4 SpreadArbitrageManager.h/.cpp (+Init.cpp) —— 总编排器

持有 calculator_manager/risk_manager/mm_enhancer/strategies(pair_id→插件列表)/pair_configs/
pair_states/pair_arb_states；外部注入 Portfolio SSOT 只读指针。三把自旋锁分工（alignas64）：
`_pair_states_spin`(状态+last_signals+risk 状态)、`_pair_arb_spin`(arb_states+timed_out)、
`_intent_spin`(close_intents+overshoot)。Init TU=纯装配(loadConfig/initializeStrategy/addSpreadPair
/removeSpreadPair ~270 行)，主 TU=运行时（文件级二分 S-4）。

**generateSignals（arb 线程）**：PnL 快照读 Portfolio atomic → updatePortfolioPnL(drawdown) →
逐 pair 冷却检查(signal_cooldown_ms=1000，µs→ms 比较，未过冷却锁内早退不做拷贝) → state 拷贝 →
canOpenPosition(风控闸) → 插件循环(hybrid 取置信度最高) → **leg 元数据补全**（空 code 会致
OrderRouter segfault）→ confidence 门(0.5 单键 R3 统一) → applyB3Gate。

**applyB3Gate（子系统心脏，入口 current_time_us/1000 转 ms 域）**：

- *平仓类通道*：C0 门控(is_allowed(type)，默认全抑制 NONE) → B5 冷却检查 → B4 平仓超时清理
  (>5000ms 强清+推 timed_out) → B4 平仓防双发(close_in_flight>0.5→NONE) → 零持仓降级
  (derived<0.5 已被 MM 消耗完) → 量裁剪 close_qty=min(size, derived×max_close_size_pct(0.5)) →
  武装 close in_flight(qty×(r1+r2)) → B1 广播 setActiveCloseIntent；
- *开仓类通道*：信号类型→意图映射 → intent 更新+computeDerivedSpread(matched=min(|leg1|/r1,
  |leg2|/r2)) → open in_flight 超时清理(60s) → 防双发(in_flight>0.5→NONE) → gap=target−derived
  (<1 手 NONE 让 MM 自然消耗) → ramp 保护(order_qty≤max_pos×25%) → 投影上限(derived+qty≤
  max_pos×1.05 余量收缩) → 武装 in_flight。

**in_flight 两族三个回调**：onArbSignalDropped(pair,is_close)（V8-A3 按通道精确释放，close 时锁外
clearActiveCloseIntent）；onArbLegCancelled（撤单事件不带类型——只清实际在途>0.5 的通道）； 
onArbOrderFilled（**close 族优先扣减**，余额扣 open 族；close_done 锁外清 intent）。

**B1/B5/B6 协同**：

- B1 意图实时通道：CloseIntent{pair,direction,qty} 存 _active_close_intents；getArbCloseDirection
  (leg_code) 做 **1:N 全量遍历**（A13：旧 any-match 无序首项丢信息），一致返回该方向冲突返回
  kArbCloseConflict=2 → QuotePolicyChain ArbCloseSyncPolicy 双侧抑制；
- B5 过冲保险丝：FutuPortfolio sign-flip 检测(+N→−M 且该 leg 有活跃 close intent) →
  onOvershootDetected → 连坐过滤（只冷却有 intent 的 pair）→ 写 overshoot_cooldowns(now+3600000ms
  =1h)+推待撤队列+CRITICAL 告警；bridge 每 tick popOvershootPairs→cancelByPair；
- B6 聚合查询：lock-free z-score 缓存(vector<unique_ptr<atomic<double>>>)；getAggregateZscore 取
  |z| 最大者；getQuotingAdjustmentForLeg 完整调整建议（当前仅观测模式消费）。

**C0/C1/C2 分级平仓体系**：CLOSE_LONG/SHORT **永不解禁**（allow 默认 false）；STOP_LOSS 最高优先
解禁 C1（FAK order_flag=1+对手价+1000ms 超时）；TIMEOUT_EXIT 次优先 C2（GFD 挂 mid+30000ms 升级）。
出厂 enabled=false=纯 B-3（故 C1/C2 路径无回测覆盖，靠单测+实盘灰度）。

#### 4.15.5 AsyncArbitrageExecutor.h/.cpp —— 异步执行器

- 三条 SPSC 管道（载荷 trivially copyable static_assert + FixedString24 定长串）：_tick_queue(1024,
  Md→arb)/_order_queue(256, arb→Md)/_orphan_legs_from_arb(64)；
- 线程模型：use_async_arb_thread=false（回测）pushTick 直接同步 processTick+processSignals 不开
  线程避免 data race；实盘 arb 线程自适应自旋（pause×1000→yield→sleep 50µs 三级退避）；
  **异常兜底**：处理异常不让线程退出（连续 ≤3 重试否则 disable 套利保线程存活）；
- setReplayNowUs（A5）：策略每 tick 注入 replay µs，替换 5 处墙钟——回测 TIMEOUT_EXIT/
  maxDivergenceTime/orphan 超时可复现触发；
- executeSignal：平仓方向**按 spread_position 符号**（修旧硬编码平多致空头止损=加仓 bug）→ STP
  调价（O(1) 读预计算极值标量：arb 买价≥min_sell→调至 min_sell−tick）→ 利润门槛（spread_impact
  代数重算除以平均 tick size，短价差符号已修 A1，超 threshold(1.0 tick) REJECT+释放 in_flight）→
  原子双腿提交（request_id fetch_add(2) 相邻编号；预检双槽位；leg2 push 失败 leg1 无法撤回→登记
  孤儿腿 OrphanLeg{hedge_qty 含 ratio/retry_count/last_attempt}）；
- 孤儿腿对冲（processOrphanLegs，主线程）：dual-queue 设计（先 pop 光 arb 队合入 main-thread-only
  deferred）；delta_ratio 动态超时（ratio≥1 立即强制；(0,1) 区间线性缩短最低保 1000ms）；三级处置
  （age≥force_ms urgent 市价式 / ≥timeout 对手价积极 / 否则等待自然成交）；**有限重试**（500ms
  节流、回调返回 false retry_count++、超 3 次 error GIVEUP 防死循环 V8-A2）；定价权全在 bridge
  （leg2 盘口对手价，urgent 再让 1 tick）；
- `_oid_to_pair` 配对表：tagOrderPair 写于 MdSpi/consumePairTag·onOrderFinalized 读删于 TdSpi——
  大锁时代被掩盖的竞态，收官审计后加 `_oid_pair_lock`（RecursiveSpinLock）。

#### 4.15.6 ArbExecutionBridge.h/.cpp —— 主线程执行桥

Deps 注入十项。`RecursiveSpinLock _lock`（Md onTick vs Td onTradeFill/onLegCancelled）。关键逻辑：

- **closeout 冻结桥**（A12 修订）：仅活跃平仓期(DRAINING/ASSESSING/EXECUTING)暂停一切——新开仓
  会让 drain 永不完成且被强平单反向成交；COMPLETED/FAILED 解冻恢复对冲保护；
- processPendingOrders 下单回调（核心风控层）：同价去重（undone>0 且价完全一致→丢弃，防底层自动
  撤挂相同单毁掉交易所排队优先级；drop 同步释放 in_flight B13 fix）→ **B3 第二层精判**（读 Portfolio
  实时仓位：C3 方向校验 live_pos×signed_qty≥0 说明"平仓"实为开仓→skip+释放 in_flight(A10 修复
  止损重试被 B4 抑制窗口)；过冲预估 predicted 翻转→clamp 到恰好平仓(B5 事前防线)；FAK 对手价替换）
  → Router 提交 → M3 腿失败兜底(cancelByPair+markLegRejected) → 成功逐 id tag+register+trackArbOrder；
- onTradeFill：consumePairTag→onArbOrderFilled；残腿对冲 remaining=original−hedged 上限未知全额
  hedge_qty=min(vol,remaining)；stale 条目清理（original>0 者 30s 过期、==0 者 2s 过期）；
- markLegRejected 多次拒单取最大预期上限；onLegCancelled 覆盖"leg1 成交+leg2 被撤"场景；
  resetSession 只清 hedge_on_fill。

#### 4.15.7 SpreadRiskManager.h/.cpp —— 套利风险管理

- 配置 SpreadRiskConfig（yaml risk_limits 段，H4 修复此前未加载）：max_total_position(50)/
  max_single_pair(20)/max_leg_exposure(30)/max_correlation_break(0.3)/max_divergence_zscore(5.0)/
  max_divergence_time(7200s)/**portfolio_stop_loss(50000 元)**/pair_stop_loss(10000)；
- drawdown：peak 跟踪 total=unrealized+realized，drawdown=max(0,peak−total)；数据来自 Portfolio
  PnL 快照（F4 接线，每 arb 周期 ~5ms 更新）；
- 四级告警矩阵：drawdown>0.8×stop_loss→CRITICAL；drawdown>stop_loss→**EMERGENCY**；
  correlation<0.3→WARNING；|z|>4.0→WARNING；|pos|>0.9×single_pair→WARNING；
- **EMERGENCY 消费链（A8 修复假保险丝）**：checkRiskAlerts(arb 线程)→alert_callback→
  UftFutuMmStrategy::handleRiskAlert → haltTrading(**IRREVERSIBLE**, −drawdown) +
  spread_arb_manager->disable() + EventNotifier topic="ARB_RISK"。该回调运行于 arb 线程，
  是 halt 域唯一绕大锁写者（WS-A 已收编）；
- canOpenPosition/checkPositionLimits：单 pair 新仓与组合 Σ 限制（Manager B-3 门前调用）；
  calculatePairRisk 净敞口公式 leg1_exp+beta×leg2_exp（修旧相减虚高 2 倍）；var_99=2.33×|pos|×std。

### 4.16 观测与统计层

**BilateralQuoteStats.h —— 做市义务双边统计（per-quoter 值成员）**

记录"双边有效报价时间占比"等交易所做市义务考核指标。时间体系内部单位=minute×60+sec，
经 WTSSessionInfo::timeToMinutes 映射（非交易时段丢弃）；setSessionInfo 预计算全天总秒数。

- 有效判定 checkBilateral：双边有效 + spread∈(0, obligation_max_spread_ticks]（**挂在哪=统计到哪**
  口径统一）+ 累计深度加权价达到 min_valid_qty(10 手)；
- update 边沿驱动累计：进双边记起始并 switch_count++，出双边累加时长；仅双边时记 spread 样本；
- **5 个诊断计数器**（both_empty/no_bid/no_ask/crossed/spread_wide）定位覆盖率失真根因；
- formatString(日终)/formatLiveString(实时)/flushSection(段终点输出自包含行并清零段计数器)/
  serializeSnapshot/seedFrom（重启续算，ses>0 才覆盖分母）；
- 调用链路：quoter.onEntrustAck/onOrder/onTrade → coordinator logBilateralStatsPeriodic(300s 周期
  live 行→内存积压)+flushBilateralStats(section/closeout 定点) → RuntimeOps onSessionBegin(seed)/
  onSessionEnd(formatString)。

**PerformanceMonitor.h/.cpp —— 延迟监控（系统健康）**

- 5 个延迟通道：TICK_TO_QUOTE / ORDER_TO_ACK / QUOTE_TO_FILL / CANCEL_TO_ACK /
  SIGNAL_TO_ORDER（实为 on_tick 入口→主流水线结束的全链路通道，rdtsc 计时）。每通道
  RingBuffer<uint64_t,16384> + atomic 当前值；
- ThroughputStats **17 个全 atomic 字段**（累计×5/每秒差分×5/基线×5/时间戳×2，V8-R6 收官原子化）；
- LatencyStats：min/max/mean/std/p50/p90/p95/p99_ns（P999 错误别名已删）；
- checkThresholds（1s 节拍由 coordinator 调用）：样本<100 不告警；p99>critical(50000ns)→error、
  >warn(10000ns)→warn；latency_threshold_ns=100000 即时 warn；
- TscClock 校准在装配期完成；默认 enabled=false（coordinator.yaml 开关）。

**PerformanceAnalyzer.h/.cpp —— 绩效分析器（策略质量，事件驱动记账）**

- TradeRecord{immediatePnL()=买:(mid−price)×qty 卖对称; spreadCaptured() 价差归一捕获}；
- PendingAdverse 真实逆向选择追踪：成交后等 10 个同合约 tick 用当前 mid 计算逆向移动，
  30s 墙钟兜底过期（时间域统一 replay 时钟 R3-残余修复）；
- 输出指标：total_pnl/volume/trades、avg_spread_captured、fill_rate、win_rate、max_drawdown、
  sharpe_ratio（年化因子 sqrt(250×trades_per_day) HFT 修正）、adverse_ratio 与
  real_adverse_per_vol（独立不被 PnL 放大）、alpha_accuracy(|alpha|≥0.3 时方向命中)、
  trading_time_sec/trading_days；PnLAttribution 归因（spread/inventory/adverse/alpha/timing 残差
  自闭合）；generateSummaryReport 会话报告。determineMarketCondition 显式占位 NORMAL（待
  RegimeTracker）。默认 enabled=false。

**MonitorBridge.h/.cpp —— GUI 监控桥**

文件契约对接 wtpy WtMonSvr（非 MQ 直连）：定期把资金/持仓快照写 JSON 到
`generated/stradata/{straId}.json`（DataMgr 60s 轮询），契约与 CtaStraBaseCtx::save_data 完全对齐；
onSessionEnd 追加 outputs/{id}/funds.csv（同日去重）。热路径零开销（maybeFlush 时间戳比较 ~ns，
到点才锁内序列化），write_file_atomic=.tmp+rename 防半文件。配套框架补丁：WtUftRunner 补调
initEvtNotifier 使 notifier 段生效（MQ 实时订单/成交/日志通道）。config.yaml monitor 节控制
（enabled=true/flushIntervalMs=1000）。

**MarketMakingEnhancer.h/.cpp —— 套利信号增强做市（观测模式半接线）**

实例由 SpreadArbitrageManager 持有；calculateAdjustment 基于 z-score：skew 线性缩放
|z|∈[1.0,3.0] 区间超额×max_skew×confidence（z>0 价差偏高倾向卖→负 skew ask 更激进）；
spread_multiplier 在 [1.5,3.0] 区间放宽；suppress 双向 z>±3.0。现状：QuotePolicyChain B6 观测模式
计算但不注入（static 开关默认 false）；yaml enhanceMarketMaking 键保留兼容属待清理空开关。

### 4.17 配置与装配层

**配置四源分工（单一权威位置原则）**：

| 文件 | 职责 |
|---|---|
| config.yaml | Runner 主配置（basefiles/parsers/traders/env/strategies）：策略身份+合约+业务参数。**不承载任何模块开关** |
| coordinator.yaml | **模块开关唯一权威**（7 个根级 use* 开关）+ taker 减仓/requote/section-break/双边统计/pipeline 参数 + modules.*（signalAggregator/toxicityDetector/spreadOptimizer/selfTradeCalibrator/selfTradePrevention/autoCancel/correlationManager） |
| spread_arbitrage.yaml | 套利子系统全局/pairs/risk_limits/statistical |
| hotparams.yaml | 27 个热参数运行时文件 |

**FutuConfigLoader.h/.cpp**：解析 config.yaml params 子树进 FutuMmConfig+ContractInfo 列表。
fail-fast 校验（V8-R6）：contracts 缺失/非数组/为空→error+false（拒绝零合约空跑）；anchorCode 空
或不在 contracts 列表→error+false。参数边界 error 级（拒绝启动）：maxDelta∈(0,1e8]/numLevels∈
[1,10]/levelStep∈(0,100]/**useBilateralQuote=true 与 obligationLevel≠0 不兼容**(路径A硬编码 L0)/
baseSpread≤20/baseQty≤100/hedgeRatio∈[0,1]/maxOrdersPerSec∈[1,500] 等；warn 级含 full_side_depth
预估 <obligationMinQty 提示可能不满足交易所总深度义务。

**FutuConfig.h/.cpp**：WTSVariant 读取封装。isInvalidScalar 区分"键缺失/Null/容器/空串"→回落
默认值（修旧 asDouble 空串→0.0 致 protectTicks 空值静默关价格保护）；readBool 支持数值型
(YAML `1`→true)；readString 保留合法空串语义。

**FutuConfigValidator.h**：header-only 校验工具集——validateSignalWeights（五权重和偏离 1.0 超
±0.1 warning）/validateStopLoss/validateExitThreshold/validateAddSafetyRatio/validateQueueSize
(2 的幂)/checkRange/checkPositive。on_init 内做交叉一致性校验。

**FutuModuleAssembler.h/.cpp —— 装配器（friend+引用别名直调策略私有成员）**

assemble 第 0~16 步顺序：

```
 0 StrategyCoordinator(loadConfig(coordinator.yaml)→setConfig 策略覆盖→7 开关解析)
 1 FutuPortfolio(addContract 逐合约)
 2 CorrelationManager(两两 addRelation)
2b FutuQuoter ×N(quoting 全量透传; min_valid_qty=obligation_min_qty)
 3 SpreadOptimizer ×N(GLFTParams)
 4 UnifiedOrderTracker(arb 启用时 STP 强制开启) + per-quoter initBilateralStats
5.6 OrderRouter(tracker 注入; arb/hedge 30/s, closeout 不限)
5.7 CloseoutExecutor → session 注入 → coordinator.initialize()
 6 FutuRiskMonitor(RateLimits 整体拷贝单一来源/RecoveryConfig/CloseoutConfig)
   → CloseoutOrchestrator.setDeps 十项
 7 MarketDataContext ×N(setContract+largeTradeThreshold 单一口径 P0-4)
7.1 SignalAggregator ×N(model.type 校验失败直接终止 assemble; 非 anchor 加 lead)
 9 ToxicFlowDetector → 9.5 SelfTradeCalibrator(anchor tick_size; 注入毒性检测器)
11 PerformanceAnalyzer(可选) → 12 PerformanceMonitor(可选; TscClock::calibrate)
14 SpreadArbitrageManager(独立 yaml; Portfolio SSOT 注入)
15 SelfTradePrevention → 16 AsyncArbitrageExecutor(minProfitThreshold 接线;
   B5 过冲保险丝 setArbManager; ArbExecutionBridge.setDeps 十项)
收尾: deps.trading_state(if/else 外!) → wireDeps(内嵌 Trigger/RiskCoord setDeps +
   initLastMid) → validateDeps fail-fast → EventNotifier 补注 → 依赖完备性二次校验
```

loadContractInfos（assemble 之前）：fullCodeToStdCode 取 comminfo/sessionInfo 缓存、乘数/tick 回填、
从 trading sections 推导白盘/夜盘收盘时间（夜盘跨日品种 endTime≤600 保持 HHMM）、anchor 收盘时间
回写 _config.closeout（平仓触发时间基准）。

**FutuComponentFactory.h/.cpp**：8 个 create 方法集中实例化+依赖注入，签名统一接受
CoordinatorConfig（经 `_raw_variant` 直读原始 YAML 节点）。createSpreadOptimizer(baseSpread 从
config.yaml 经参数传入权威单源)、createMarketDataContext(largeTradeThreshold 单一口径)、
createToxicFlowDetector/SelfTradeCalibrator/SelfTradePrevention/PerformanceMonitor/Analyzer/
AsyncArbitrageExecutor。

**FutuHotParamManager.h/.cpp + FutuHotParamWatcher.h/.cpp —— 热参数机制**

27 参数索引表（HP_BASE_SPREAD…HP_MAX_PRICE_DEVIATION、HP_CONTRACT_MAX_DELTA），registerParams 以各模块当前值为默认经
`ctx->sync_param(name,default)` 注册进引擎共享内存。链路：

```
hotparams.yaml (snake_case, watcher 1Hz 轮询)
  → parseHotParamFile(纯函数): strtod 全串校验拒收 "abc"/布尔/空串 + 27 参数边界表
    (越界/NaN/inf warn+跳过保留旧值; 未知键忽略)
  → syncFromFile: 逐项与共享内存现值比对去重, 变更才写+审计日志 old->new,
    变更数>0 置 _pending_apply(release)      [watcher 线程只碰共享内存]
  → UftFutuMmStrategy::on_tick (MdSpi, 大锁内):
    consumePendingApply()(atomic exchange ~1ns) 为真 → on_params_updated()
    → applyAll 分发: GLFT 11 参(seqlock 安全)/quoting 4 参(updateQuotingParams 刷数量表)/
      信号权重+strongThreshold(穿透 updateBaseWeights 否则热更无效)/alpha_sensitivity/
      max_delta(portfolio+coordinator)/sticky/improve_retreat/protect_ticks/max_price_deviation
```

27 参数：base_spread/base_qty/level_qty_multiplier/level_step/max_delta/alpha_sensitivity/
ofi_weight/trade_weight/book_imbalance_weight/momentum_weight/lead_lag_weight/strong_threshold/
confidence_weight_min/confidence_weight_max/phi/delta_skew_threshold/delta_skew_factor/
max_spread_mult/min_spread_mult/depth_sensitivity/toxicity_spread_factor/
low_confidence_spread_factor/sticky_threshold/improve_retreat_ratio/protect_ticks/max_price_deviation/
contract_max_delta(B2: 单合约 delta 软限, 应用于全部合约; 差异化需重启)。
watcher start 的首轮成功判定="解析失败才算失败"（空 hotparams.yaml 合法，修复旧逻辑致 watcher
不启动的潜伏 bug）。回测侧引擎无 start_watching 故策略自带轮询是两模式都可靠的通道。

**热参加固三道防线（2026-08-24）**——27 个热参数稳态权威是 hotparams.yaml（实盘 ~1s 覆盖
config/coordinator 同名键；重启时共享内存旧值经 initial applyAll 立即生效），回测不加载热参：

1. **边界表对齐**：拒收域收紧至 FutuConfigLoader error 级 / validator 口径——base_spread [0.5,20]、
   base_qty/level_step (0,100]、max_delta (0,1e8]（0=静默关组合 skew+WIDEN 分母）、phi [0.01,2]、
   delta_skew_threshold [0,0.9]、sticky_threshold [0.01,10]（0=churn 风暴）、
   protect_ticks ≥0.5（0=静默关价格软保护；关闭应走 price_protection 开关）；warn 级口径保持宽松；
2. **applyAll 交叉复查**（`crossCheckIssues`，warn "[HOTPARAM-CHECK]" 不阻断）：五路权重和偏离
   1.0±0.1 / full_side_depth<obligationMinQty（loader 同公式）/ portfolio_max_delta>任一合约
   maxPosition（delta-position 语义边界）/ min>max_spread_mult / conf_min>conf_max；
3. **启动漂移摘要**（`logDriftSummary`，回测也打印）：hotparams.yaml 与 config/coordinator 同名键
   逐条 warn "[HOTPARAM-DRIFT]" 并汇总——差异键即回测/实盘行为分叉点（该机制上线即发现 dist
   回测副本 base_spread/base_qty/max_delta 三键真实漂移）。

### 4.18 低延迟基础设施

**SpinLockGuard.h —— 自旋锁族**

- `SpinLockGuard`：RAII 包装 atomic_flag（test_and_set(acquire) 循环+_mm_pause；析构 clear(release)）；
- **RecursiveSpinLock**：atomic_flag + atomic<thread::id> owner + 非原子 uint32_t count——同线程
  重入仅 ++count 不抢 flag（无竞争路径 ~2 次原子操作）；count 非原子安全因为仅 owner 可读写；
  **不得跨线程移交所有权**。用于 Quoter/Tracker/Router/Bridge/Portfolio/RiskMonitor halt 域/
  Calibrator 等公开方法间嵌套调用结构。

**LockFreeQueue.hpp —— SPSC 无锁队列**

容量须 2 的幂 static_assert；alignas(64) 修饰 head/tail/buffer 防伪共享；capacity()==Capacity−1
恒空一槽区分满空；tryPush release 发布/tryPop acquire 读取；popAll 批量排空；析构 drain（成员可含
std::string）。附带 BlockingQueue(mutex+cv MPMC 替代品) 供非热路径多生产者场景。

**TscClock.h —— rdtsc 时钟**

now()=_mm_lfence+__rdtsc（V8-R4 加 lfence 序列化防乱序漂移；单次 ~6-8ns 比 chrono 低 3 倍）；
calibrate() 幂等：cpuid 探测 invariant-TSC（非 invariant error+返回 false 不再静默兜底 0.4ns/tick），
steady_clock 采样 10ms 求 ns/tick；maybeRecalibrate(interval_ms=60000) 低频重校准接口。

**FutuDataDefs.h / FutureTypes.h / AlphaTypes.h**

FutuDataDefs 为别名转发头（复用 UFT 框架 mmap 数据块定义 PositionBlock/OrderBlock/TradeBlock/
RoundBlock 等，支撑 .membin 持久化）；FutureTypes 定义 OrderFlag_Normal/FAK/FOK 常量；
AlphaTypes 为 alpha 相关类型定义。

## 5. 关键机制专述

### 5.1 B+ 订单槽状态机（撤单确认制）

2026-08-19/20 僵尸单事故（撤单"发送即遗忘"+tracker 5s 强制遗忘 → 撤单静默失效 → 僵尸单在
交易所存续冻结可平量）后的根治方案：

```
撤单发出: quoter.cancelLevelOrders → tryMarkPendingCancel(原子取得撤单权) → stra_cancel
          [id 保留在槽内, 挂单门禁=order_ids.empty()]
超时 300ms: tracker.checkAutoCancel → CancelAction(TIMEOUT) → coordinator 补发 stra_cancel (≤3 次)
3 次失败: setZombie(IS_ZOMBIE, 永不 force-untrack) → getZombieEscalations
          → coordinator: error 日志 + setZombieHalt(合约闩锁) + stra_cancel_all(fullCode) 引擎兜底
恢复: 存活集合清零 → retainZombieHalts 自动释放闩锁;
      通道恢复 onChannelReady → clearZombies() 返回 id 列表 → 引擎全撤 + 广播 quoter.onOrder 清孤儿槽
      → 交持仓对账
```

配套：live-only 事件驱动补挂（on_order 撤单终态后立即 requoteAfterFill，不等下一 tick；回测维持
下一 tick 保可复现）；sticky 判定从尾向头找最新 live 单（防 pendingCancel 头部残留致 churn 风暴）。

### 5.2 delta/position 语义边界

- **策略逻辑 = delta 口径**：skew/qty 衰减/义务报价/穿越授权/retreat 只用 delta
  （contract delta = position×hedge_ratio、portfolio delta），归一化分母只用软限
  contract_max_delta / portfolio_max_delta；
- **风控逻辑 = position 口径**：maxPosition 硬顶只用于风险闸门（halt_quoting/pending_drain/
  side_pause/taker reduce），触发即暂停；
- 落地：PreTradeDecision 双层结构、checkHardPositionRisk 与 computeInventoryStrategyInputs 拆分、
  SpreadOptimizer 单路径 skew、getRawPortfolioDeltaUtilization 全链路唯一口径。有专项单测护栏。

### 5.3 回调大锁与去大锁路线（FUTU_CB_LOCK_BIG）

`_cb_mtx`(recursive_mutex) 串行化全部回调入口：代价不是平均开销而是**尾延迟**（实盘 on_trade 到达
时若 MdSpi 正在锁内发 CTP 单可达 µs~ms 级）。去大锁工程按"属主化+快照+投递"设计原则推进：

| 工作流 | 内容 | 状态 |
|---|---|---|
| WS-A | 停机域收编：_halt_category atomic 化 + _halt_domain_lock 临界区（含 _closeout_state） | ✅ 已落地 |
| WS-B | Portfolio 快照优化 | 降级可选（Portfolio 已全方法递归锁，无正确性缺口） |
| WS-C | TradingState 单写者收敛（EventDispatcher 投递低频处置转移） | 待独立会话 |
| WS-D | 成交路径保持同步（B+ 补挂留 TdSpi 不迁移） | ✅ 设计如此 |
| WS-E | 罕见重操作命令化（PendingCommand 属主域拆分：Channel=Td 域自 drain / Session=Md 域 on_tick 消费） | ✅ 已落地 |
| WS-F | 两态编译开关 `FUTU_CB_LOCK_BIG=big|none`（默认 big） | ✅ 脚手架就绪 |

none 模式已完成的硬阻断清零：AsyncArbitrageExecutor._oid_to_pair 加锁、SelfTradeCalibrator 全方法
收编、PerformanceMonitor 17 字段原子化、_blocked_contracts 死字段删除、_violations_buf 局部化。
回测验证：none 模式四张 CSV 与 big 基线逐字节一致。切 none 收官流程（验收性质）：TSAN 构建→
_ec_5d 全量逐比特 A/B→refreshQuotes 持锁时长埋点量化→生产灰度 p99/p999→删除 _cb_mtx（单独
commit 可 revert）。

### 5.4 回测可复现性（replay 时钟体系）

- `_exchange_time_ms` 由 tick actiondate/actiontime 合成，注入 coordinator/router/risk_monitor/
  arb_manager(setNowMs)/async_arb(setReplayNowUs)；PerformanceAnalyzer/SelfTradeCalibrator/
  requote 限频等全部统一该时钟（修过多处墙钟/replay 双域混用导致的死代码或不可复现）；
- 回测必须 `useAsyncArbThread:false`（主线程同步）且策略配置显式 `isBacktest:true`——缺失时
  live-only 补挂在回测回调栈内同步发单会引发 mocker 迭代器失效死循环；
- 已知噪声源（框架层）：HftMocker srand(time(NULL)) 随机拆分成交量——同配置两次运行成交拆分序列
  不同，评估需接受噪声或多轮取均值；
- 验证方法论：逐比特 A/B（修复前后 .so 对比四张 CSV）、逆向编辑重建基线 .so 校验 md5。

### 5.5 收盘平仓端到端与 session 边界

见 §4.12。要点补充：生产环境 WtUftEngine 的 on_session_end 不触发（框架限制 WtUftTicker.cpp），
策略侧绕开方案 = section-break 定点 flush + closeout TRIGGERED 定点 flush + 周期 live 输出 +
当日文件 seed 重启续算；双边统计周期行内存积压定点排空（R6-b 把文件 I/O 移出 MdSpi 热路径）。

---

## 6. 配置体系

### 6.1 config.yaml 关键参数（src/WtFutuCore/config 权威源）

```yaml
anchorCode: SHFE.ag.ag2608          # 锚定合约（lead-lag lead 方/收盘时间基准）
isBacktest: false                   # 回测必须显式 true!
contracts:                          # 当前权威合约列表（白银跨期）
- { code: SHFE.ag.ag2608, maxPosition: 50, maxDelta: 30 }   # position 硬顶 / delta 软限
- { code: SHFE.ag.ag2612, maxPosition: 50, maxDelta: 30 }
quoting:    numLevels 2 / obligationLevel 1(L0 自由探测+L1 义务层) / scoutQty 1.0 /
            baseSpread 2.0(GLFT 权威源) / baseQty 10.0 / levelQtyMultiplier 0.7 / levelStep 1.0 /
            stickyThreshold 1.0 / improveRetreatRatio 2.0 / maxPriceDeviation 20 /
            useBilateralQuote false / priceProtection true / protectTicks 1.0 /
            qtyDecayFactor 2.0 / obligationMinQty 10 / obligationMaxSpreadTicks 10
portfolio:  maxDelta 30 / hedgeRatio 1.0
risk:       maxExposure 2000万 / maxDailyLoss -200000(内部取 abs)
            frequency: orders 50/s cancels 30/s trades 20/s / maxDeltaChangePerSec 50
            cooldownMs 30000 / checkIntervalMs 5000 / recoveryThreshold 0.8 /
            maxRecoveryCount 3 / pnlRecoveryRatio 0.5 / autoClearIrreversibleOnReset false
            positionBreachPauseThreshold 1.2 / hardBlockRatio 1.0 / deltaCriticalMult 1.5 /
            warningL1 0.8(×1.5) L2 0.9(×2.0) / 同侧熔断 3 次·3000ms·暂停5000ms
closeout:   minutesBefore 2 / flattenPosition true / maxRetries 10 / retryIntervalMs 2000 /
            nightMinutesBefore 2 / drainTimeoutMs 3000 / depthRatio 0.3·0.5·0.8 /
            sweepThresholdMs 5000 / sweepTicks 3 / useFak true
下单控制:    orderErrorThreshold 3 / maxOrders 32 / maxPendingPerSide 30
monitor:    enabled true / flushIntervalMs 1000
```

### 6.2 coordinator.yaml 关键参数

```yaml
coordinator:
  useMarketMaking true / useSpreadArbitrage true / useAsyncArbThread false   # 实盘改 true!
  usePerformanceMonitor false / usePerformanceAnalyzer false / use_signal_aggregator true
  takerReduceThreshold 1.1 / targetUtil 0.8 / cooldownMs 30000
  requoteAfterFillMinIntervalMs 200 / sectionBreakSecondsBefore 10
  bilateralStatsLogIntervalSec 300
  pipeline: alphaSensitivity 2.0
  modules:
    signalAggregator:
      signals: ofi.window 50 / trade_flow.window 100 + largeTradeThreshold 50 /
               book_imbalance.threshold 0.2 / momentum.window 50 + emaAlpha 0.1 / lead_lag.window 50
      model: type linear / weights ofi .35 trade .25 book .20 mom .15 ll .05 / strongThreshold 0.7
      volatility: window 100 / elevatedThreshold 0.0005 / extremeThreshold 0.0017 (实测标定)
      warmupTicks 20
    toxicityDetector: adverseThreshold 0.75 / vpinThreshold 0.60 / vpinWeight 0.5 /
        window 20 / bucketSize 50 / minWarmupBuckets 5 / cooloffMs 5000 /
        extremeSignalThreshold 0.9
    spreadOptimizer(GLFT): phi 0.20 / deltaSkewThreshold 0.3 / Factor 1.5 / Power 1.5 /
        maxSpreadMult 3.0 / skewCrossMaxTicks 3.0 / portfolioSkewWeight 0.5 ...
    selfTradeCalibrator: retreatTicks 3 / retreatCooldownMs 5000 / adverseThreshold 0.6
    selfTradePrevention: enabled true / stpMinPriceGap 1.0
    autoCancel(B+): maxAgeMs 10000 / staleExtensionTicks 2.0(B1: STALE 延寿阈值,
        原 priceDeviation 键删除——其消费者为死接口) /
        cancelRetryIntervalMs 300 / cancelMaxRetries 3
    correlationManager: windowSize 100 / minCorrelation 0.5 / spreadZThreshold 2.0
```

### 6.3 spread_arbitrage.yaml 关键参数

```yaml
spread_arbitrage:
  enabled true / primaryStrategy mean_reversion / maxTotalPosition 20 / maxPairs 3
  minSignalConfidence 0.5(单键 R3 统一) / signalCooldownMs 2000 / minProfitThresholdTicks 1.0
  arb_close: enabled false(C0 出厂关闭=纯 B-3) / stop_loss FAK+1000ms / timeout GFD+30000ms 升级
             max_close_size_pct 0.5 / close_in_flight_timeout_ms 5000(B4) /
             oversold_protection true + overshoot_cooldown_ms 3600000(B5) / intent_broadcast true(B1)
  pairs: [ id ag0812, leg1 SHFE.ag.ag2608, leg2 SHFE.ag.ag2612, ratio 1.0,
           entryZScore 2.0 / exitZScore 0.5 / stopLossZ 4.0 / windowSize 200 ]
  risk_limits: portfolioStopLoss 50000 / maxTotalPosition 50 / maxSinglePair 20 /
               maxCorrelationBreak 0.3 / maxDivergenceZscore 5.0 / maxDivergenceTime 7200s
```

### 6.4 热参与配置文件的一致性约定

**优先级规则**（按参数范围）：

| 参数范围 | 稳态权威 | 说明 |
|---|---|---|
| 27 个热参数（见 §4.17） | **hotparams.yaml > 共享内存残留 > config.yaml/coordinator.yaml** | 实盘启动后 ~1s 内文件值覆盖一切；重启时共享内存旧值立即生效，期间 config 同名键改动无效 |
| 其余全部参数 | config.yaml / coordinator.yaml | 运行中改文件不生效，须重启 |
| 回测 | 一律 config/coordinator | watcher 不启动、热参完全不生效 |

运维约定：

- 27 键日常调参**只改 hotparams.yaml**（运行目录副本），config/coordinator 视为出厂基线；
  两边都改容易漂移——漂移键启动日志会以 `[HOTPARAM-DRIFT]` 逐条列出；
- **回测不吃热参**：若存在 `[HOTPARAM-DRIFT]` 差异键，回测验证的参数组合≠实盘运行组合，
  A/B 结论失真，须先对齐再回归；
- 越界值（如 base_spread>20、protect_ticks=0）在热路径被拒收并 warn 保留旧值；参数间关系
  （权重和/义务深度/软限 vs 硬顶/GLFT 区间）由 applyAll 后交叉复查以 `[HOTPARAM-CHECK]` 告警。

---

## 7. 构建、部署与回测

### 7.1 构建

```bash
# Linux 全量构建
cd src/build_all && cmake .. && make -j$(nproc)
# 或单模块
cd src/build_all && make -j$(nproc) WtFutuCore
```

产物为动态库 `libWtFutuCore.so`（CMake ADD_LIBRARY SHARED），输出至
`build_x64/<Cfg>/bin/WtUftRunner/futu/`。编译选项哲学：C++17；**不用 -flto/-march/native**
（ABI 兼容）；**不用 -ffast-math**（隐含 -ffinite-math-only 会优化掉 NaN 检查使风控静默失效，
改用 `-fno-math-errno -fno-trapping-math -ffp-contract=fast` 子集）+ `-O3 -funroll-loops
-fno-omit-frame-pointer`。链接 WtUftCore/WTSTools/Share + boost_filesystem/dl/pthread/atomic。
Windows 用 `src/all.sln`，需 `MyDepends141` 环境变量。

### 7.2 部署到回测目录

```bash
cp src/build_all/build_x64/Debug/bin/WtUftRunner/futu/libWtFutuCore.so dist/WtBtFutu/uft/
cp src/WtFutuCore/config/coordinator.yaml dist/WtBtFutu/
```

配置的**权威来源是 src/WtFutuCore/config/**；dist 下运行副本允许调参（如 _ec_5d.yaml）。

### 7.3 回测

```bash
cd dist/WtBtFutu
LD_LIBRARY_PATH=./uft:$LD_LIBRARY_PATH timeout 900 ./uft/WtBtRunner -c <config.yaml> \
    -l logcfgbt.yaml < /dev/null
```

- 策略日志 `outputs/Strategy_uft.log`、Runner 日志 `outputs/Runner.log`；
- 成交/资金/持仓/平仓 `outputs_bt/uft/{trades,funds,positions,closes}.csv`；
- 判据惯例：EXIT=0、0 HALT/0 zombie/0 timeout/error、session end Delta=0、资金曲线收敛；
  行为对比看成交笔数/TOXIC 抑制事件/dynbalance（mocker 随机拆分有 ±1.5% 左右噪声带）。

### 7.4 生产部署要点（详见 AGENTS.md §5）

Release 版部署远程（**禁止 Debug 版**，assert 风险）；cron 值守工作日 08:40/20:40 重启；
**同一账户任何时刻只允许一个 WtUftRunner 实例**（kill 后轮询确认退出再启动）；配置变更先备份再改。

---

## 8. 测试体系

- 单元测试位于 `src/TestUnits`（GoogleTest），当前基线约 **113/115**（2 个既有环境性失败：
  test_session.test_allday / test_shm.test_sharehelper），构建目标 `TestUnits` 后运行二进制；
- 覆盖模块举例：test_order_slot_bplus（B+ 状态机 9 用例）、test_inventory_delta_separation
  （delta/position 语义边界 8 用例）、test_toxicity_direction（毒性方向映射）、
  test_hot_param_manager（热参解析边界）、test_v9_r6a（halt 域并发 hammer 4 线程×20000 次/
  IRREVERSIBLE 语义/恢复冷却）等；
- 回测回归：`dist/WtBtFutu/_ec_5d.yaml` 为全量基准配置；行为变化验证采用 A/B 对照 +
  归因记录（预期变化项 vs mocker 噪声带）；
- 并发正确性：双线程 hammer 用例（CalibratorClosing.ConcurrentFillTickNoCrash 等）；
  TSAN 兼容（RecursiveSpinLock 基于 atomic_flag acq/rel 可正确识别 happens-before）。

---

## 9. 已知框架层限制与文档索引

**已知外部限制（记录于 AGENTS.md"已知外部限制"，禁止越界修复，待框架层处理）**：

1. HftMocker 回测随机拆分不可复现（srand(time(NULL)) 等）；
2. WtUftEngine::on_session_end 生产不触发（策略侧已绕开）；
3. 柜台账户资源类拒单（CTP 50/51/31）缺少分类通道——策略层只做通用错误计数；
4. 成交审计账(trades.csv)与事件流不一致疑似框架 CSV 落地问题；
5. stra_cancel_all 的 code 匹配口径实盘用 fullCode 两段式——策略兜底必须传 fullCode 或空串；
6. 框架层已打补丁（越界修改记录）：UftStraContext DATA_SIZE_STEP 8000→200000（做市高频扩容）、
   WtUftRunner 补调 initEvtNotifier（GUI MQ 通道）。

**文档索引**：

| 文档 | 内容 |
|---|---|
| `AGENTS.md`（本目录） | 开发工作流规范（先规划后执行）、全部历史方案实施记录与验证数据、已知外部限制、构建/回测/生产运维流程 |
| `docs/DEEP_ANALYSIS_V5~V7.md` | 历史深度诊断报告 |
| `docs/DIAGNOSTIC_REPORT_V8.md` | V8 全量诊断（R1~R5 修复来源，60+ 条目） |
| `docs/ARB_SELF_CLOSE_DESIGN.md` | 套利自平仓/B-3 门/C0-C2 分级平仓设计 |
| `docs/MM_SOFT_RISK_V3.md` / `MM_V3_TUNING_RESULTS.md` | v3 软风控设计与调参结果 |
| `docs/REFACTOR_ROADMAP.md` | 重构路线图 |
| `docs/FUTU_CALLBACK_LOCK_EVAL.md` | 回调锁评估（去大锁背景） |
| `L0_PATCH_DESIGN.md` / `OPTIMIZATION_REPORT.md` | L0 触板全停设计 / 优化报告 |

> 修改本子项目代码前请先阅读 `AGENTS.md` §1 工作流（查原因→设计方案→写入 AGENTS.md→用户确认→
> 动手），并遵守 §2 修改范围限制（仅 src/WtFutuCore 内；策略层不得解析柜台错误码）。
