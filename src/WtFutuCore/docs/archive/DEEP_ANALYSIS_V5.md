# WtFutuCore 深度分析与修复报告 v5.0

> 生成日期: 2026-07-18
> 方法: 5 组并行逐行源码审查（入口流水线/下单持仓/信号毒性/套利/风控基础设施）+ 主线程复核验证
> 前置报告: OPTIMIZATION_REPORT.md (v4.0, 已修 19 项)

## 一、项目现状总览

规模：46 .h + 31 .cpp，约 2.8 万行。架构骨架优秀，但存在系统性"半完工"状态：

| 宣称能力 | 实际状态 |
|---|---|
| 异步套利线程(~50ns push) | `start()` 被注释(UftFutuMmStrategy.cpp:1217)，套利主线程同步执行 |
| OrderRouter <500ns/order | 含 string 构造+vector 堆分配+fmt 日志，不成立 |
| 5 级风控递进 | WIDEN_SPREAD/REDUCE_SIZE/FLATTEN_POSITION 无 switch 分支，缺挡 |
| REVERSIBLE halt 自动恢复 | `checkAndRecover` 无调用者（死代码），halt 后基本卡死 |
| 热更新 22 参数 | baseQty/qtyDecay/levelStep/baseSpread 不穿透 FutuQuoter；信号权重不穿透权重框架 |
| PerformanceMonitor 5 类延迟 | 只有 recordTickToQuote 被接线，且 monitor 未 set 进 Coordinator |
| SyntheticSignalFusion 3 源融合 | 无 feed 调用点，反用空数据覆盖 Predictive 通道 |
| MarketMakingEnhancer | 全链路无调用者 |
| 套利仓位跟踪/止损/退出 | `SpreadArbitrageManager::updatePosition` 无调用者，退出/止损全为死代码 |

## 二、架构评估

### 优点
1. 双路径下单（MM 直调 ctx / 非 MM 走 Router）延迟分层正确
2. TradingState HSM（MmPhase + QuotingPhase + 方向软禁）+ 线程契约
3. StrategyCoordinator 8-Stage TickContext 流水线
4. ISignalSource 插件化 + 三层权重框架方向正确
5. LockFreeQueue SPSC 主路径 acquire/release 配对正确

### 缺陷
1. **上帝类**: UftFutuMmStrategy.cpp 2843 行，混合配置解析/DI/热参数/套利回调/closeout 编排/风控恢复
2. **状态多处冗余**: _last_mid/_toxicity_resume_time/portfolio_max_delta 多处各存一份，是半数 Bug 温床
3. **配置双向覆盖**: config.yaml → coordinator.yaml → FutuMmConfig 回写，生效来源不可静态推断；FutuConfig 工具类无人使用
4. **新增套利策略需改 7 处**（无公共基类）；**新增信号源需改 ~8 处**（SignalType 与 WeightedSignalType 双枚举必然漂移）
5. **时间表示全库混用 4 种**（epoch-ms / µs epoch / date*86400000 / HHMMSSmmm 打包整数）——P0 Bug 总根源

## 三、确认 Bug 清单（复核验证过）

### P0（已修复，见第四节）

| # | 问题 | 位置 |
|---|---|---|
| 1 | closeout 时间戳 3 种单位混用 → 重试间隔失效，FAILED 后每 tick 重试烧完重试次数 | UftFutuMmStrategy.cpp:1996, StrategyCoordinator.cpp:342-410, FutuRiskMonitor.cpp:880 |
| 2 | REVERSIBLE halt 无恢复路径（checkAndRecover 死代码） | StrategyCoordinator.cpp:671-684, FutuRiskMonitor.cpp:529 |
| 3 | "日亏损"跨日累计：resetDaily 不重置 FutuPortfolio::realized_pnl | FutuRiskMonitor.cpp:581, FutuPortfolio.h:91 |
| 4 | OrderRouter::getActiveOrders 返回局部变量引用（悬垂 UB） | OrderRouter.cpp:263-277 |
| 5 | 部分成交即 untrack → 自成交检查绕过+在途量低估+sticky 失效+成交双计数 | UftFutuMmStrategy.cpp:1818-1819 |
| 6 | stra_quote 单侧成功时成功侧成孤儿单（未 track 无人撤） | FutuQuoter.cpp:170-185 |
| 7 | RollingScaleTracker 节流失效：_cache_dirty 恒 true → 每 tick 4×(500元素拷贝+sort) | ICWeightTracker.h:207-235 |

### P1（已修复部分）

| # | 问题 | 位置 |
|---|---|---|
| 8 | PerformanceMonitor 未接线进 Coordinator | UftFutuMmStrategy.cpp:849 |
| 9 | 热更新 price_protection 硬编码 true | UftFutuMmStrategy.cpp:2787 |
| 10 | param_update_interval 配 0 → 取模除零 SIGFPE | StrategyCoordinator.cpp:1291 |
| 11 | _closeout_pending_ids 从不 insert → on_order closeout 跟踪死代码 | UftFutuMmStrategy.cpp:2030 |
| 12 | 魔法数 static_cast<Source>(1)；HALT 不撤 ARBITRAGE 单 | StrategyCoordinator.cpp:716-719 |
| 13 | maxDailyLoss 符号陷阱（配正数时风控反转） | UftFutuMmStrategy.cpp:446 |
| 14 | _toxicity->analyze() 每 tick 算两遍 | StrategyCoordinator.cpp:641,937 |

### P1/P2（待修复，按优先级排序）

| # | 问题 | 位置 |
|---|---|---|
| 15 | 套利仓位不回填（updatePosition 无调用者）→ 策略退出/止损/风控限仓全失效 | SpreadArbitrageManager.cpp:589 |
| 16 | B-3 门提前置 in_flight，下游丢弃不回收 → pair 卡死 60s；confidence=0.5 恰好被 executor 过滤 | SpreadArbitrageManager.cpp:879, AsyncArbitrageExecutor.cpp:287 |
| 17 | 残腿无防线：leg1 成交 leg2 被拒无跟踪无对冲；同价去重按单腿跳过制造腿不平衡；cancelAllBySource 误伤其他 pair | UftFutuMmStrategy.cpp:2346-2420 |
| 18 | IC 未来回报口径错误（1-tick 收益冒充 horizon 收益）；Regime trend 检测 stub（short_ma==long_ma）→ 动量权重恒×0.5 | SignalAggregator.h:361-365,376 |
| 19 | PredictiveToxicity warmup 提前 return 吞掉 alpha 通道；extreme 信号双重叠加 | PredictiveToxicity.cpp:151-156, ToxicFlowDetector.cpp:157-162 |
| 20 | Fusion 死引擎每 tick 用 alpha=0 覆盖 _predictive._latest_alpha | ToxicFlowDetector.cpp:236-250 |
| 21 | 平仓后 unrealized_pnl 不清零 → daily_pnl 双重计数 | FutuPortfolio.cpp:98-101 |
| 22 | 反手成交（多5→空3）closed_qty 少算、均价错误 | UftFutuMmStrategy.cpp:1750-1772 |
| 23 | 风控滑窗只在新事件到达时推进（stale window）；widen→pause→flatten 递进死代码 | FutuRiskMonitor.cpp:31-38,358-380 |
| 24 | 撤单限速时钟混用（steady vs epoch）→ max_cancel_rate 失效；最优价缓存幽灵值 | UnifiedOrderTracker.cpp:93-95,137-171 |
| 25 | FutuQuoter 定价从不使用 spread_mult/base_spread → updateQuotingParams 失效 | FutuQuoter.cpp:54-56 |
| 26 | 套利时间域混乱致持仓"立即超时"；SpreadRiskManager 净敞口符号错误；Z-score 扩展窗口非滚动；WEIGHTED 价差丢乘数 | SpreadArbitrageManager.cpp:657, SpreadRiskManager.cpp:97, SpreadCalculator.cpp:149-175 |
| 27 | 脏 tick 先污染 Portfolio/Correlation 后校验；arb pushTick 成交量硬编码 1.0 | UftFutuMmStrategy.cpp:1354-1419,2326 |
| 28 | CloseoutExecutor：PASSIVE 档价格方向反、MID_PASSIVE 不 tick 对齐、updateRoundFill 死代码、FAILED 无 RETRYING 出口 | CloseoutExecutor.cpp:406-435 |
| 29 | PAUSE_QUOTING 不撤存量做市单；风控恢复误翻 MARKET 暂停态；tryResumeFrom 可绕过 RISK_HALTED | StrategyCoordinator.cpp:748-785, TradingState.h:169-174 |
| 30 | 信号权重热更新不穿透 AdaptiveWeightFramework；cap 在归一化前施加可突破 0.50；IR=0 新信号免费升权 1.15 | SignalAggregator.h:116-123, ICWeightTracker.h:384-410 |
| 31 | SelfTradeCalibrator 时间基准混乱 → FillRetreat/prune/decay 全失效 | SelfTradeCalibrator.cpp:210,315,447 |
| 32 | 自成交调价取"第一个"而非最优对手单 | AsyncArbitrageExecutor.cpp:334-394 |
| 33 | TradeFlow onTransaction 通道无滑窗衰减（IC=-0.83 同根因） | MarketDataContext.cpp:224-254 |
| 34 | STOP_LOSS/TIMEOUT 无方向信息，空头价差止损=加仓；StatArb 止损不可达 | AsyncArbitrageExecutor.cpp:298-300, StatisticalArbStrategy.cpp:287 |
| 35 | haltTrading 重置 _recovery_count → 恢复上限失效；max_loss/max_exposure 无 >0 guard | FutuRiskMonitor.cpp:394,141,163 |

### 死代码清单
`checkToxicityAndCircuitBreak`（声明未实现）、`trackArbOrder`、`updateAdaptiveParams`（空占位）、`combineSignals`/`StrategyWeights`/`testCointegration`/`pushOverwrite`/`BlockingQueue`、`_current_tick_timestamp/_current_tick_mid`、`_blocked_contracts`（只 clear）、coordinator `_channel_ready`（从不更新）、`CorrelationManager::getSpreadSignals/hasSpreadOpportunity`（存根）、`computeBidPrice/computeAskPrice`（FutuQuoter.h:247-264）、RealizedToxicity 伪造指标字段、PerformanceAnalyzer `_start_time`（trading_time_sec 恒 0）。

## 四、性能优化清单

### 已实施
1. RollingScaleTracker 节流修复 + nth_element 替代全排序（P0-7）

### 待实施（按收益排序）
| # | 优化 | 位置 | 收益 |
|---|---|---|---|
| 1 | 恢复套利异步线程（先修跨线程读写）或彻底删除异步机制 | UftFutuMmStrategy.cpp:1217 | 移出做市关键路径 |
| 2 | 每 tick 时钟只取一次注入 TickContext（当前 ≥6 次） | UftFutuMmStrategy.cpp:1560, StrategyCoordinator.cpp:222-289 | ~100-200ns/tick |
| 3 | processTick 零堆分配：checkRiskLimits/checkAutoCancel 按值返回 vector 改 out-param；computeWeights 改 std::array；PortfolioContext 拷贝含 vector | StrategyCoordinator.cpp:694,871,1061 | 消除 1500+ malloc/s |
| 4 | 每 tick ~10 次 unordered_map<string> 同 code 重复 find → 入口解析一次 | StrategyCoordinator.cpp:565-1005 | 微秒级累计 |
| 5 | 热路径 fmt 日志降频：[TOXIC]/[LIMIT]/风控 warn/OrderRouter 拒单 | StrategyCoordinator.cpp:644,984 等 | µs 级/次 |
| 6 | Momentum 每 tick 128 次 std::log → log-return 环形缓冲 O(1) | MomentumSignalSource.h:147-154 | O(n)→O(1) |
| 7 | Portfolio 每 tick 双更新（markToMarket+onTick）合并 | UftFutuMmStrategy.cpp:1358 | 减半 |
| 8 | stra_buy/sell/quote 返回 vector 值语义 → 每单 1 malloc | OrderRouter.cpp:69, FutuQuoter.cpp:170 | 每单省 1 malloc |
| 9 | LockFreeQueue _drop_count 与 buffer 共享 cache line；ArbTickData 含 std::string 改 char[16] | LockFreeQueue.hpp:253 | false sharing |
| 10 | updateMMOrders 每 tick 全量深拷贝 MM 快照 → 增量/世代号 | AsyncArbitrageExecutor.cpp:143-150 | 每 tick 多次堆分配 |

### 编译选项（CMakeLists.txt）
- **去掉 -ffast-math**：隐含 -ffinite-math-only，NaN 时风控比较恒 false 静默失效 → 改 `-fno-math-errno -fno-trapping-math -ffp-contract=fast`
- 恢复 `-march=x86-64-v2 -mtune=native`（不改 ABI）与库内 LTO（不改导出符号 ABI）
- file(GLOB) 加 CONFIGURE_DEPENDS；升 cmake_minimum；加 -fvisibility=hidden -Wall -Wextra

## 五、业务逻辑模块功能地图

- **UftFutuMmStrategy**: 策略入口，回调分发+模块装配+热参数表（表驱动已预留扩展）
- **StrategyCoordinator**: 8-Stage 流水线（preCheck→行情→信号→风控→报价→撤单→对冲→自适应）
- **TradingState**: 统一交易状态 HSM，resumeFromRisk() 为 RISK_HALTED 唯一合法出口
- **FutuQuoter**: 单合约多档双边报价（bilateral sticky / obligation 全撤全报 / flexible）
- **OrderRouter**: 非 MM 统一下单（Source 分桶限速+STP；优先级只有分类未实现调度）
- **UnifiedOrderTracker**: 订单 SSOT（仅 MM 单；trackArbOrder 死 API → router 单互无 STP）
- **FutuPortfolio**: Delta/敞口/对冲，合约数组+code 索引 O(1)
- **SignalAggregator/ICWeightTracker**: 6 源聚合，三层权重×p95 幅度归一化
- **毒性栈**: ToxicFlowDetector 门面 = Predictive(VPIN+OFI) + Realized + Fusion（死引擎）
- **SpreadArbitrageManager**: B-3 门（Portfolio 衍生仓位去重+限仓+in_flight 防重发）
- **AsyncArbitrageExecutor**: SPSC tick 队列+订单队列+orphan leg 对冲（当前同步运行）
- **CloseoutExecutor**: DRAINING→ASSESSING→EXECUTING，urgency 5 档价格升级+FAK 批量
- **FutuRiskMonitor**: 只读 Portfolio 的风控器（频率/敞口/日亏/closeout 状态机）

## 六、行动路线图

## 七、本轮修复记录 (2026-07-18, 编译通过)

| # | 修复 | 文件 |
|---|---|---|
| F1 | getActiveOrders 悬垂引用 → 按值返回（含头文件声明） | OrderRouter.h:217, OrderRouter.cpp:263 |
| F2 | closeout 时间戳统一为 epoch-ms（5 处：on_order 的 now_ms 垃圾计算、session_end/executor 完成失败的压缩时间戳、coordinator 的 time_hms*100 ×4） | UftFutuMmStrategy.cpp:1981-1998,1520,1529,1310,1631; StrategyCoordinator.cpp:342-410 |
| F3 | halt 恢复：checkRisk 在 isTradingHalted 时调用 checkAndRecover（自带节流+cooldown+IRREVERSIBLE 拒绝），恢复成功后同步 TradingState/方向解禁/套利执行器 | StrategyCoordinator.cpp:672-700 |
| F4 | 日亏损跨日：新增 FutuPortfolio::resetDailyPnl() 并在 on_session_begin 调用；平仓后 unrealized_pnl 清零（顺带修 Bug21） | FutuPortfolio.h:252, FutuPortfolio.cpp:98,131; UftFutuMmStrategy.cpp:1183 |
| F5 | 部分成交不再 untrack：UnifiedOrderInfo 增加 filled_qty + recordOrderFill()，仅完全成交才 untrack；移除 FutuQuoter::onTrade 的 recordFilled() 双重计数 | UnifiedOrderTracker.h:113,489; UftFutuMmStrategy.cpp:1817 |
| F6 | stra_quote 单侧成功：成功侧照常登记 level/映射/tracker，warn 日志提示 | FutuQuoter.cpp:170-200 |
| F7 | RollingScaleTracker：按 update_interval 真正节流 + nth_element 替代全排序（每 tick 省 4 次 500 元素 sort） | ICWeightTracker.h:216-241 |
| F8 | PerformanceMonitor setPerformanceMonitor 接线进 Coordinator | UftFutuMmStrategy.cpp:849-856 |
| F9 | 热更新 price_protection 不再硬编码 true | UftFutuMmStrategy.cpp:2790 |
| F10 | param_update_interval 配 0 钳位为 1（防取模 SIGFPE） | StrategyCoordinator.cpp:152 |
| F11 | closeout 订单识别改用 OrderRouter::isOrderFromSource(CLOSEOUT)（在 onOrderDone 抹除前判定），_closeout_pending_ids 死代码兜底保留 | OrderRouter.h:212, UftFutuMmStrategy.cpp:2014-2025 |
| F12 | HALT_TRADING 撤单补 Source::ARBITRAGE，魔法数 static_cast<Source>(1) → Source::HEDGING | StrategyCoordinator.cpp:733-737 |
| F13 | max_loss 符号陷阱：std::abs(max_daily_loss) 兼容正负两种配置约定 | UftFutuMmStrategy.cpp:448 |
| F14 | analyze() 双次调用：复核确认 ToxicFlowDetector::analyze 有 _cache_dirty 惰性缓存，第二次调用为 no-op，**无需修改**（报告此项降级） | ToxicFlowDetector.cpp:127-172 |

**编译验证**: `make WtFutuCore` Debug 构建通过，无新增警告（仅 TimeUtils.hpp 的 ftime 弃用警告为框架预存问题）。

## 八、第二轮修复记录 (2026-07-18, 编译通过)

| # | 修复 | 文件 |
|---|---|---|
| F16 | 反手成交 PnL/均价：重写为四情形（同向减仓/同向加仓/方向翻转/全平），翻转时 closed_qty=|old_pos|、新均价=成交价 | UftFutuMmStrategy.cpp:1750-1790 |
| F17 | haltTrading 不再重置 _recovery_count（恢复上限生效），改由 resetDaily 重置；max_exposure/max_loss 加 >0 防护（配 0=禁用） | FutuRiskMonitor.cpp:394,608,141,163 |
| F18 | 撤单限速时钟统一 epoch ms（原 steady_clock vs epoch 混用 → max_cancel_rate 失效）；最优价幽灵值修复（erase 空条目时同步清 best 缓存，不再 operator[] 重插） | UnifiedOrderTracker.cpp:93-99,128-181 |
| F19 | 毒性 warmup 期 alpha 通道恢复生效（VPIN 门控但 OFI/Trade/extreme 保留）；extreme_signal 双重叠加修复（移除 Predictive 内部叠加，门面统一叠加） | PredictiveToxicity.cpp:147-194 |
| F20 | Fusion 死引擎不再用空 alpha 覆盖 Predictive 通道（hasAnySource() 门控 runFusionCycle） | SyntheticSignalFusion.h:191, ToxicFlowDetector.cpp:236 |
| F21 | IC 未来回报口径修正为 mid[t]-mid[t-horizon]（维护 horizon+1 长度 mid 历史）；Regime trend 检测接入真实短/长 MA（O(1) 滚动和，20/60 tick），动量权重不再被恒 ×0.5 | SignalAggregator.h:361-400,554-568 |
| F22 | SelfTradeCalibrator recordFill 时间戳统一 epoch ms（FillRetreat/prune/decay 复活） | UftFutuMmStrategy.cpp:1874-1880 |
| F23 | 套利仓位回填：新增 refreshPositionsFromPortfolio() 每 tick 从 Portfolio(SSOT) 回填 spread_position，策略退出/止损与风控限仓复活；updatePosition 加同向腿防护 | SpreadArbitrageManager.h:194, SpreadArbitrageManager.cpp:589+, UftFutuMmStrategy.cpp:2350 |
| F24 | in_flight 生命周期：新增 onArbSignalDropped()，执行器四处丢弃路径（confidence 过滤/调价超限/队列满/push 失败）均释放 in_flight；confidence 过滤 >0.5 改 >=0.5（MR 阈值入场恰为 0.5） | SpreadArbitrageManager.cpp:925, AsyncArbitrageExecutor.cpp:284-295,431,468,478 |
| F25 | 套利时间域统一：positionDuration 修正为 µs→秒（原 /1000 且会下溢）；signal 冷却 µs/ms 混用修复（冷却从 1ms 恢复为 1000ms）；position_open_time 改用 µs epoch（原 actiontime 打包整数）；checkConvergenceFailure 不再 positionDuration(0) | SpreadArbitrageTypes.h:220, SpreadArbitrageManager.cpp:409,657, SpreadRiskManager.cpp:271-279 |
| F26 | 风控频率滑窗读侧剔除（新增 pruneRateWindows，checkRiskLimits/checkRateLimits 调用），停止报单后 RATE 误报不再持续 | FutuRiskMonitor.cpp:72-92,184,270 |
| F27 | CMake 去掉 -ffast-math（NaN 时风控比较静默失效），改 -fno-math-errno -fno-trapping-math -ffp-contract=fast | CMakeLists.txt:45-51 |

**编译验证**: `make WtFutuCore` 通过，无新增警告。

## 九、第三轮修复记录 (2026-07-18, 编译通过)

| # | 修复 | 文件 |
|---|---|---|
| F28 | CloseoutExecutor PASSIVE 档价格方向反转（买=bid+tick/卖=ask-tick，旧代码宽价差时变贴对手激进价）；MID_PASSIVE 加 tick 对齐（半 tick 非法价拒单）；updateRoundFill 接线（批次结算时记录成交，_total_filled/estimateFillRate 复活） | CloseoutExecutor.cpp:404-413,222-226 |
| F29 | PAUSE_QUOTING 补撤存量做市单（旧代码持仓超限仍留旧报价在场）；风控自动恢复仅限 RISK_HALTED（旧 !isActive() 会把 MARKET 暂停误翻 NORMAL 造成状态闪烁） | StrategyCoordinator.cpp:753-766,798-800 |
| F30 | 信号权重热更新穿透：AdaptiveWeightFramework::updateBaseWeights + updateWeights 同步（旧代码热权重对 alpha 计算完全无效） | ICWeightTracker.h:431-441, SignalAggregator.h:117-131 |
| F31 | 残腿防护：单腿被拒（流控/STP）时标记 pair，对侧腿在途成交即在 on_trade 反向平仓（净仓 API+FAK+CLOSEOUT 源）；session begin 清空标记 | UftFutuMmStrategy.cpp:1794-1813,2427-2455, UftFutuMmStrategy.h:375 |
| F32 | FutuQuoter spread_mult 语义澄清：spread 由上游 SpreadOptimizer 计入 l0 价，Quoter 不得重复应用（(void) 标注+注释，防误用导致双重计宽） | FutuQuoter.cpp:50-57 |
| F33 | TradeFlow onTransaction 通道接入滑窗衰减（与 onTickInference 同机制，修复整个 session 单调漂移，IC=-0.83 同根因） | MarketDataContext.cpp:224-280 |
| F34 | [TOXIC] debug 日志降采样 50 tick/次（热路径 fmt 开销） | StrategyCoordinator.cpp:646 |

**编译验证**: `make WtFutuCore` 通过，无新增警告。

## 十、第四轮修复记录 (2026-07-18, 编译通过) — 异步套利恢复 + 热更新穿透 + 性能

| # | 修复 | 文件 |
|---|---|---|
| F35 | **异步套利线程恢复**：跨线程隐患全部修复后启用 — ① computeDerivedSpread 改读 _pair_states(spin 保护，不再跨线程直读 FutuPortfolio)；② generateSignal 的 canOpenPosition 加 spin 保护；③ getQuotingAdjustment 删除锁外 find；④ _min_profit_threshold 原子化(atomic<double>)；⑤ 新增配置项 useAsyncArbThread（默认 true 实盘异步，回测配 false 走同步） | SpreadArbitrageManager.cpp:709-726,424-435,527-537; AsyncArbitrageExecutor.h:223,342; UftFutuMmStrategy.cpp:275,1215-1232 |
| F36 | 热更新穿透 FutuQuoter：baseQty/qtyDecay/levelStep/baseSpread 经 updateQuotingParams 下发，且重算 _level_qtys 预计算表（旧代码只改 _cfg 不刷表）；maxDelta 同步 coordinator setPortfolioMaxDelta | FutuQuoter.h:118-132, UftFutuMmStrategy.cpp:2799-2813,2830-2836 |
| F37 | 风控检查零堆分配：checkRiskLimits 增加 out-param 重载，coordinator/策略各持复用缓冲（消除每 tick 每合约 1 次 malloc） | FutuRiskMonitor.h:330, FutuRiskMonitor.cpp:101-115, StrategyCoordinator.cpp:716-718, UftFutuMmStrategy.cpp:1968,2205 |
| F38 | 每 tick 墙钟单次读取：on_tick 取一次 now_ms，经 handleCoordinatorTick 注入 processTick（可选参数，默认内部自取保持兼容） | UftFutuMmStrategy.cpp:1568-1571,1444-1458; StrategyCoordinator.cpp:219-237 |

**编译验证**: `make WtFutuCore` 通过，无新增警告。

**异步模式说明**: 启动日志会显示 `AsyncArbExecutor: async mode (arb thread started)`；回测部署时在 config.yaml 的 modules 节点加 `useAsyncArbThread: false` 恢复同步执行。arb 线程模型：主线程 pushTick(~50ns SPSC) → arb 线程 processTick+generateSignals → 订单请求 SPSC → 主线程 processPendingOrders 经 OrderRouter 下单，下单始终在主线程（ctx 无线程安全问题）。

**遗留（后续轮次）**: Bug34 STOP_LOSS 方向（被 B-3 门抑制，潜伏）、性能项（同 code map find 合并、updateMMOrders 世代号增量、stra_* 返回 vector 出参化(需改框架接口)、LockFreeQueue false sharing、Momentum log 滚动和）、架构项（拆上帝类/ISpreadStrategy 注册表/信号注册表）。

## 十一、第五轮修复记录 (2026-07-18, 编译通过) — 热路径微优化

| # | 修复 | 文件 |
|---|---|---|
| F39 | Momentum 信号 O(1) 化：增量对数收益环形缓冲 + 滚动和（旧代码每 tick 对 ≤127 个价格重算 std::log，相同相邻对反复计算） | MomentumSignalSource.h:65-95,127-150 |
| F40 | LockFreeQueue false sharing：buffer 64B 对齐（旧代码 buffer 首元素与 _drop_count 共享 cache line） | LockFreeQueue.hpp:249-258 |
| F41 | updateMMOrders 世代号门控：UnifiedOrderTracker 增加 _generation（track/untrack 递增），快照仅在订单集变化时深拷贝（旧代码每 tick 每合约 2 个 vector 深拷贝 + spinlock） | UnifiedOrderTracker.h:396,547, UftFutuMmStrategy.cpp:2360-2373 |
| F42 | TickContext 组件指针预解析：aggregator/book/quoter/spread_opt 在 processTick 入口一次性解析，updateSignals/processQuoting 复用（消除每 tick ~7 次重复字符串哈希查找） | StrategyCoordinator.h:64-69, StrategyCoordinator.cpp:245-263,566-585,878-950 |
| F43 | STOP_LOSS/TIMEOUT 方向按持仓推导（潜伏修复：旧代码硬编码"平多价差"，空头价差止损=加仓） | AsyncArbitrageExecutor.cpp:295-318 |

**编译验证**: `make WtFutuCore` 通过，无新增警告。

**遗留（架构级）**: 拆上帝类（ConfigLoader/ModuleAssembler/HotParamManager/CloseoutOrchestrator）、ISpreadStrategy 公共基类+注册表（新增策略从 7 处改动降为 1 处注册）、信号源注册表（消除 SignalType/WeightedSignalType 双枚举）、stra_* 返回 vector 出参化（需改 WtUftCore 框架接口）、TradingState::tryResumeFrom 加 RISK_HALTED 断言防护。

## 十二、架构重构记录 (2026-07-18, 三方案全部完成, 每步编译验证通过)

### 方案 A: ISpreadStrategy 公共基类 + 注册表 ✅
- 新增 `ISpreadStrategy.h`: 插件接口(generateSignal/update/configure/reset/typeName) + `SpreadStrategyRegistry` 工厂注册表
- 4 策略继承适配: configure() 各自从 SpreadPairConfig 提取参数(替代 Manager 类型分支); update() 统一 tick 入口
- `StrategyInstance` 4 个类型成员 → `std::vector<std::unique_ptr<ISpreadStrategy>> strategies` (hybrid 模式顺带复活)
- Manager 三条 else-if 链(onTick update/generateSignal/reset)全部改为插件循环; 内置策略静态注册(BuiltinStrategyRegistrar)
- **新增套利策略: 7 处改动 → 1 行注册**

### 方案 B: 信号源注册表 ✅
- `SignalAggregator` 引入 `SignalSlot` 表(type/wtype/缓存源指针/权重成员指针/归一化标志/extract/set_component)
- `computeAlpha` 提取段 + 加权段从 5 段硬编码改为槽位循环; IC recordSignal 同样槽位驱动
- 行为等价: 归一化规则(LL 除外)、fallback 权重、IC 记录值与重构前逐一对应
- **新增信号源: ~8 处改动 → initializeSignalSources 中一段注册**

### 方案 C: 上帝类拆分 ✅ (UftFutuMmStrategy.cpp 2924 → 2112 行)
| 新组件 | 职责 | 行数 |
|---|---|---|
| `FutuConfigLoader` | config.yaml 解析 + 边界校验 + 合约列表 (原 init 全段) | 254 |
| `FutuHotParamManager` | 26 个热参数注册(共享内存) + applyAll 分发 | 156 |
| `CloseoutOrchestrator` | closeout 触发/延迟启动/执行器驱动/订单回报跟踪/session 收尾 | 250 |
| `ArbExecutionBridge` | tick 推送/仓位回填/MM 快照世代号/订单回调执行/orphan 对冲/超时清理/残腿防护 | 304 |

- 原则: "搬移不改写", 每个组件拆完即编译验证
- 搬移中顺手修复: executeHedge 内 markCloseoutDraining 压缩时间戳 → epoch ms (又一处时间单位遗漏)

### yaml 模块化统一 ✅
- **发现**: coordinator.yaml 根级的 useMarketMaking/useSpreadArbitrage 一直被解析到错误层级(modules 节点)而实际丢弃 — dist 部署的开关是死配置
- coordinator 解析支持根级+modules 双位置(根级优先); 策略侧 7 个开关全部以 coordinator.yaml 为权威来源, config.yaml modules 节点降级为回退(向后兼容)
- dist 两个 yaml 增加分层职责注释: config.yaml=身份+业务参数, coordinator.yaml=模块开关+模块参数, spread_arbitrage.yaml=套利子系统

**遗留**: stra_* 返回 vector 出参化(需改 WtUftCore 框架接口)、TradingState::tryResumeFrom RISK_HALTED 断言防护、checkToxicityAndCircuitBreak 死声明清理。建议下一步: UftMocker 回测全量回归(对比重构前后 [QUOTE]/[RISK]/[SPREAD_ARB] 日志序列与 PnL)。

## 十三、复核修正记录 (2026-07-18, 编译通过)

> 经独立逐行复核发现的 5 个问题，全部修复。

| # | 问题 | 严重度 | 修复 |
|---|---|---|---|
| R1 | **F5 recordOrderFill 与 updateOrderQty 的 qty 字段语义冲突**: onOrder 部分成交分支 updateOrderQty 把 order.qty 改写为剩余量, recordOrderFill 的 `filled_qty >= order.qty` 变为与剩余量比较 → 多次部分成交后提前 untrack, 残留手数脱离 tracker | **P0** | UnifiedOrderInfo 新增 `original_qty` 字段(track 时赋值, 不被 updateOrderQty 改写), recordOrderFill 比对 `original_qty` | 
| R2 | **Bug30 cap 在归一化前施加**: [0.50, 0.05×4] 归一化后首权重 0.714 > cap | P1 | cap 移到归一化后施加, floor 仍在前(防归零); IR=0 ic_factor 从 1.15 修正为 1.0(`1.0+tanh(ir*2)`, 中性无升权) |
| R3 | yaml 注释"7 开关"实为 5 | P2 | 注释修正为"5 个策略级开关", 注明 toxicity/optimizer/adaptive 仍只查 modules 节点 |
| R4 | 死代码: _prev_mid_for_ic(F21 后无读取), _cache_dirty(F7 后无读取) | P3 | 删除字段与所有 dead write |
| R5 | F42 冷启动分支残留 `_spread_opts->find()`, 未用 tc.spread_opt | P3 | 改用 tc.spread_opt, 补齐 F42 宣称 |

**编译验证**: `make WtFutuCore` 通过, 零警告。







1. **第一周（正确性止血）**: 时间戳统一 / halt 恢复 / 日亏跨日 / 悬垂引用 / 部分成交 untrack / 孤儿单 ✅ 本轮已完成
2. **第二周（接线收尾）**: 套利 updatePosition 回填 / in_flight 生命周期修正 / 残腿检测器 / Fusion 与 MarketMakingEnhancer 用或删
3. **第三周（性能冲刺）**: 时钟单次读取 / 零堆分配 / 日志降采样 / CMake 选项
4. **第四周（架构还债）**: 拆上帝类 / 状态单源化 / ISpreadStrategy+信号注册表 / 异步套利二选一
