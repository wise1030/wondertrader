# WtFutuCore — 期货高频做市引擎（UFT 架构）

> 版本基线：v9-R6（2026-08-24② 复核修复包后）。本文档为**代码级**架构手册：
> 所有关键逻辑均标注 `文件:行号` 锚点，可直接对照源码逐行检查。
> 配置字段的逐键说明见同目录 **CONFIG.md**。

---

## 目录

- [1. 项目定位与总体架构](#1-项目定位与总体架构)
- [2. 线程模型与并发契约](#2-线程模型与并发契约)
- [3. 单 tick 数据流全景](#3-单-tick-数据流全景)
- [4. 模块详解](#4-模块详解)
  - 4.1 UftFutuMmStrategy（策略壳）
  - 4.2 StrategyCoordinator（tick 流水线）
  - 4.3 FutuModuleAssembler（装配器）
  - 4.4 FutuConfigLoader（配置加载与校验）
  - 4.5 SpreadOptimizer（GLFT 报价数学 ★核心）
  - 4.6 信号体系（SignalAggregator + 六信号源）
  - 4.7 MarketDataContext（行情上下文）
  - 4.8 ToxicFlowDetector（VPIN 毒性检测）
  - 4.9 QuotePolicyChain（报价决策链）
  - 4.10 FutuQuoter（三层报价执行器）
  - 4.11 UnifiedOrderTracker（订单槽状态机 B+）
  - 4.12 FutuRiskMonitor（风控闸门群）
  - 4.13 RiskCoordinator（风控统一入口）
  - 4.14 TradingState（分层状态机）
  - 4.15 FutuPortfolio（持仓/Delta 账本）
  - 4.16 FutuRuntimeOps(事件处理)
  - 4.17 热参数体系（HotParamManager/Watcher）
  - 4.18 套利子系统（Arb 三件套）
  - 4.19 Closeout 平仓族（Trigger/Orchestrator/Executor）
  - 4.20 性能与监控
- [5. 关键设计裁决与语义边界](#5-关键设计裁决与语义边界)
- [6. 构建 / 回测 / 部署](#6-构建--回测--部署)
- [7. 测试体系](#7-测试体系)
- [8. 已知限制与勘误](#8-已知限制与勘误)

---

## 1. 项目定位与总体架构

WtFutuCore 是运行于 WonderTrader UFT 引擎之上的**期货高频做市 + 内套利**策略模块
（编译产物 `libWtFutuCore.so`，由 WtUftRunner 实盘加载或 WtBtRunner 回测加载）。

### 1.1 五层结构

```
┌─────────────────────────────────────────────────────────────┐
│ 壳层    UftFutuMmStrategy                                    │
│         13 个框架回调 → _cb_mtx 大锁串行化 → 分发             │
├─────────────────────────────────────────────────────────────┤
│ 装配层  FutuModuleAssembler + FutuConfigLoader               │
│         启动期一次性构建依赖图，wireDeps + validateDeps fail-fast│
├─────────────────────────────────────────────────────────────┤
│ 协调层  StrategyCoordinator（单 tick 主流水线）               │
│         QuotePolicyChain（报价决策六策略固定顺序链）           │
│         RiskCoordinator（风控统一入口）                       │
├─────────────────────────────────────────────────────────────┤
│ 领域层  SpreadOptimizer(GLFT) │ 信号族 │ ToxicFlowDetector    │
│         FutuQuoter │ UnifiedOrderTracker │ FutuPortfolio      │
│         FutuRiskMonitor │ Arb 三件套 │ Closeout 族            │
├─────────────────────────────────────────────────────────────┤
│ 设施层  TradingState(HSM) │ HotParamManager/Watcher │         │
│         TscClock │ PerformanceMonitor/Analyzer │ BilateralStats│
└─────────────────────────────────────────────────────────────┘
```

### 1.2 核心设计原则（读码前必读）

| 原则 | 内容 | 出处 |
|---|---|---|
| **delta/position 语义边界** | 策略逻辑(skew/qty衰减/义务/穿越)只用 **delta 口径**（分母=contract_max_delta/portfolio_max_delta）；风控闸门只用 **position 口径**（maxPosition 硬顶）。禁止混用 | AGENTS.md §2 |
| **暂停源单一化(D批)** | 下单错误风暴只走 `qphase=ERROR` 单轨；`_trading_halted` 只属于真正风控 halt。恢复预算留给后者 | FutuRuntimeOps.cpp:859-881 |
| **义务 vs 探测** | 报价层分义务层(obligationLevel)/自由探测层(scout)/弹性层(flexible)，scout 成交=逆向信号→撤同侧义务层 | FutuQuoter.h:91-94 |
| **B+ 订单槽状态机** | 撤单"确认后才清槽"+重试+zombie 升级，永不 force-untrack 活单 | UnifiedOrderTracker.cpp |
| **热参稳态权威链** | `hotparams.yaml > 共享内存残留 > config/coordinator`（实盘）；回测不跑 watcher | §4.17 |
| **可复现性让位随机性** | 回测 mocker 的 splitVolume 用 srand(time(NULL))——不存在逐比特判据，验证用统计带+健康度 | §8 |

---

## 2. 线程模型与并发契约

### 2.1 实盘线程真相（UFT 引擎不串行调度回调）

| 线程 | 承载回调 |
|---|---|
| **MdSpi**（行情） | on_tick / on_transaction / processTick 主流水线 |
| **TdSpi**（交易） | on_trade / on_order / on_entrust / on_channel_* |
| **RtTicker**（定时） | on_session_begin/end（实盘） |
| **arb 线程**（可选） | AsyncArbitrageExecutor 循环（回测必须关） |
| **hotparam watcher**（实盘专属） | 1Hz 轮询 hotparams.yaml → 只写共享内存+置脏 |

### 2.2 锁层级（自上而下）

| 保护对象 | 机制 | 位置 |
|---|---|---|
| 全部回调入口 | `_cb_mtx` recursive_mutex（大锁基线；两态宏 FUTU_CB_LOCK_BIG，none 需完整验收流程） | UftFutuMmStrategy.h:490, .cpp:72-79 |
| 罕见重操作命令化 | PendingCommand{ChannelReady/Lost/SessionBegin/End} 队列：`_cmd_mtx`+`_cmd_has_pending`(快路径 1 次 acquire load)+`_cmd_executing`(单飞 claim)；Channel=Td 域发布者自 drain，Session=Md 域 on_tick 消费 | UftFutuMmStrategy.h:455-459, .cpp:844-912 |
| FutuQuoter 槽位 | 全方法 RecursiveSpinLock | FutuQuoter.cpp |
| UnifiedOrderTracker | RecursiveSpinLock + generation 号 | UnifiedOrderTracker.h |
| GLFT 参数 | seqlock 一致性快照 snapshotParams()（防热更撕裂读） | SpreadOptimizer.cpp:37 |
| RiskMonitor halt/closeout 域 | `_halt_category` atomic + `_halt_domain_lock` RecursiveSpinLock（V8-R6/WS-A 收编） | FutuRiskMonitor.h:578+, .cpp:598 |
| RiskMonitor 频控环 | `_rate_lock` atomic_flag（recordOrders 批量接口修复双生产者违规） | FutuRiskMonitor.cpp |
| 中间价表 | MidSlot{atomic<double>} per-contract，Coordinator 唯一属主 | StrategyCoordinator.h:411-423 |
| CachedQuote | RecursiveSpinLock（processQuoting 写 :1160/requoteAfterFill 读 :1286，锁内拷贝锁外发单） | StrategyCoordinator.h:453-455 |
| SelfTradeCalibrator | RecursiveSpinLock `_lock`（onTick=Md / recordFill=Td / getFillRetreat=Md+Td） | SelfTradeCalibrator |
| Portfolio | 全方法 RecursiveSpinLock | FutuPortfolio.h:339 |
| arb oid→pair 映射 | `_oid_pair_lock`（tag 写 Md / consume 读删 Td） | AsyncArbitrageExecutor |
| PerformanceMonitor 计数器 | ThroughputStats 17 字段全 atomic<uint64_t> | PerformanceMonitor.h |

### 2.3 回测 vs 实盘行为分叉点全集

| 位置 | 差异 |
|---|---|
| UftFutuMmStrategy.cpp:257 | `_is_backtest` 来自配置 `isBacktest`（**默认 false，回测必须显式 true**，否则 live-only 补挂在回调栈内发单→mocker 迭代器失效死循环） |
| UftFutuMmStrategy.cpp:405-407 | 热参 watcher 仅实盘启动；回测只有 logDriftSummary 打印差异 |
| FutuRuntimeOps.cpp:176-185 | on_trade 补挂：回测=条件式撤单（不回调内挂新单）；实盘=同步 requoteAfterFill |
| FutuRuntimeOps.cpp:948-950 | 撤单终态事件驱动补挂：live-only |
| FutuRuntimeOps.cpp:645-652 | async arb 线程仅实盘 start（coordinator 必须 useAsyncArbThread:false） |

---

## 3. 单 tick 数据流全景

```
WTSTickData (MdSpi)
   │
   ▼
UftFutuMmStrategy::on_tick (cpp:615) ── _cb_mtx ──────────────────────────┐
  ├ drainPendingCommands(Session 域)                    (:622)            │
  ├ 热参 drain: consumePendingApply→on_params_updated   (:627) ←watcher置脏│
  ├ handleQuotingAutoResume (ERROR 指数退避试探)          (:637,:477)       │
  ├ replay 时钟推进 _exchange_time_ms (A5 注入全家)       (:643)            │
  ├ handleMarketDataUpdate → markToMarket/correlation    (:664,:506)       │
  ├ handleLeadLagPush (anchor mid → 非 anchor 聚合器)     (:679)            │
  └ handleCoordinatorTick                                 (:682)            │
       ├ drainTdSpiLogs (SPSC 日志卸载排空)                (:581)            │
       ├ coordinator 空 → FAIL-SAFE 撤全单+RISK_HALTED     (:584)            │
       ├ StrategyCoordinator::processTick  ══════════════► 见 §4.2        │
       ├ recordQuote → MonitorBridge                                        │
       └ closeout_orch.onTick                                               │
                                                                            │
并行地（TdSpi）:                                                             │
  on_trade  → processTradeFill (FutuRuntimeOps.cpp:35)                      │
              breaker→簿记对账→quoter.onTrade→补挂(实盘)→恢复四道闸(:342)     │
  on_order  → onOrderEvent (:894) 槽态更新→live-only 补挂→STP/router 清理    │
  on_entrust→ onEntrust (:759) 成功清错误计数 / 失败计数++→D1 ERROR 单轨     │
  on_channel→ ready: zombie 清扫+持仓同步+局部缓冲 checkRiskLimits (:401)    │
              lost: RISK_HALTED+haltTrading(REVERSIBLE)+cancelAll (:981)     │
```

### 3.1 StrategyCoordinator::processTick 流水线阶段

| Stage | 内容 | 行号(StrategyCoordinator.cpp) |
|---|---|---|
| 0 | closeout 触发判定（活跃平仓期仍跑硬风控） | :494-509 |
| 0.5 | section break（每节收盘前 N 秒边沿进入撤 MM 单+arb 在途单） | :641-711 |
| 1 | TickContext 组装（价格/时间/cs 快照/组合聚合值） | — |
| 2 | updateSignals：SignalAggregator.update → AlphaResult（ofi_component 搬运 T1） | :879-963 |
| 3 | 毒性更新（updateToxicity 显式入口 A6）+ cooloff 判定 | — |
| 4 | RiskCoordinator::checkRisk 统一风控入口 | :570 |
| 5 | processAutoCancel（B+ 重试/zombie 升级/retainZombieHalts） | — |
| 6 | processQuoting：QuotePolicyChain.run → SpreadOptimizer.computeOptimalQuote → FutuQuoter.refreshQuotes | :1160+ |
| 7 | taker 减仓检查（checkTakerReduce）/双边统计定点落盘 | — |

---

## 4. 模块详解

### 4.1 UftFutuMmStrategy（策略壳）— UftFutuMmStrategy.h/.cpp

**职责**：持有全部业务模块；把框架 13 个回调经 `_cb_mtx` 串行化后分发到协调器/运行时操作。

#### 关键成员

| 成员 | 含义 | 行号(.h) |
|---|---|---|
| `_config` | FutuMmConfig 全量配置 | 341 |
| `_portfolio` / `_quoters` / `_spread_optimizers` | 组合账本/每合约报价器/GLFT 优化器 | 353/359/362 |
| `_order_tracker` / `_coordinator` | 统一订单跟踪/tick 流水线 | 365/368 |
| `_market_data` / `_signal_aggregators` | L2 上下文/信号聚合器(每合约) | 370/373 |
| `_risk_monitor` / `_closeout_orch` / `_closeout_executor` | 风控/平仓编排/渐进执行器 | 376/382/385 |
| `_toxicity_detector` / `_self_trade_calibrator` | VPIN 毒性/自成交校准 | 388/395 |
| `_spread_arb_manager`/`_stp`/`_order_router`/`_async_arb`/`_arb_bridge` | 套利管理/自成交防护/统一下单路由/异步执行/套利桥 | 410-422 |
| `PendingCommand{...}` + `_cmd_*` | WS-E 罕见重操作命令化队列 | 438-459 |
| `_trading_state` | TradingState HSM（属主=壳层） | 518 |
| `_order_error_count` / `_quoting_paused_since` | 连续下单错误计数/ERROR 起始时刻(atomic) | 533/535 |
| `_exchange_time_ms` | replay 主时钟(ms, atomic, A5 全家注入) | 541 |
| `_hot_mgr` / `_hot_watcher` | 热参管理/watcher(实盘专属) | 555/556 |
| `_tdspi_log_queue` | 成交路径日志 SPSC 卸载队列(C11) | 496 |

#### on_init 启动序列（.cpp:249-443）

1. `TradingState::setExternalLocking(true)`(:253) → 存 `_main_ctx`、置 `_is_backtest`(:256-259)
2. EventNotifier 注入（RiskMonitor 直达; ArbManager 经 handleRiskAlert 转发，EMERGENCY→IRREVERSIBLE halt+disable arb, :262-266/:184-233）
3. `loadContractInfos`(:273→Assembler:747)：multiplier/tickSize 缺省从基础数据回填、收盘时间从 session 推导
4. `FutuModuleAssembler::assemble`(:276/:242) — 见 §4.3
5. 配置二次校验块（信号权重和/GLFT 范围/maxDelta-vs-maxPosition 语义告警 :281-341）
6. **热参注册**（:351-381）：从首个 optimizer/aggregator 读默认值，anchor 的 ci.max_delta 作 contract_max_delta 默认 → `registerParams`
7. **启动期 initial applyAll**（:385-395）——共享内存残留旧值立即生效（"重启不丢上次热更结果"）；随后**无条件** `logDriftSummary`(:397-399，回测也打印)
8. 仅实盘 `_hot_watcher.start(...1000ms)`(:405-407)
9. 订阅全部合约行情(:431)、MonitorBridge init、arb 信号回调接线(:420-428)

#### 回调路径速查

| 回调 | 实现 | 要点 |
|---|---|---|
| on_tick (:615-699) | MdSpi | 流程见 §3；CLOSEOUT/section-break 中不喂 arb tick(:690-693) |
| on_session_begin/end (:445-471) | post 命令(Md 域)，on_tick 消费 | 实际序列 FutuRuntimeOps.cpp:602-757：resetDaily→resetCloseout(true)→TradingState.reset→错误计数清零→async arb start(仅实盘开关)→双边统计 seed 重启续算；End: enterCloseout→arb stop→cancelAll→绩效报告→finalizeAtSessionEnd |
| on_trade (:757) | TdSpi | → processTradeFill §4.16 |
| on_order (:766) | TdSpi | → onOrderEvent §4.16 |
| on_entrust (:914) | TdSpi(无 ctx 用 _main_ctx) | → onEntrust §4.16 |
| on_channel_ready/lost (:818-832) | Td 域命令自 drain | → §4.16 |
| handleQuotingAutoResume (:477-504) | on_tick 内调用 | qphase==ERROR 按错误计数指数退避 10s×2^n 上限 60s 试探 tryResumeFrom(ERROR) |

---

### 4.2 StrategyCoordinator（tick 流水线）— StrategyCoordinator.h/.cpp

**职责**：单 tick 主流水线编排 + 报价决策链 + 双边统计/session 阶段管理。

#### 核心数据结构

| 结构 | 内容 | 行号(.h) |
|---|---|---|
| `TickContext` | 价格/时间/组件指针缓存/cs 快照/组合聚合值 | 56-103 |
| `ProcessingResult` | processed/quote_placed/order_canceled/closeout_executed/耗时 | 106-120 |
| `ModuleParams` | portfolio_max_delta(:126)/toxicity_cooloff_ms(:129)/auto_cancel_max_age_ms(:132)/**stale_extension_ticks(:136,B1)**/cancel_retry_interval_ms(:138)/cancel_max_retries(:139)/alpha_sensitivity(:144)/cold_start_confidence_factor(:145) | 123-146 |
| `CoordinatorConfig` | 开关+taker 减仓三键+requote 限频+section break 秒+双边统计参数；`_raw_variant` 存档(:191, 装配期 8 处使用) | 149-202 |
| `CoordinatorDeps` | B7 一次性依赖注入结构（15 setter 合一） | 213-235 |
| `_quote_chain` | QuotePolicyChain 六策略（§4.9） | 438 |
| `_last_mid` | MidSlot 原子中间价表（唯一属主，getLastMid :419-423） | 414-415 |
| `_global_portfolio_ctx` | 组合级聚合缓存（含 C2 的 contract_realized_delta_util） | 406 |

#### loadConfigFromVariant 解析行号（.cpp:201-348）

raw 存档 :206｜根级开关 :237-239｜modules 开关 :242-270（MM=false 级联关 toxicity/spreadOptimizer :260-264）｜pipeline :273-280（paramUpdateInterval=0 强制改 1 防 SIGFPE :277）｜toxicity cooloffMs :289-292｜autoCancel 四键 :299-310｜taker 减仓 :321-323｜requote 限频 :326-327｜sectionBreak 秒/分钟兼容 :331-335｜双边统计 :338-341。

---

### 4.3 FutuModuleAssembler（装配器）— FutuModuleAssembler.cpp

**装配次序**（依赖决定顺序，:59-745）：

```
0   StrategyCoordinator + loadConfig(coordinator.yaml)          :92-96
0a  策略级 closeout/perf 参数覆写 setConfig                      :99-113
0b  七开关回填 coordBool (MM/Arb/AsyncArbThread/PerfMon/
    PerfAna/signal_aggregator/Hedging根级唯一权威)               :119-158
    └ STP 读 modules.selfTradePrevention (:149-157);
      arb=true 强制 STP ON (:309-313)
1   FutuPortfolio (params/anchor/addContract 循环)              :167-191
2   CorrelationManager + 两两 addRelation                       :196-218
2'  FutuQuoter 每合约 (scout/义务参数 :248-253)                  :223-266
3   SpreadOptimizer 每合约 (createSpreadOptimizer 工厂)          :271-287
4   UnifiedOrderTracker (tracker_cfg) → quoter 注入 tracker
    → per-Quoter BilateralStats                                 :292-344
5.6 OrderRouter (arb=30/s, hedge=30/s, closeout 不限速)          :365-374
5.7 CloseoutExecutor + session 表 + initialize()                :379-405
6   FutuRiskMonitor (RateLimits/RecoveryConfig/CloseoutConfig)
    + CloseoutOrchestrator.setDeps                              :412-465
7   MarketDataContext 每合约 (setContract+setLargeTradeThreshold
    单一口径 P0-4)                                              :472-476
7.1 SignalAggregator 每合约 (model.type 无效→error+return :492;
    lead-lag anchor 接线 :500-507)                              :481-522
9   ToxicFlowDetector (工厂)                                    :534-545
9.5 SelfTradeCalibrator (anchor tick_size, 注入毒性检测器)        :550-574
11  PerformanceAnalyzer                                         :579-584
12  PerformanceMonitor (+TscClock::calibrate)                   :589-597
14  SpreadArbitrageManager (独立 yaml; Portfolio SSOT B-3;
    in_flight 120s)                                             :603-629
15  SelfTradePrevention (tracker 共享)                          :633-636
16  AsyncArbitrageExecutor + ArbExecutionBridge.setDeps
    (Portfolio 过冲保险丝 :652-654)                             :641-682
收尾 deps.trading_state 在 arb if/else 外(防段错误 :684-686)
    → wireDeps + validateDeps fail-fast 断言(require 列表)       :688-744
```

`loadContractInfos`（:747-826）：commInfo 查询→_session_cache 缓存；multiplier/tick_size=-1 时自动回填基础数据（查不到 1.0/0.2+warn）；白盘/夜盘收盘时间从 trading sections 推导并写回 `_config.closeout.*`(:802-805)。

---

### 4.4 FutuConfigLoader（配置加载与校验）

读取节行号：anchor/isBacktest :28-29｜config 路径 :34-37｜contracts :41-60（multiplier/tickSize=-1=待回填）｜risk :91-95｜quoting :100-120｜portfolio :125-129｜order_control :132-134｜closeout :137-155｜risk.frequency :170-184｜performance :189-196。

**error（拒启）校验清单**（.cpp:208-342）：

| # | 校验 | 行号 |
|---|---|---|
| E1-E3 | contracts 缺失/非数组/空；anchorCode 空；anchor 不在 contracts | 64-86 |
| E4/E5 | maxDelta ∉ (0,1e8]；maxExposure ≤ 0 | 210-219 |
| E6-E8 | numLevels ∉ [1,10]；levelStep ∉ (0,100]；obligationLevel ≥ numLevels | 222-240 |
| E9 | useBilateralQuote=true 与 obligationLevel≠0 互斥（路径 A 硬编码 L0 双边） | 251-257 |
| E10-E12 | baseSpread ∉ (0,20]；baseQty ∉ (0,100]；obligationMinQty ≤ 0 | 258-273 |
| E13/E14 | hedgeRatio ∉ [0,1]；maxOrdersPerSec ∉ [1,500] | 312-339 |

**warn（放行）**：W1 scoutQty>(0,baseQty]｜W2 obligationMinQty<scoutQty｜W3 全侧满挂总深度<obligationMinQty（三层公式累加 :283-297）｜W4 levelQtyMultiplier ∉ [0.1,1.0]｜W5 stickyThreshold ∉ (0,10]｜W6 maxPriceDeviation ∉ [0,100]。

> **死键警示**：config 内 `modules:` 节与根级 `stpMinPriceGap` 自 V8-R6 起**零读取**（权威=coordinator.yaml）；`portfolio.hedgeDeltaThreshold/hedgeCooldownMs`、`alwaysObligation` 无消费者。详见 CONFIG.md §7。

---

### 4.5 SpreadOptimizer — GLFT 报价数学 ★核心（SpreadOptimizer.h/.cpp）

**职责**：把 (mid, alpha, 毒性, 库存, 波动率, 深度) 变成 (bid_price, ask_price, pause 标志)。
参数结构 `GLFTParams`（.h:19-120，fromVariant :89）经 **seqlock 快照** `snapshotParams()` 读取（防热更撕裂读，.cpp:37）。

#### 主函数 computeOptimalQuote 逐步（.cpp:31-249）

```
① base_spread = computeBaseSpread(ctx)                          :44
② spread_mult 复合乘子（统一管理所有非 delta 风险的加宽）:
   2a 毒性加宽: toxic_detected && score > params.toxicity_min_score(B4)
      → tox_mult = 1 + score × toxicity_spread_factor           :60-66
   2b 低置信保护: alpha.valid && conf < low_confidence_threshold
      → mult = 1 + (thr-conf)/thr × lowConfidenceSpreadFactor(M8) :70-74
   2c EMA 平滑(三常数):
      无风险事件: _s = 0.5×mult + 0.5×_s; 再 +0.05×(1-_s) 拉回    :81-86
      有风险事件: _s = 0.30×mult + 0.70×_s                       :87-90
      B-1 速率限幅: 上行 ≤+10%/tick 下行 ≤-15%/tick
        基准=上一 tick 最终输出值; 首 tick(_mult_initialized=false)
        跳过限幅直接采用 EMA 值(A2 显式化, 原 <0.5 魔数等价)       :96-106
   应用: base_spread ×= spread_mult                              :112
③ 公允价 = mid + alphaSensitivity × alpha × confidence_weight × tick_size
   confidence_weight = min + (max-min)×confidence                :117-118
   alpha_adjustment 截断 ≤ half_spread_price                     :122-125
④ delta skew（双维加权, 全 ticks 量纲 C1 后）:
   cross_authorized = |contract_realized_delta_util| ≥ 1.0
     （C2: realized 口径——主动减仓不被未成交挂单预授权）          :140-141
   contract_skew = f(contract_realized_delta_util, half_spread)  :147-150
   portfolio_skew = f(total_delta, half_spread)                  :153
   合成: 权重和>0 → w_p×portfolio + w_c×contract
         否则退回 max 模式(向前兼容)                             :159-164
⑤ 总偏移截断: clamp_limit = cross_authorized ?
     half_spread + skew_cross_max_ticks : half_spread            :173-174
⑦ bid/ask = fair_value ∓ half_spread_price + skew_price
   skew_price = total_skew × tick_size × spread_mult
   【有意设计】skew 随 spread_mult 放大：毒性高时"保护性+进攻性"
   协同加速出清库存（用户确认不得解耦）                           :180-184
   第二次截断上限含穿越扩展(spread_mult>1 时防截回抵消放大意图)    :189-193
   取整: bid 向下 floor / ask 向上 ceil 到 tick                   :198-199
⑧ crossed 保护: bid≥ask → pause_quoting=true 并回退无 skew 报价   :204-210
⑨ spread_mult_out = base_spread/base_spread_cfg;
   pause |= ctx.shouldPause() ||
            mult ≥ max_spread_mult × pause_spread_mult_ratio      :215-217
```

#### 子公式

**computeBaseSpread**（:251-267）：
```
depth_adj = avg_depth≤0 ? no_depth_spread_mult
          : 1/(1 + avg_depth/depth_normalization × depth_sens × depth_sens_scale)
spread = base_spread × depth_adj
       + phi × (vol_percentile/vol_percentile_scale) × vol_scale   ← phi 实际角色=
                                                                    波动率加价系数(A4)
clamp [base×min_mult, base×max_mult]
```

**computeContractDeltaSkew**（:269-292）——库存厌恶主通道：
```
util = |signed_util|;  direction = 多头→-1 空头→+1
norm = util^delta_skew_power × inventory_skew_gain      (:284)
cap  = util≥1 && cross_max>0 ? 1+cross_max/half : 1.0   (:287-290)
     （util<1 时 cap=1 保证义务合规——减仓侧最多贴 mid）
return direction × norm × half_spread_ticks
示例(gain=1,power=1.5): util=0.5→0.35×half, 0.8→0.72×half, 1.0→贴mid,
                        >1→穿越 mid 至多 cross_max_ticks(:281-283 注释)
```

**computePortfolioDeltaSkew**（:294-311）：
```
util = |total_delta|/portfolio_max_delta;  ≤threshold(delta_skew_threshold)→0
excess = util - threshold
return direction × delta_skew_factor × excess^power × half_spread_ticks
【C1 修正】×half_spread 归一到 ticks——此前返回无纲量被隐式当 ticks 与
contract 分量相加，相对力度随价差宽度漂移（与"contract 主导"注释矛盾）
```

---

### 4.6 信号体系（signals/）

#### SignalAggregator（SignalAggregator.h）

**职责**：六信号源槽位表驱动聚合 → AlphaResult{alpha, confidence, ofi_component}。

| 组件 | 行号(.h) |
|---|---|
| SignalAggregatorConfig::fromVariant（model.*/signals.*/volatility.* 全解析） | 78-186 |
| setConfig/updateWeights（热参运行时注入权重） | 199/215 |
| updateToxicity（A6 显式入口替代非 const getContext 双写者） | 277 |
| updateLeadContract/addLeadContract（lead-lag anchor 接线） | 293/303 |
| is_ready() = `_tick_count >= warmup_ticks` | 311 |
| initializeSignalSources（presence 即启用） | 331 |
| registerSlot（类型→源→归一器槽位表驱动） | 460 |
| computeAlpha（~110 行线性化：提取→权重→聚合→fallback→confidence；IC 记录/regime 已归还 AdaptiveWeightFramework R4b S-1/S-2） | 529 |
| initScaleTrackers（B3: rolling_window/rolling_interval/ic_update_interval 可配） | 725 |
| normalizeSignal/getDynamicWeight | 737/748 |

**两层权重架构**：
- Layer1 base weights：coordinator.yaml model.weights 五路（ofi/trade_flow/book_imbalance/momentum/lead_lag），热参可覆盖；
- Layer2 自适应：AdaptiveWeightFramework（ICWeightTracker.h）——`processTick` 单入口（R4b S-1/S-2 收拢 IC 簿记+regime MA 检测+computeWeights），rolling_window=500 归一化窗口、ic_update_interval=50 更新节拍、enabled[5] 掩码保证禁用信号不进分母（A-5）、BOOK regime 因子进 Config（book_deep/thin 1.3/0.7）。

#### 六信号源一览

| 源 | 文件 | 关键点 |
|---|---|---|
| OFISignalSource | OFISignalSource.h | order flow imbalance，窗口 signals.ofi.window=50；pressure 线性互补 0.5×(1±ofi)（S5 修复阶跃退化）；LEVEL 被用作 DEVIATION 的局限=R4 遗留 |
| TradeFlowSignalSource | TradeFlowSignalSource.h | largeTradeThreshold=50 手单一口径（P0-4 同步灌入 MarketDataContext/TickTransactionInferer）；方向推断在 onTransaction |
| BookImbalanceSignalSource | BookImbalanceSignalSource.h | threshold=0.2，距离加权依赖 tick_size 正确装配 |
| MomentumSignalSource | MomentumSignalSource.h | S6 后生效：最近 min(window,128) 收益 + emaAlpha=0.1 平滑 |
| LeadLagSignalSource | LeadLagSignalSource.h | anchor mid 推送驱动；**唯一持久化 last-value 通道**——B5 maxAgeMs 老化：update() 内 age 超限置 `_result.valid=false`（anchor 到来自愈），默认 0=关闭 |
| VolatilitySignalSource | VolatilitySignalSource.h(.cpp) | realized_vol 分档：ELEVATED≥elevatedThreshold(0.0005≈p95)→widen、EXTREME≥extremeThreshold(0.0017≈p99.5)→pause（S10 按 EC 4 日分布实测标定）；statsLogInterval 埋点工具默认关 |

---

### 4.7 MarketDataContext（行情上下文）— signals/MarketDataContext.h

组合对象 = OrderBookStateTracker(_state) + TickTransactionInferer(_flow)：

| 接口 | 行号 | 说明 |
|---|---|---|
| setContract(code,tickSize,depthLevels) | :122/:208 | P0-4 起由工厂必调（此前恒默认 0.2 造成 EC 盘口偏差 2.5×） |
| onTick(tick) | :123/:215 | 快照更新+derived metrics；首帧未装配一次性 warn |
| estimateLiquidity | :128 | 深度流动性估计 |
| OrderBookStateTracker::updateDerivedMetrics/calculateImbalance/calculateDepthImbalance | :151-153 | 单边盘口 mid/spread 清零（S3 对齐策略层 C1） |
| TickTransactionInferer::onTransaction | :167 | 成交方向分类（P2-4 重构后唯一归属；旧策略层 last_mid==0 默认 isBuy=true 偏差源已删）+大单判定；InferenceRecord 记录原始量对称增减（R3 数值泄漏修复） |
| setLargeTradeThreshold | :213 | 顺带把 tick_size 灌入 inferer |

---

### 4.8 ToxicFlowDetector（VPIN 毒性检测）— ToxicFlowDetector.h

**三层毒性响应联动**（A4 文档固化）：
1. GLFT 加宽（toxicity_spread_factor，score 过 toxicity_min_score 门槛）— §4.5②a
2. 本模块 is_toxic → ToxicityPolicy 停边 + cooloffMs 冷却（qphase=TOXICITY）— §4.9
3. GLFT pause（spread_mult ≥ max×0.9）— §4.5⑨

**评分管线**：

```
PredictiveToxicity (VPIN 桶算法):
  onTrade(price,qty,isBuy,ts) → 桶累计 buy/sell
  bucket_size 满 → 封桶 imbalance=|buy-sell|/total∈[0,1](T3 经典口径)
  vpin = 窗口(window=20 桶)内桶归一均值; minWarmupBuckets=5 预热门
RealizedToxicity:
  self-trade 校准 + adverse move; updateCache 无内部权重(T2 加权归门面单次施加)
门面 combined (ToxicityParams.fromVariant .h:53-96):
  combined = vpinWeight(0.5)×vpin_norm + (1-vpinWeight)×alpha_toxicity
  alpha_toxicity = normalize(alphaWeight×ofi_ch, bookWeight×book_ch)   ←T6 通道内归一
                 + selfTradeWeight(0.4)×selftrade(单次施加 T2)
  extreme 兜底: |signal|≥extremeSignalThreshold(0.9,R2) → extremeSignalWeight
is_toxic = combined > adverseThreshold(0.75)
         OR (vpin_ready && vpin > vpinThreshold(0.60))     ←T4 恢复 OR 条件
方向 toxic_side: ofi_component 与 book_imbalance 同号时单边抑制
  (1=激进买流→抑制 ask; -1→抑制 bid; QuotePolicyChain §6.2 语义)
冷却 cooloffMs(5000): RiskCoordinator.cpp:347-356 进入/退出 TOXICITY 相位
```

加载期边界校验（adverse/vpin/weights 越界 warn 回落默认，fromVariant :69-94）。

---

### 4.9 QuotePolicyChain（报价决策链）— QuotePolicyChain.h

固定顺序执行六策略（与旧 processQuoting 内联顺序一致，:391-393；chain.run 固定顺序保证=A4 边界声明）：

| # | 策略 | 行号 | 职责 |
|---|---|---|---|
| 1 | RiskWidenPolicy | :94 | 软风控 WIDEN_SPREAD 倍数注入（tickSoft 无状态逐 tick 重算 vs onHardWiden 升级路径——同 tick 覆盖顺序由链序保证） |
| 2 | ArbCloseSyncPolicy | :156 | ARB 平仓协同抑制（getArbCloseDirection A13: 冲突→双侧抑制 kConflict） |
| 3 | ToxicityPolicy | :209 | is_toxic 停边（1→ask / -1→bid，T5 方向交换后语义）+ cooloff 冷却 |
| 4 | LimitPricePolicy | :268 | 涨跌停保护 L0-L3 分档（v7.8 修复 spread_mult 死写→L0 实际拉宽 :308） |
| 5 | ColdStartPolicy | :344 | 信号未热身时 maxSpreadMult 保守报价（cold_start_confidence_factor=0.005） |
| 6 | FillRetreatPolicy | :372 | 成交后退让（SelfTradeCalibrator.retreatTicks 机制，取保守价） |

> 边界声明：本链是"风控响应与业务调整混链"——顺序即优先级，新增策略必须评估对下游的覆盖关系。

---

### 4.10 FutuQuoter（三层报价执行器）— FutuQuoter.h/.cpp

**层级模型**（.h:91-94）：`level < obligation_level` 为自由探测层（qty=min(qty,scoutQty)，.cpp:140-142）；`== obligation_level` 为义务层(base_qty)；之后为衰减 flexible 层（level_step 价距 × level_qty_multiplier 量衰减）。

| 接口 | 行号(.h) | 说明 |
|---|---|---|
| refreshQuotes(ctx, req) | :178 | 主入口：按 QuoteResult 逐层维护槽位 |
| cancelAll/cancelSide/cancelLevelOrders | :181/:185/:192 | 撤单族（B+ 下 id 保留至 onOrder 终态） |
| onScoutFillCancelObligation | :197/.cpp:639-663 | scout 成交=逆向信号→撤同侧义务层全部挂单 |
| onOrder/onEntrustAck/onTrade | :205/:213/:220 | 槽态回报更新（quoter 单上方清 level 状态） |
| checkStickyUpdate | :322 | 顶单黏性：价格改善 ≥ improve_retreat_ratio×sticky 才重挂（sticky_threshold 收窄为顶单黏性专用，B1） |
| applyPriceProtection | :417 | 价格保护带 protectTicks/maxPriceDeviation（开关 quoting.priceProtection） |
| needObligation | :422 | 义务触发判定（obligationMinQty 总深度阈值） |
| handleObligationQuote/handleFlexibleQuote | :434/:438 | sticky 判定扫描（order_ids[0] 依赖"B1/B2 槽内 pendingCancel 同质性"隐式不变量——V9 建议 A4-残余） |
| updateQuotingParams/updateStickyParams/updateProtectionParams/updateMaxPriceDeviation | :120/:134/:140/:146 | 四个热参入口 |

**义务报价语义**：义务层需要提前性——其 qty 衰减/force_obligation 用**前瞻口径**（含 pending 投影，computeInventoryStrategyInputs）；与 skew 的已实现口径形成刻意的分工（C2 语义固化）。

---

### 4.11 UnifiedOrderTracker（订单槽状态机 B+）— UnifiedOrderTracker.h/.cpp

**背景**：8/19-20 僵尸单事故（撤单"发送即遗忘"+force-untrack→撤单静默失效，交易所残留 858 手）。B+ 方案：撤单**确认后才清槽**+超时重试+zombie 升级。

| 接口 | 行号(.h) | 说明 |
|---|---|---|
| trackMMOrder/trackArbOrder | :372/:391 | 挂单入册（OrderFlags 位集：IS_BID/IS_ACTIVE/PENDING_CANCEL/IS_MM/IS_ARB/IS_ZOMBIE :56-162） |
| tryMarkPendingCancel(orderId,reason,now) | :440 | 发撤单前先标记（未知 id 返回 false→调用方兜底） |
| checkAutoCancel | .cpp | 超龄单(max_age_ms)：发撤单动作+cancel_time 刷新+retry_count++（间隔 cancel_retry_interval_ms=300）；K 次(cancel_max_retries=3)后置 IS_ZOMBIE（保留跟踪/计入 pending/升级告警）——**永不 force-untrack 活单** |
| STALE 延寿 | .cpp:459 | stale_extension_ticks(B1,默认2.0)：价格偏离超过此值的挂单才允许被 auto-cancel 判死（原 sticky×2 隐式值显式化） |
| untrackOrder | :425 | 仅终态（成交完全/撤单确认/拒单 finalizeOrder M1/M2 幂等） |
| recordOrderFill | .cpp:196 区 | 完全成交才 untrack+quote→fill 延迟埋点 |
| clearZombies() | :593 | 返回 untrack 的 zombie id 列表（onChannelReady 清扫锚点用） |
| getAliveZombieContracts | .cpp | zombie 清零合约重置 `_zombie_escalated` 去重（重新武装升级） |
| getPendingBuyQty/SellQty(AllSources) | :549/:558 | maxPendingPerSide 闸门输入（MM-only vs 全源两个口径） |

zombie 升级动作链（StrategyCoordinator processAutoCancel）：error 日志 → RiskMonitor.setZombieHalt 闩锁 → `stra_cancel_all(fullCode)` 引擎侧兜底（**必须 fullCode 或空串**，见 §8）。

---

### 4.12 FutuRiskMonitor（风控闸门群）— FutuRiskMonitor.h/.cpp

**两套口径铁律**：position 口径硬闸门（checkHardPositionRisk :1128）vs delta 口径策略输入（computeInventoryStrategyInputs :1201——分母只用 contract_max_delta :1209 注释）。

| 闸门 | 触发公式 | 触发后 |
|---|---|---|
| 频控 ORDER/CANCEL/TRADE_RATE | 滑窗计数 vs RateLimits（maxOrdersPerSec 等） | violation→分级 |
| DELTA_RATE | \|Δdelta\|/window_sec > maxDeltaChangePerSec（滑窗 deltaRateWindowSec=10s） | `_delta_rate_breached=true`+breach_time(:823-825)；恢复须 breached 转 false 且过 cooldownMs(:834-838)；Coordinator 侧 halt+同步撤 MM 单（停摆期黏单最危险） |
| EXPOSURE | Σ exposure > maxExposure | violation |
| DAILY_LOSS | dailyPnL < -\|maxDailyLoss\| | **IRREVERSIBLE** halt |
| POSITION_NET | \|net\| > maxPosition（per-contract, position 口径） | violation→BLOCK_SIDE/HALT |
| block_add | \|delta\| ≥ contract_max_delta × positionHardBlockRatio（delta 口径, :1262-1270） | 仅 flexible 加仓侧跳过（策略库存管理非风控措施） |
| SideFillBreaker | 同侧连续成交 maxConsecutiveSameSide/窗口 sameSideWindowMs | 撤该合约全单+side_pause（CLOSEOUT 豁免, RuntimeOps :86-97） |
| zombie halt 闩锁 | setZombieHalt(:255)/retainZombieHalts(:268) | per-contract 闩锁至清扫 |

**halt/recover 域**（WS-A 收编：`_halt_category` atomic + `_halt_domain_lock` 递归临界区）：

- `haltTrading(category, pnl_snapshot)`（.h:401）：REVERSIBLE（delta-rate/exposure/block_side/taker/channel-lost）vs IRREVERSIBLE（daily-loss/arb EMERGENCY）。IRREVERSIBLE 且非 closeout 窗口→forceFlatAll 强平（RiskCoordinator :215-225）
- 恢复预算：checkAndRecover(:596)→canRecover(:507)——节流(checkIntervalMs)+cooldownMs(30s)+次数熔断(maxRecoveryCount=3/session)+util/PnL 闸(recoveryThreshold/pnlRecoveryRatio)；IRREVERSIBLE 必拒（autoClearIrreversibleOnReset=true 时日界清除）
- closeout 子状态机 CloseoutSub（TRIGGERED/DRAINING/ASSESSING/EXECUTING/COMPLETED/FAILED）canTransitionTo(:200)；getCloseoutSubInfo **按值返回锁内拷贝**（拆大锁防撕裂视图）

---

### 4.13 RiskCoordinator（风控统一入口）— RiskCoordinator.h/.cpp

`checkRisk(ctx, tc, in_cooloff)`（.cpp:112-360）逐步：

```
1  守卫: 无 risk_monitor/portfolio → 放行                        (:114)
2  已-halt 分支: closeout 窗口内禁自动恢复(:122);
   checkAndRecover 成功 → resumeFromRisk+unblock×2+arb 复活(:127-137);
   失败 → 保持 RISK_HALTED+限频日志(:141-153)
3  delta-rate 检查: breach → RISK_HALTED+撤全部做市单(:156-170)
   【恢复只能走第 8 步统一路径, 勿在此加 else】(:158-162 注释)
4  软响应前置: util≥L1/L2 → riskWiden().tickSoft(无状态逐tick)    (:172-184)
5  违规收集: checkRiskLimits(portfolio, _violations_buf) 零堆分配 (:187)
6  分级: determineActionWithCategory(violations,category,stale)   (:190)
7  动作 switch:
   HALT_TRADING(:195-233): haltTrading(category,pnl) → 撤 MM 单
     + Router 按 source 清扫 CLOSEOUT/HEDGING/ARBITRAGE/RISK_REDUCE
     + IRREVERSIBLE→forceFlatAll + arb disable
   BLOCK_SIDE(:241-278): blockLong/Short+RISK_HALTED(进统一恢复)+pause
   WIDEN_SPREAD(:280-290): WARNING 升级路径 onHardWiden
   (PAUSE_QUOTING/FLATTEN 死分支已删——数学不可达 :236/:295)
8  统一恢复(violations 空, 仅 qphase==RISK_HALTED 进入)(:305-340):
   区分 delta-rate-only 与 hard violation; delta 冷却且
   (非硬违规 || canRecover 通过=预算未耗尽) → resumeFromRisk
   +unblock×2+resumeQuoting+riskWiden.reset()+arb 复活
9  毒性冷却(:347-356): in_cooloff→TOXICITY 相位+校准器 decay;
   出冷却→tryResumeFrom(TOXICITY)(避免误翻其它相位)
10 return qphase != RISK_HALTED                                  (:359)
```

调用点：processTick Stage 4（StrategyCoordinator.cpp:570）与 closeout 窗口复跑（:505）。另含 checkTakerReduce（util=\|净头寸\|/maxPosition > takerReduceThreshold → taker 减仓单，Source::RISK_REDUCE，P2-3 后脱离 CLOSEOUT 口径）。

**与 D 批的关系**：ERROR（下单错误）不再进入本模块视野——halted 未设时已-halt 分支不触发、统一恢复只认 RISK_HALTED；两套暂停彻底解耦。

---

### 4.14 TradingState（分层状态机 HSM）— TradingState.h

两维正交状态 + 方向软禁（全字段 atomic，CAS 转移）：

```
MmPhase:      QUOTING ⇄ CLOSEOUT            (enterCloseout/exitToQuoting :131/:138)
QuotingPhase: NORMAL / TOXICITY / MARKET / ERROR / RISK_HALTED   (:62-69)

canQuote() = QUOTING && NORMAL                                (:102-106)
canBuy/Sell() = canQuote && !long/short_blocked               (:109-112)

转移规则:
  setQuotingPhase(q) — read-check-CAS 循环(:163-176):
    当前==RISK_HALTED 时仅允许目标 NORMAL(:152-157 canTransitionQuoting),
    其余子态互相可抢占(高优先级覆盖)
  tryResumeFrom(expected) — CAS "仅当当前==expected 才翻 NORMAL"(:188-194):
    非 H 子态的统一退出入口, 防高优先级期间被低优先级 else 分支误翻
    (例: HALT 期间 MARKET 的 shouldPause=false 分支不得把 H 翻 N)
  resumeFromRisk() — RISK_HALTED→NORMAL 唯一合法出口(:197-201)

D1 单轨语义(2026-08-24②): 下单错误风暴仅设 qphase=ERROR(指数退避自探恢复),
  不再叠加 haltTrading —— _trading_halted 属 FutuRiskMonitor, 仅真正风控 halt 设置。
  契约测试 test_error_single_track.cpp×3 固化。
```

线程契约（文件头 :14-33）：实盘 Md/Td 双线程读写 → v7.6 全原子+CAS；多字段复合操作（reset/exitToQuoting）逐字段 store 有 ns 级混合视图窗口，仅在 session 安静期调用。

---

### 4.15 FutuPortfolio（持仓/Delta 账本）— FutuPortfolio.h/.cpp

全方法 RecursiveSpinLock 保护（.h:339）；双写者分字段：markToMarket 价格域=Md、onTradeFill 记账域=Td。

| 接口 | 行号 | 说明 |
|---|---|---|
| ContractState::delta() = position × hedge_ratio | .h:106 | **delta 定义唯一权威** |
| exposure()/raw_exposure() | .h:111/:122 | 敞口（元） |
| isPositionLimitBreached | .h:131 | \|position\|>max_position（position 口径硬限判定输入） |
| getContractDeltaUtilization | .h:141 | delta+同向 pending×hr)/contract_max_delta（前瞻口径） |
| addContract/setContractMaxDelta(B2 热更) | .cpp:23/.h:274 | 锁内遍历置 ContractState |
| onTick/markToMarket/updateDailyPnL | .cpp:88/:110/:123 | 盯市；unrealized 由引擎 profit 权威更新（:119 注释） |
| onTradeFill/onPositionUpdate/updatePosition | .cpp:178/:148/:164 | 成交记账+引擎持仓回调 |
| resyncPosition/setShadowFromEngine/markShadowStale | .cpp:195/:214/:243 | 影子簿对账族（漂移检测→markShadowStale→broadcastCostBasisStale） |
| smoothUpdateHedgeRatio | .h:269 | correlation beta 平滑注入 |

---

### 4.16 FutuRuntimeOps（事件处理）— FutuRuntimeOps.cpp

#### processTradeFill（on_trade, :35-399）

1. 方向归一 `is_buy = isLong == isOpen`(:71)；recordTrade(:75)
2. SideFillBreaker 同侧熔断检查(:86-97, CLOSEOUT 豁免)
3. 分向成本簿记账+影子簿对账（漂移→markShadowStale/resyncPosition :104-148）
4. arb 桥成交处理（in_flight 递减+残腿对冲 :151）
5. quoter 定位 isMyOrder→onTrade+scout 撤义务(:157-168)
6. **回测/实盘分叉**(:175-186)：回测条件式撤单防 mocker 迭代器失效；实盘 requoteAfterFill
7. tracker recordOrderFill(:196)、perf recordTrade(:207)、自成交校准回灌(:253-281)
8. 日志 SPSC 卸载(:316)+info 每 50 笔采样(:321)
9. 成交成功重置 `_order_error_count`(:334-337)
10. **RISK_HALTED 恢复四道闸**(:342-394)：checkRiskLimits(复用 Td 专属缓冲)→硬违规扫描(POSITION_NET/EXPOSURE/DAILY_LOSS)→IRREVERSIBLE 拒恢复→closeout 进行中跳过→checkAndRecover 通过才 resumeFromRisk+unblock+复活 arb

#### onEntrust（:759-892）— D1 后形态

```
RISK_HALTED 期间直接忽略(H 不被下单错误覆盖)                    (:788-791)
成功: 错误计数清零→双边统计 ack→tryResumeFrom(ERROR)            (:793-813)
失败(策略层不做柜台错误分类——AGENTS §2 边界):
  计数++→死单清理 quoter.onOrder(canceled)(:833-842)
  →arb 拒单撤同 pair 另一腿+markLegRejected(A2 残腿防护)(:845-853)
  →finalizeOrder M1/M2 幂等清理(tracker markPendingCancel(REJECTED)
    +untrack+router.onOrderDone)(:1026-1044)
  →阈值(_config.orderErrorThreshold)分支:
    达到: qphase=ERROR+paused_since+cancelAll 全撤             (:859-881)
      【D1】不再 haltTrading —— 暂停源单一化:
        ① 恢复预算(max_recovery_count=3/session)不被下单错误消耗,
          留给真风控 halt(delta-rate/exposure/block_side)
        ② 消除 ERROR/_trading_halted 双轨重叠(V9 §三.2 立案)
      指数退避自探恢复(handleQuotingAutoResume 10s×2^n≤60s)
    未达: 仅 ERROR 相位+warn(软触发不 cancelAll 维持原语义)      (:882-891)
```

#### onChannelReady/lost（:401-600/:981-1024）

ready：channel_ready=true/price_stale=true→arb enable→**zombie 清扫锚点**（clearZombies 非空→stra_cancel_all("")全撤+广播清孤儿槽 :434-447）→逐合约持仓同步（本地净仓唯一权威，lastMid 作价）→非 IRREVERSIBLE 用**局部 violations 缓冲**跑 checkRiskLimits：无违规→完整恢复；POSITION_NET 超限→liquidator.reduceContract AUTO REDUCE(:543-575)。lost：RISK_HALTED→cancelAll→haltTrading(REVERSIBLE)→arb disable→持仓快照日志。

---

### 4.17 热参数体系（FutuHotParamManager/Watcher）

**27 参数稳态权威链**（实盘）：`hotparams.yaml > 共享内存残留 > config/coordinator`；
**回测不跑 watcher**——热参完全不生效，logDriftSummary 打印的差异键即"回测/实盘行为分叉点"。

#### 完整链路（V8-P0-1 加固后 + 2026-08-24 加固）

```
[实盘] watcher 线程 1Hz (UftFutuMmStrategy.cpp:405-407):
  parseHotParamFile(纯函数) ── strtod 全串校验拒收 "abc"/布尔/空串;
    NaN/inf/越界(bounds 表)拒收该键保留旧值 warn; 未知键忽略
  syncFromFile: 只写共享内存+值比对去重(每轮全量, 无 mtime 门控);
    变更数>0 → atomic _pending_apply
  【watcher 线程绝不 applyAll —— 消除裸写竞态】

[tick 线程] on_tick 内 drain (UftFutuMmStrategy.cpp:627-628):
  consumePendingApply()==true → on_params_updated()(锁内组装 Targets)
  → FutuHotParamManager::applyAll (在 _cb_mtx 内执行)

applyAll 应用顺序 (FutuHotParamManager.cpp:74-203):
  GLFTParams → quoter 报价四参数 → SignalAggregator 权重(updateWeights)
  → Coordinator alpha_sensitivity → Portfolio max_delta/contract_max_delta(B2)
  → sticky/protection/max_price_deviation → 末尾 crossCheckIssues

crossCheckIssues (纯函数, warn 级 [HOTPARAM-CHECK], 不阻断):
  ① 五路权重和偏离 1.0 超 ±0.1
  ② 全侧满挂深度(base_qty×衰减结构) < obligationMinQty(与 loader 同公式)
  ③ portfolio_max_delta > 任一合约 maxPosition(软限>硬顶)
  ③b contract_max_delta > maxPosition
  ④ min_spread_mult > max_spread_mult(clamp 区间倒置)
  ⑤ confidence_weight_min > max(插值反向)

logDriftSummary (on_init initial applyAll 后无条件调用, 回测也打印):
  对比 27 键 hotparams 解析值 vs 注册默认(config/coordinator 值)
  → 逐条 warn [HOTPARAM-DRIFT] 'x' config_default=A hotparams=B + 汇总行
```

**启动期语义**：initial applyAll 先把共享内存残留旧值盖到 config 初始化值上（"重启不丢上次热更结果"，UftFutuMmStrategy.cpp:383-395）；实盘 ~1s 内 watcher 首轮又以文件为准覆盖。27 键按名注册天然兼容旧共享内存布局。

边界表收紧原则（2026-08-24）：loader error 级→热路径拒收同界（base_spread{0.5,20}/base_qty{1e-6,100}/level_step{1e-6,100}/max_delta{1e-6,1e8}/phi{0.01,2}/delta_skew_threshold{0,0.9}）；sticky_threshold{0.01,10}（0=churn 防呆拒收）、protect_ticks{0.5,1e4}（<半 tick 无意义，关闭走 price_protection 开关）；warn 口径保持宽松交给交叉复查。

---

### 4.18 套利子系统（Arb 三件套）

```
SpreadArbitrageManager (SpreadArbitrageManagerInit.cpp 装配 ~270 行
                        + 主文件运行时 ~1089 行 S-4 文件级二分)
  ├ SpreadCalculatorManager: pair z-score/OLS beta(fresh-pairing A-4 去重)
  │   leg multipliers 接线(R4b A-1: yaml leg1Multiplier/leg2Multiplier
  │   → setLegMultipliers → computeSpread 含乘数)
  ├ 统计策略族: MeanReversion/TrendFollowing(current_price 打通 A2)/PairsTrading
  ├ B-3 门(Gatekeeper): minSignalConfidence(单键双层统一)/信号冷却/
  │   两族 in_flight(open/close 各自精确释放 A-3)/超时队列
  └ B-1 intent/B-5 过冲保险丝/B-6 聚合
AsyncArbitrageExecutor (异步执行器; 回测同步模式)
  ├ LegExecutionFSM(隐式): PENDING→LEG1_SENT→LEG2_SENT→HEDGING→DONE
  ├ 孤儿腿对冲: enqueueOrphanLeg(hedge_qty 含 ratio A-2; 失败保留重试
  │   500ms 节流×3 上限后 error 放弃); Source::HEDGING(A9)
  ├ 时钟: setReplayNowUs(A5 统一 replay 时钟, TIMEOUT/maxDivergence 可复现)
  └ EMERGENCY → handleRiskAlert → IRREVERSIBLE halt(A8 真保险丝)
ArbExecutionBridge (桥)
  ├ pushTick(tick, leg1_mult, leg2_mult) 3 参(A-1 后)
  ├ 回调受理状态制: rejected/rate_limited/self_trade_blocked/无价
  │   → false → executor 重试(A-2)
  ├ cancelByPair(A7: 原 cancelAllBySource 误撤其它 pair)
  ├ markLegRejected(pair_id, qty) 残腿防护标记(A2)
  └ closeout 冻结桥: isCloseoutFlattening 才冻结(A12: FAILED/COMPLETED 解冻)
```

配置见 CONFIG.md §6（spread_arbitrage.yaml）。回测默认 arb_close.enabled=false——该子系统回测覆盖有限，验证靠单测+实盘灰度。

---

### 4.19 Closeout 平仓族

| 组件 | 职责 |
|---|---|
| CloseoutTrigger | 触发判定：currentTime ≥ close_time - minutesBefore×60s（白盘/夜盘双时刻自动推导 Assembler:771-805）；processTick Stage 0 |
| CloseoutOrchestrator | 状态编排（依赖 RiskMonitor CloseoutSub 状态机）；策略 `_closeout_orch.onTick` 每 tick 驱动；夜盘收市恢复报价分支（Coordinator:554-560）|
| CloseoutExecutor | 渐进平仓执行：depthRatioPassive/Mid/Aggressive 三档比率+sweep 判定(sweepThresholdMs/sweepTicks)+FAK+drainTimeoutMs+maxRetries |

Fix4 裁决：CLOSEOUT 相位豁免 REVERSIBLE halt（保证收盘减仓不被锁死）；IRREVERSIBLE 在 closeout 窗口不强平。

---

### 4.20 性能与监控

- **PerformanceMonitor**：rdtsc 埋点（_tsc_tick0 on_tick 入口）；RingBuffer p99/p999 分位数；checkThresholds 1s 节拍分级告警（warn/critical ns 阈值）；ThroughputStats 全 atomic。
- **PerformanceAnalyzer**：绩效统计（spread_at_trade 兜底查配置 tick_size×2，R7）；时钟统一 replay `_exchange_time_ms`（R3-残余修复量纲）。
- **BilateralStats**：双边报价统计——周期行内存积压 `_bilateral_io_backlog`（MdSpi 专属免锁），section-break/closeout TRIGGERED 定点排空落盘（R6-b 移出热路径）；当日文件 seed 重启续算（框架 session_end 生产不触发，见 §8）。
- **MonitorBridge**：WtMonSvr GUI 数据桥（框架层 initEvtNotifier 补丁依赖，§8）。

---

## 5. 关键设计裁决与语义边界

| # | 裁决 | 理由/出处 |
|---|---|---|
| 1 | delta(策略) / position(风控) 语义边界 | 2026-08-19 用户明确；skew 用 realized（C2），义务/qty 衰减用 projected——"义务需要提前性，skew 不需要" |
| 2 | skew_price 随 spread_mult 放大不得解耦 | 毒性高时保护性+进攻性协同出清库存（SpreadOptimizer.cpp:180-183 用户确认） |
| 3 | B+ 永不 force-untrack 活单 | 僵尸单事故根因；撤单确认才清槽+重试+zombie |
| 4 | ERROR 单轨化（D 批） | 下单错误不消耗风控恢复预算；消除双轨重叠 |
| 5 | closeout/taker/liquidator 不计入 ORDER_RATE 频控 | 紧急路径不应被频控 HALT 卡死 |
| 6 | 回测补挂时序与实盘不同属有意 | 可复现性优先；mocker 迭代器失效死循环教训（2026-08-21） |
| 7 | 策略层不解析柜台错误码/文本 | 50/51/31 分类属框架层职责（AGENTS §2）；orderErrorThreshold=1000 为临时运维值待回收 |
| 8 | stra_cancel_all 必须传 fullCode 或空串 | 实盘 TraderAdapter::cancelAll 用 getFullCode 匹配，stdCode 永不匹配静默全不撤（§8） |
| 9 | QuotePolicyChain 固定顺序即优先级 | RiskWiden tickSoft 与 onHardWiden 同 tick 覆盖关系由链序保证 |
| 10 | 热参交叉复查 warn 不阻断 | 避免热调参被误卡死 |

## 6. 构建 / 回测 / 部署

```bash
# Linux 全量/单模块
cd src/build_all && cmake .. && make -j$(nproc)          # 或 build_release.sh
cd src/build_all && make -j$(nproc) WtFutuCore            # 单模块
# 注意: 新增测试文件后需重跑 cmake . (GLOB configure 期求值)

# Windows
src/all.sln (或 uft.sln); 需 MyDepends141 环境变量

# 部署到回测 dist
cp src/build_all/build_x64/Debug/bin/WtUftRunner/futu/libWtFutuCore.so dist/WtBtFutu/uft/
cp src/WtFutuCore/config/{coordinator,hotparams}.yaml dist/WtBtFutu/

# 回测冒烟
cd dist/WtBtFutu
LD_LIBRARY_PATH=./uft:$LD_LIBRARY_PATH timeout 900 ./uft/WtBtRunner \
    -c configbt_v5.yaml -l logcfgbt.yaml < /dev/null
# 全量基线: -c _ec_5d.yaml timeout 1200
```

**生产部署**：仅 Release（Debug assert 风险）→ 远程 ubuntu@129.211.5.54 `dist/uft/`，先备份 `.bak_YYYYMMDD`；cron 值守 08:40/20:40 重启——kill 后轮询等退出、确认单实例再启动。

## 7. 测试体系

- GoogleTest 于 `src/TestUnits`；构建目标 TestUnits 后运行二进制。
- 当前基线 **140 过 / 2 个既有环境性失败**（test_session.test_allday、test_shm.test_sharehelper）。
- 关键测试文件与守护点：

| 文件 | 守护点 |
|---|---|
| test_hot_param_manager.cpp | parse 校验/边界拒收/pending apply |
| test_hot_param_hardening.cpp | 边界收紧表驱动+crossCheck 五项+drift 计数（16 用例） |
| test_toxicity_direction.cpp | 方向分类/停边映射/warmup 门（10 用例） |
| test_v8_r1_p0 / r3_coverage / r5_fixes / v9_r6a | 各轮修复回归 |
| test_order_slot_bplus.cpp | B+ 槽状态机/zombie 升级重武装（9 用例） |
| test_inventory_delta_separation.cpp | delta/position 口径分离（8 用例） |
| test_skew_dimensionality.cpp | C1/C2 量纲与口径（6 用例） |
| test_error_single_track.cpp | D1 单轨契约（ERROR 可恢复/HALT 守卫/closeout 正交，3 用例） |

- 回测判据（§8 勘误后）：统计带（笔数 ±1.5%）+健康度（0 HALT/zombie/error、session end Delta=0、资金收敛）。**不存在逐比特判据**。

## 8. 已知限制与勘误

1. **回测随机性（含 UFT 路径）**：`HftMocker.cpp splitVolume()` 每次 `srand(time(NULL))`；`UftMocker.cpp:34` extern 复用 → 同配置两次运行成交拆分序列不同。CSV md5 对比一律无效。
2. **日志路由**：策略类 debug 日志（[TOXIC]/[SIGNAL_DECOMP]/[HOTPARAM-*]）落 **outputs/Runner.log**；outputs/Strategy_uft.log 只含引擎/订单级。
3. **stra_cancel_all 口径不一致**（禁止越界修复）：实盘 fullCode 两段式 vs stdCode 三段式——策略侧兜底必须传 fullCode/空串。
4. **柜台账户资源类拒单无分类通道**：CTP 50/51/31 在多策略共享账户下是常态，框架 on_entrust 无错误类型字段→策略只能通用计数。正确修复点在 TraderCTP/TraderAdapter。
5. **WtUftEngine on_session_end 生产不触发**（WtUftTicker.cpp:183）：双边统计已绕开（定点 flush+seed 续算）。
6. **实盘 on_entrust 仅失败触发**（TraderAdapter.cpp:1335）：成功回报不存在，依赖该事件的逻辑在实盘恒空。
7. **框架已打补丁（越界记录）**：UftStraContext DATA_SIZE_STEP 8000→200000；WtUftRunner 补 initEvtNotifier() 调用；libWtMsgQue.so 需置于 runner 工作目录。
8. **TOXIC 计数簿记漂移**：R5 簿记 2,770 → 现 983（preBC 对照二进制证明与本修复包无关，源头 R6 系，最可疑 P2-4 改变 trade_flow 输入；留独立归因）。
9. **去大锁收官清单**（WS-C/none 切换验收）：TSAN 全量→_ec_5d 逐比特 A/B(big vs none)→refreshQuotes 持锁埋点→灰度 p99/p999→删 _cb_mtx（单独 commit）。

---

*文档生成：2026-08-24③。行号锚点基于当时 HEAD（d963a44e 之后工作树）；后续提交可能漂移，
以方法名检索为准。配置字段逐键说明见 CONFIG.md。*
