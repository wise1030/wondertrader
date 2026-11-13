# WtFutuCore 全面诊断报告（V8）

> **日期**：2026-08-20
> **范围**：`src/WtFutuCore/` 全部 90+ 源文件（约 3.4 万行）
> **方法**：逐文件代码级审计（非文档推导）。做市主链路（策略壳 / Coordinator / Quoter / 风控 / 下单 / 平仓）逐行阅读；signals / arb / 毒性检测 / 性能 / 配置基础设施由并行深度审计覆盖，全部发现均标注 `文件:行号` 证据。
> **基准**：README v7.7 + AGENTS.md 约定（delta/position 语义边界、策略层不解析柜台错误等）

---

## 0. 总体结论

架构骨架优良（流水线分层、决策链、双路径下单、分层状态机、replay 时钟等设计达到专业量化系统水准），但**大量业务功能处于"接线断链"状态**——代码存在、配置存在、执行路径不通；并发安全事实上依赖一把大锁兜底；毒性检测与套利子系统存在多处影响实盘行为的数值错误。

当前状态概括：**框架 8 分，兑现度 5 分**。

| 维度 | 评分 | 一句话 |
|---|---|---|
| 架构分层 | 8/10 | 流水线/决策链/双路径清晰；SpreadArbitrageManager 职责过载 |
| 正确性 | 5/10 | 毒性评分断链、套利止损发不出、频控对 MM 失明 |
| 并发安全 | 6/10 | 大锁下安全；目标态（细粒度）下至少 6 处假无锁；watcher 线程 P0 |
| 功能完整度 | 5/10 | 约 1500+ 行"存在的死代码"；多个配置是安慰剂 |
| 性能 | 8/10 | 热路径基本功扎实；遗留字符串哈希与日志浪费点 |
| 可测试性 | 3/10 | signals/arb/毒性三块 TestUnits 零覆盖 |

---

## 1. 架构现状评价（好的方面，予以保留）

| 设计 | 证据 | 评价 |
|---|---|---|
| 分层流水线 | `StrategyCoordinator::processTick` Stage 0→8（closeout→sectionBreak→preCheck→行情→信号→风控→撤旧→taker→报价），注释完备 | ✅ 清晰，职责链固定 |
| 策略壳瘦身 | UftFutuMmStrategy 864 行纯转发；Assembler/RuntimeOps 拆分（5A-3） | ✅ 到位 |
| 双路径下单 | MM 直调 ctx API（零中间层）/ 非 MM 走 OrderRouter（限速+STP+审计） | ✅ 符合 HFT 惯例 |
| 报价决策链 | QuotePolicyChain 6 policy 固定顺序（RiskWiden→ArbCloseSync→Toxicity→LimitPrice→ColdStart→FillRetreat）；v7.8 修复了 spread_mult 死写（绕 l0 中心拉宽） | ✅ 扩展点设计正确 |
| 分层状态机 | TradingState HSM 全原子+CAS（`setQuotingPhase` read-check-CAS、`tryResumeFrom` CAS）；RISK_HALTED 唯一恢复出口 | ✅ 防误恢复设计好 |
| **delta/position 语义边界** | `checkHardPositionRisk`（position→halt_quoting/pending_drain/side_pause）与 `computeInventoryStrategyInputs`（delta→skew/义务/block_add）已按 2026-08-19 原则分离；`checkTakerReduce` 用 position 口径（RiskCoordinator.cpp:49）；SpreadOptimizer 全链 delta 口径；启动校验 maxDelta/maxPosition 关系（UftFutuMmStrategy.cpp:291-303） | ✅ **全项目最干净的一条线** |
| 回测可复现意识 | replay 时钟统一注入 `_exchange_time_ms`（UftFutuMmStrategy.cpp:599-612），墙钟仅限纯日志限频 | ✅ |
| 记账权威统一 | UnifiedNetBook：引擎 local position/profit 为唯一权威，影子簿对账 + `markShadowStale` 降级日亏 halt（FutuRuntimeOps.cpp:104-148, FutuPortfolio.cpp:214-241） | ✅ 设计成熟 |
| 构建卫生 | CMake 已去 `-ffast-math` 保 NaN 风控（CMakeLists.txt:46-52） | ✅ |
| 事故沉淀 | requoteAfterFill 的 mid 平移重算+retreat 防死循环（StrategyCoordinator.cpp:1252-1341）等修复均有完整注释证据链 | ✅ |

---

## 2. P0 级问题（建议立即处理）

### P0-1 热参数 watcher 线程裸写主链路状态——唯一绕过 `_cb_mtx` 的写者

- **证据链**：`FutuHotParamWatcher.cpp:60` 独立 `std::thread` 检测 hotparams.yaml mtime 变更 → `syncFromFile`（FutuHotParamManager.cpp:196）→ **在 watcher 自己的线程里** `applyAll`，无锁改写：
  - `SignalAggregator::updateWeights`（SignalAggregator.h:200-213）：裸写 `_cfg` 5 个 double + `updateBaseWeights`，而 MdSpi 正在 `computeAlpha` 读同一批字段（SignalAggregator.h:591、ICWeightTracker.h:395-399）；
  - `FutuPortfolio::setParams`（FutuPortfolio.h:188）：约 200B 结构体整体赋值，可撕裂；
  - `t.config->quoting.base_spread = hotVal(...)`（FutuHotParamManager.cpp:93-96/143-146）：直写策略配置结构体。
- **对照**：引擎正规路径 `on_params_updated` 有 `FUTU_CB_LOCK_GUARD()`（UftFutuMmStrategy.cpp:803）；SpreadOptimizer 有 seqlock、Quoter/Portfolio 有 RecursiveSpinLock——唯独热更新这条**最高频的人为触发路径**裸奔。盘中调参正是该功能的设计场景。
- **附带**：`syncFromFile` 无类型/范围校验（FutuHotParamManager.cpp:182-185），`base_spread: "abc"` → 0.0、负值照单全收并 applyAll；watcher mtime 秒级粒度，同秒二次修改丢失；`start()` 首轮 loop 必然重复 sync 一次。
- **建议**：watcher 线程只置 dirty 标志/写共享内存，由 `on_tick` 在 `_cb_mtx` 内 drain 后 applyAll；热更新值复用 FutuConfigLoader 的边界校验。

### P0-2 做市报单完全绕过 ORDER_RATE 频控

- **证据**：`recordOrder()` 仅 3 处调用：RiskCoordinator.cpp:98（taker reduce）、ArbExecutionBridge.cpp:270/356。FutuQuoter 的 `stra_buy/sell/quote` 路径**从不计数**，`on_entrust` 成功分支（FutuRuntimeOps.cpp:758-780）也不计数。
- **后果**：`checkRiskLimits` 的 `orders > max_orders_per_sec(50)`（FutuRiskMonitor.cpp:228-237）只统计上述样本——策略自身频率风控对最高频的报单来源（MM）**失明**，MM 超限不会产生 ORDER_RATE violation。
- **建议**：在 `handleObligationQuote/handleFlexibleQuote/handleBilateralQuote` 下单成功处（或统一在 `on_entrust` 成功分支）补 `recordOrder()`。

### P0-3 `FUTU_CALLBACK_LOCK=0` 细粒度模式存在至少 6 处假无锁（当前靠大锁兜底）

代码明确把细粒度模式作为目标态（UftFutuMmStrategy.h:450-453），但以下组件在无大锁时即 data race：

1. `SelfTradeCalibrator::_retreat_states`（std::map）/`_contract_states`（wt_hashmap）：TdSpi `operator[]` 插入（SelfTradeCalibrator.cpp:111/117）vs MdSpi `find`（:412/:133）——头文件 h:259-270 注释自称"无锁设计"，实现不符（map 插入与查找并发是 UB）；
2. `PerformanceMonitor` 全部吞吐计数器为裸 uint64 自增（PerformanceMonitor.cpp:69-92），头注释宣称 "lock-free metrics collection"；
3. `SignalAggregator::updateWeights` / `FutuPortfolio::setParams`（见 P0-1）；
4. `FutuRuntimeOps.cpp:812` 的 `consumePairTag` 不在 bridge 锁内；
5. `FutuRiskMonitor::_last_soft_warn_ms`（mutable map，FutuRiskMonitor.h:582）：MdSpi 的 `checkPreTradePosition` 与 TdSpi 的 `checkRiskLimits` 双写；
6. `EventDispatcher::subscribe`（vector push_back）与 `dispatch` 并发本身是 race（当前零订阅掩盖）。

**建议**：要么建立"跨线程写点清单"逐项加 seqlock/atomic（照抄 SpreadOptimizer.h:192-221 的 F20 模式），要么正式宣布放弃细粒度模式、删除编译开关，避免后人误开。

### P0-4 MarketDataContext 装配断链：tick_size 全链三个互斥默认值

- **证据**：`MarketDataContext::setContract`/`setLargeTradeThreshold` 全仓零调用；`FutuComponentFactory::createMarketDataContext` 忽略参数直接 `return std::make_unique<MarketDataContext>()`（FutuComponentFactory.cpp:46-49）。
- **后果链**：
  - `OrderBookStateTracker` 默认 `_tick_size(0.2)`（MarketDataContext.cpp:17），而 EC 合约实际 tick=0.5 → depth_imbalance 距离权重 `(mid-price)/_tick_size` 系统性偏差 2.5 倍；`getSpreadTicks()` 对 tick≠0.2 的合约输出错误 tick 数；
  - `TickTransactionInferer` 用自己的默认 `tick_size(1.0)`（TickTransactionInferer.h:120-121），方向分类容差全部失真；
  - 大单阈值两套口径：tracker 默认 10.0 vs SignalAggregator 传 50.0。
- **建议**：`createMarketDataContext` 接收 ContractInfo 并调 `setContract`；`onTick` 首帧校验 setContract 是否被调过；tick_size（Context/Inferer/SignalContext）单一来源。

---

## 3. P1 级 Bug（按子系统，附证据）

### 3.1 毒性检测——评分链路数值断链（直接驱动 ToxicityPolicy 停报价）

| # | 问题 | 位置 | 后果 |
|---|---|---|---|
| T1 | **毒性方向 toxic_side 永远为 0**：StrategyCoordinator 构造 `AlphaResult` 只填 `alpha/is_strong_signal/timestamp`，不填 `ofi_component`（恒 0），而 toxic_side 判定要求 ofi_component 非零；`SignalContext.alpha.ofi_component` 其实在 slot 里已算好（SignalAggregator.h:339），纯搬运遗漏。另一喂入路径 `onSyntheticAlpha` 同样漏填 | StrategyCoordinator.cpp:914-924 vs PredictiveToxicity.cpp:159/186-190 | 买毒/卖毒单边抑制从不生效，永远走双边抑制的 else 分支 |
| T2 | `self_trade_weight` 双重施加：RealizedToxicity 内部乘 `_cfg.weight`、门面再乘 `realized_weight`，realized 通道有效权重 = 0.4²=0.16 | RealizedToxicity.cpp:60 + ToxicFlowDetector.cpp:44/149-151 | 已实现毒性被稀释 2.5 倍 |
| T3 | VPIN 数值无界且系统性高估：imbalance 按整桶量算（单笔 2V 买单→imbalance=2V）而分母用 `桶数×bucket_size`；关桶时超额量直接丢弃 | PredictiveToxicity.cpp:59-85 | VPIN 可 >1.0，阈值判断失真 |
| T4 | 门面丢弃 PredictiveToxicity 的独立 VPIN 触发条件：`pred_result.is_toxic` 被丢弃，VPIN 单独触发实际需 >2×vpin_threshold | ToxicFlowDetector.cpp:130-166 | 配置语义与实际阈值差 2 倍 |
| T5 | **toxic_side 语义三处矛盾**：PredictiveToxicity.h:59（1=buy toxic）vs SelfTradeCalibrator.h:94（1=avoid sell）vs .cpp:345 推导；QuotePolicyChain.h:216-219 疑似抑制错边——激进买流应停 **ask**（先被知情买方吃掉）而代码停 bid | QuotePolicyChain.h:216-229 | 一旦 T1 修复，方向可能反 |
| T6 | 权重不归一化：combined=0.5/0.5 硬编码；alpha 通道 ofi_weight+trade_weight=0.6，理论上限 0.3；无归一化校验（validateSignalWeights 只管 SignalAggregator） | PredictiveToxicity.cpp:166-176 | 分数尺度失真 |
| T7 | warmup 内 alpha 通道照常生效（设计如此），但 `large_trade_ratio` 从不传递（构造 trade_res 不填），`0.5+0.5×0` 恒 0.5 | StrategyCoordinator.cpp:920-922 | alpha_toxicity 上限被压到 0.15×\|ratio\| |

### 3.2 信号子系统——装配断链与数值缺陷

| # | 问题 | 位置 | 后果 |
|---|---|---|---|
| S1 | （见 P0-4 tick_size 断链） | — | — |
| S2 | LeadLag `_current_mid/_current_timestamp` 未初始化即被读（UB） | LeadLagSignalSource.h:58/182-183/207 | 当日首笔 tick 为 anchor 时触发（anchor 通常最活跃） |
| S3 | 单边盘口（锁板）时 mid/spread 保留上一双边时刻陈旧值（bids/asks 每帧清空重建但 derived metrics 只在有双边时更新），与策略层 C1 修复的 mid=0 跳过语义不一致 | MarketDataContext.cpp:37-67 vs UftFutuMmStrategy.cpp:618-624 | 锁板期间 RealizedVol 推 0 收益使波动率坍缩→vol_tier 误判、动量/IC 被陈旧 mid 污染（AGENTS.md 记载锁板是真实事故场景） |
| S4 | Coordinator 反向裸写 Aggregator 内部状态（非 const `getContext()` 后手写 `mutable_sig_ctx.toxicity.*`） | StrategyCoordinator.cpp:879/954-963 | SignalContext"单一真相源"名存实亡，双写者 |
| S5 | OFI bid/ask_pressure 公式代数退化为阶跃（ofi>0 时恒 bid=1/ask=0，ofi=0 处不连续翻转） | OFISignalSource.h:121-128 | 潜伏数学 bug（暂无下游消费者） |
| S6 | momentum 的 window 配置被静默忽略（实际固定 RingBuffer 128）；LeadLag 的 lag_ms 完全未实现、window 固定 64、scale_factor 注释 10000 实际 3000 | MomentumSignalSource.h:37/125、LeadLagSignalSource.h:55/178/194 | 安慰剂配置 + 注释漂移 |
| S7 | `computeAlpha` 170 行上帝方法；regime 检测（两套滚动 MA deque）留在 Aggregator 而非 ICWeightTracker（Layer2 输入与权重框架耦合） | SignalAggregator.h:502-679 | 层次耦合，扩展困难 |
| S8 | 权重归一化含未启用信号（cap-after-normalize 语义失真）；BOOK regime 因子 1.3/0.7 硬编码 | ICWeightTracker.h:394-462/524-527 | 调试权重占比失真 |
| S9 | TradeFlow 双通道（tick 推断 vs 逐笔）共用同一累积器，L2 场景同一成交量计两次（注释自认） | MarketDataContext.cpp:141-262 | 口径不一致 |
| S10 | vol_percentile 名不副实（线性映射非历史分位）；EC 常态波动 ≈1.35e-4 而 elevated/extreme 配 0.002/0.004 ≈15/30 倍常态——ELEVATED/EXTREME 分档与 should_widen/should_pause 闸门对 EC 基本不可达 | VolatilitySignalSource.cpp:82 + coordinator.yaml | 市场状态闸门失效 |

### 3.3 套利子系统——数学错误 + 状态机碎片化

| # | 问题 | 位置 | 后果 |
|---|---|---|---|
| A1 | **`spread_impact` 空价差符号错误**：short 分支 `pi1+pi2` 应为 `pi1-pi2`（文件内注释写的公式与代码不一致，证明是笔误） | AsyncArbitrageExecutor.cpp:437-444 | 净改善 +1.2 tick 的有利平仓/止损单被错误 REJECTED——**止损单发不出** |
| A2 | **TrendFollowing 止损是死代码**：`SpreadState::current_price` 全链路从未赋值（grep 证实仅构造置 0） | TrendFollowingStrategy.cpp:155-169 | 趋势价差仓没有任何价格止损 |
| A3 | StatArb 微结构因子恒 0：`mid_price/bid_price/ask_price/total_volume/buy_volume/sell_volume/average_trade_size` 从未写入 → mspread 因子（15% 权重）无效、volume_imbalance 恒 0、流动性得分恒 0 | StatisticalArbStrategy.cpp:52-55/163-179、SpreadRiskManager.cpp:150-151 | "多因子统计套利"实际只剩 4 因子 |
| A4 | PairsTrading 协整检验 `testCointegration` 从未被调用（入场仅相关性+残差 std 把门），且死代码内部公式缺回归残差方差项（接上会"永远协整"）；价格历史不做腿间同步配对（任一腿 tick 都 push 样本，leg1 连续两 tick 会 push 两个 (new_leg1, stale_leg2)），与 SpreadCalculator 的 fresh-pairing 双重标准 | PairsTradingStrategy.cpp:158-219、SpreadArbitrageManager.cpp:365-375 | 宣称的协整门不存在；OLS beta 被陈旧价格污染 |
| A5 | 时间基准三重混用：position_open_time/in_flight 超时/信号冷却用 `high_resolution_clock`，B5 冷却/hedge 过期用 replay `_now_ms` | SpreadArbitrageManager.cpp:736-741、AsyncArbitrageExecutor.cpp:246-248 | 回测中 TIMEOUT_EXIT（3600s）与 maxDivergenceTime（7200s）永不触发 |
| A6 | 孤儿腿对冲 fire-and-forget：回调 rejected/rate_limited 只打日志不重试、一次性移出 deferred；对冲数量用 leg1 口径（ratio≠1 时错）、ctx fallback 路径用 leg1 原价下 leg2 单 | AsyncArbitrageExecutor.cpp:594-677、ArbExecutionBridge.cpp:341-352 | 裸腿敞口仅剩 120s 超时兜底 |
| A7 | 平仓方向逻辑 3 处重复实现、依赖 3 个不同时点仓位快照；`onLegCancelled` 无差别清零 open+close 双 in_flight（部分成交+撤单场景闸门提前放行，可与残腿对冲单叠加建仓） | SpreadArbitrageManager.cpp:893、AsyncArbitrageExecutor.cpp:336-347、ArbExecutionBridge.cpp:134-161、SpreadArbitrageManager.cpp:1046-1058 | 高危维护区 |
| A8 | **EMERGENCY 级套利风控无执行动作**：`handleRiskAlert` 只日志+EventNotifier 广播；`portfolioStopLoss` 达阈生成 EMERGENCY 既不 halt 也不触发 closeout | UftFutuMmStrategy.cpp:195-214、SpreadRiskManager.cpp:296-305 | 套利"组合止损线"是假保险丝，兜底全靠做市侧另一套日亏线 |
| A9 | 残腿对冲单冒用 `Source::CLOSEOUT`：污染 closeout 订单统计、意外享受 Fix4 的 REVERSIBLE halt 豁免（orphan 队列路径已正确用 HEDGING） | ArbExecutionBridge.cpp:418-422 | 语义错位三连 |
| A10 | bridge 平仓单 skip 路径不释放 `close_in_flight`（live_pos×signed_qty≥0 skip、exe_qty<0.5 skip 均直接 return 不调 onArbSignalDropped），止损场景 5s 内被 B4 防双发抑制 | ArbExecutionBridge.cpp:141-161 | 真实风险敞口窗口 |
| A11 | `calculateVolatilityFeature` n==10 时 0/0=NaN → composite 钳位成最强做空信号 +1.0（当前被 min_samples=50 挡住为潜伏 bug）；`estimateHalfLife` 零方差除零 → half_life=NaN 绕过 MeanReversion 半衰期过滤 | StatisticalArbStrategy.cpp:127-138、SpreadCalculator.cpp:393-396 | 配置调整即触发 |
| A12 | closeout 期间 bridge 整体早退（`isCloseoutTriggered()` 在 pushTick/processPendingOrders/processOrphanLegs 全部之前），冻结孤儿腿对冲与 in_flight 清理 | ArbExecutionBridge.cpp:40-41 | closeout 失败回退时裸腿持续暴露 |
| A13 | `getArbCloseDirection` 的 any-match 取无序遍历首个 intent，1:N（一合约多 pair）方向相反时静默丢信息 | SpreadArbitrageManager.cpp:1296-1324 | QuotePolicyChain 单侧抑制依据不确定 |
| A14 | SpreadRiskManager 无自身同步原语，`getRiskSummary/getActiveAlerts` 公有接口不加锁（当前侥幸无调用者，一旦主线程调用即与 arb 线程 data race） | SpreadRiskManager.h:238-248 | API 地雷 |

### 3.4 订单执行与风控（主链路）

| # | 问题 | 位置 | 后果 |
|---|---|---|---|
| R1 | `side_pause_bid` 与 `side_pause_ask` 恒相等（`isPaused(code,now)` 不分方向，两字段同值赋值） | FutuRiskMonitor.cpp:1106-1107 | 字段语义误导；同侧熔断实际暂停全合约（cancelAll 也是全撤，行为一致但声明不符） |
| R2 | 双份 `_last_mid` 并行维护：策略壳一份（UftFutuMmStrategy.h:442）+ Coordinator 一份（StrategyCoordinator.cpp:447-454/824-826），每 tick 双写；requoteAfterFill 读 C 版、on_transaction 读 S 版 | 两处 | 重复状态，漂移风险 |
| R3 | PerformanceAnalyzer 时间戳双域混用：recordTrade 用日期合成格式（~2×10¹³）vs onTickUpdate 用 actiontime（~9×10⁷），`now > trade_timestamp` 恒假 → 30s 低流动性强制过期分支永不生效 | PerformanceAnalyzer.cpp:144-145 | pending 记录滞留 |
| R4 | TscClock 四项缺陷：`__rdtsc()` 无 lfence 序列化（测量抖动数十 ns）、无 invariant-TSC 检测、无周期重校准、未校准静默回退 0.4 | TscClock.h:23-45 | 延迟直方图精度可信度存疑 |
| R5 | FutuConfig 读取语义：键存在但值为空/类型错 → `asDouble()` 返回 0.0 而非默认值（`protectTicks:` 空值→价格保护静默关闭；数字 1 会被 asBoolean 变 false） | FutuConfig.cpp:14-15 + WTSVariant 语义 | 配置事故温床 |
| R6 | FutuConfigLoader：`contracts` 缺失/类型错误静默通过（无 else 分支），策略零合约空跑；anchor_code 未校验非空/在列表内 | FutuConfigLoader.cpp:41-42 | 启动 fail-fast 缺失 |
| R7 | `trade.spread_at_trade = cs ? cs->tick_size*2.0 : 0.2` 硬编码兜底 | FutuRuntimeOps.cpp:220/247-258 | 统计口径含默认值噪声 |
| R8 | FutuQuoter::onOrder tracker 未命中时 O(n) 线性 fallback 扫全部 level | FutuQuoter.cpp:753-774 | 订单多时退化 |

---

## 4. "假功能"与死代码清单（认知陷阱重灾区）

以下功能**配置存在/代码存在，但执行路径不通**——比删除更危险（给人已生效的错觉）：

| 项 | 证据 | 定性 |
|---|---|---|
| SyntheticSignalFusion 整个子系统（710 行） | `feedTickInference/feedBookSignal/addSelfTradeCalibration` 零调用；`runFusionCycle` 因 `hasAnySource()==false` 恒早退但**每 tick 仍被调用**（UftFutuMmStrategy.cpp:571-573）；内部还有 4 个 bug（置信度顺序更新错误、`_large_volume` 从不累加、硬编码 0.5 阈值、pruneHistory 无符号下溢） | 删除或真正接线 |
| MarketMakingEnhancer（`enhanceMarketMaking: true` 是空开关） | `s_observe_enhancer` 默认 false（QuotePolicyChain.h:152/171-172），`setQuotingCallback/applySignal/shouldPauseQuoting` 全部零调用 | 半接线 |
| adaptiveParam | 空函数体（StrategyCoordinator.cpp:1354-1363），启动时自曝 no-op（处理诚实） | 删配置或实现 |
| CorrelationManager 套利接口 | `getSpreadSignals()` 返回 `{}`（:295-298）、`hasSpreadOpportunity` 硬编码 ratio=1.0、`getCorrelationsFor/getAggregateDelta/removeContract` 零调用 | 半成品 |
| arb 假配置群 | 读入后无消费：`correlationWindow/minCorrelation/maPeriod/breakoutThreshold/halfLife/mmEnhancementWeight/useHybridStrategy/price_offset_ticks/upgrade_to_taker/strategy_overrides`；从未从 pair 段加载：`convergence_timeout/spread_type/max_single_leg/trend_ma_fast/slow/min_trend_strength/expiry_threshold`；合约乘数默认 300 是烟雾弹（恒 1.0，乘数死亡链：loadConfig 不读→onTick 丢弃→calculator 恒 1） | 配置 schema 收敛 |
| 双层置信度阈值不一致（0.3 vs 0.5） | Manager 放行（SpreadArbitrageManager.cpp:538-540）→ 执行器丢弃（AsyncArbitrageExecutor.cpp:315），[0.3,0.5) 信号每 arb 周期（5ms）空转一轮"生成→过闸→丢弃→释放" | 统一为单配置 |
| L2 数据入口空实现 | `onOrderQueue/onOrderDetail` 被 `(void)data` 丢弃但已真实接线（UftFutuMmStrategy.cpp:654-670） | 明示 TODO |
| arb 默认配置无主动止损 | `arb_close.enabled=false`（纯 B-3），所有 STOP_LOSS/TIMEOUT_EXIT/CLOSE 信号被门无条件丢弃，策略四个精心实现的退出分支出厂无效 | 被丢弃时输出一次性告警而非 debug 静默 |
| 死方法群 | `computeIntent/combineSignals/testCointegration/recordOutcome+updateAdaptiveWeights/checkConvergenceFailure/getAllowedPositionSize/getRiskSummary/getActiveAlerts` 等零调用 | 大扫除 |
| PerformanceMonitor warn/critical/logInterval 死接线、Percentile 枚举（P999=99 错误别名） | setWarnThresholdNs 等注入后无逻辑读取 | 大扫除 |
| PerformanceAnalyzer 占位指标 | `determineMarketCondition` 恒 NORMAL、`_start_time` 恒 0（trading_time_sec 恒 0）、`recordQuote` 忽略全部参数 | 大扫除 |
| BilateralQuoteStats `_last_minute_units`、RealizedToxicity `decay_factor`/book 数据、TickTransactionInferer 内部累计链（还带数值泄漏：prune 减 `volume×confidence` 留 `(1-confidence)×volume` 单调膨胀） | 五处赋值零读取 / 定义后无读取 | 大扫除 |
| ISignalCombiner + Registry、`updateWithContext`、TradeFlowSignalSource::onTrade SSOT 主路径 | 纯占位/死扩展点 | 大扫除 |

**规模结论**：signals+arb+infra 三块合计约 **1500+ 行"存在的死代码"**；TestUnits 对这三个子系统**零测试覆盖**（代码注释里每个"已修复"都对应一个应有而未有的测试）。

---

## 5. 性能评估（热路径每 tick）

**已达标的**（保留）：
- TickContext 组件指针预解析（每 tick 省 ~7 次字符串哈希查找）
- exp 衰减提升到 refreshQuotes 入口单次（原每 level 2 次）
- `nth_element` 替代 sort、`_dynamic_weights` 用 array
- F5 perf 门控（关闭时零 chrono 开销）、TscClock 埋点 ~6ns
- SpreadOptimizer seqlock 参数快照（F20，防热更新撕裂读）
- TdSpi 成交日志 SPSC 卸载（C11，队列满丢弃+计数，设计优雅）
- LockFreeQueue SPSC 实现正确（生产者/消费者内存序教科书式、cache line 对齐）；RecursiveSpinLock/OrderApiGuard 实现正确（锁序文档化是少见优点）
- RiskCoordinator 两阶段（锁内收集候选、锁外提交）

**遗留浪费点**（按收益排序）：
1. 每 tick 多处 `unordered_map operator[]`（字符串 key）：`_halt_quoting_state[code]`（StrategyCoordinator.cpp:1036）、`_last_soft_warn_ms[code]`（FutuRiskMonitor.cpp:1111/1170，即便不超限也执行插入）、`_last_requote_ms[code]`、`LimitPricePolicy::_touch_active.insert/erase(code)`、`QuotePolicyContext.code` 的 string 拷贝——建议 init 时预填 + TickContext 复用 slot 指针
2. `SIGNAL_DECOMP` debug 日志无降采样（toxicity 有 %50，这个没有；logcfg 开 debug 时每 tick 每合约一次 fmt+落盘）
3. `extractSignalResults/normalizeSignal/recordSignal` 的 4+ 次哈希查找（`_vol_source` 缓存模式已有，未推广到全部槽位）
4. RollingScaleTracker 仍用 500 元素 deque（非连续内存，每 20 tick 一次 assign+nth_element）
5. `checkRiskLimits` 的 RiskViolation 内嵌 std::string 每帧构造（message 仅超限时 fmt，但结构体本身必构造）
6. `EventDispatcher::dispatch` 每帧两枪空转遍历空 vector（有 `hasListeners()` 方法但未使用）
7. `getDeltaChangeRate` 每次栈上构造 32 元素 TimedSnap 数组+插入排序（可接受，但可预分配）

单项均在几十 ns～µs 级，对当前 2 合约规模不致命；对"扩到 10+ 合约"的扩展目标是主要障碍。

---

## 6. 架构分层改进建议（对应"模块清晰、便于扩展"的目标）

1. **信号子系统**：补齐装配契约（P0-4）；拆 `computeAlpha` 为 ICRecorder（信号-收益配对）/RegimeTracker（归还 ICWeightTracker）/ConfidenceCalculator 三协作者；删除非 const `getContext()`，毒性结果改为 `update()` 显式入参，恢复 SignalContext 单写者。
2. **毒性子系统**：评分收敛为单一纯函数 + 归一化权重（加载期校验和=1）+ 单测护栏（VPIN∈[0,1] 边界、warmup 门控、toxic_side 方向表驱动用例）；`toxic_side` 语义全链统一为一种定义（建议 1=激进买流，抑制 ask），方向问题必须先用回测 trades/positions CSV 裁决后固化进测试。
3. **套利子系统**：1459 行 SpreadArbitrageManager 拆三——纯计算（Calculator+Strategy 保持纯函数）/`ArbGatekeeper`（B-3 门、in_flight、CloseIntent、超时队列）/执行 FSM（`LegExecutionFSM`: PENDING→LEG1_SENT→LEG2_SENT→HEDGING→DONE，撤单/拒单/部分成交作为迁移事件），替代散落 applyB3Gate/executeSignal/bridge/onTradeFill 四处的隐式状态拼凑；引入贯穿 arb 的 Clock 接口（replay 优先、墙钟兜底）。
4. **状态收敛**：`_last_mid` 合并为单一属主（建议归 Coordinator，策略壳经接口读）；套利风控 EMERGENCY 接入 FutuRiskMonitor 的 halt 通道，消除两套独立止损账；SpreadRiskManager 要么自带锁要么把无锁接口降 private。
5. **配置系统**：加载后统一做"配置→组件"完整下发（含 multiplier/window/min_correlation/min_trend_strength），未消费键启动时 warn；`FutuConfig::read*` 区分 VT_Null/类型错误并回落默认值；空 contracts/anchorCode 直接 return false；两处置信度阈值合并。
6. **并发**：兑现或放弃 `FUTU_CALLBACK_LOCK=0`（见 P0-3），并把 watcher 线程收编（P0-1）。
7. **观测链路时钟统一**：全模块统一 replay epoch-ms（SelfTradeCalibrator 已做，PerformanceAnalyzer 是漏网者）；TscClock 升级 `lfence; rdtsc`/`__rdtscp` + invariant-TSC 探测 + 低频重校准，未校准从静默 0.4 改为显式失败。

---

## 7. 修复路线图

| 阶段 | 内容 | 工作量 | 验证 |
|---|---|---|---|
| **R1（优先，本周可完成）** | P0-1 watcher 收编（dirty 标志 + tick 线程 drain）；P0-2 MM 频控计数；P0-4 `setContract` 接线；T1 ofi_component 一行搬运；A1 spread_impact 一行符号修复 | 小（5 项均为一行到几十行） | Release 编译 + TestUnits + 回测回归 + 远程单实例观察 |
| **R2** | 毒性评分归一化+方向裁决（回测验证后固化）；S2 LeadLag 未初始化成员；S3 单边盘口统一；A2/A3 current_price/微结构字段填充或删分支；A9 `Source::HEDGING` 独立；A10 skip 路径释放 in_flight | 中 | 同上 + 毒性方向表驱动单测 |
| **R3** | 死代码大扫除（SyntheticSignalFusion/Enhancer/假配置/死方法）；TestUnits 补 signals/arb/毒性回归 | 中 | 测试覆盖三子系统 |
| **R4** | SpreadArbitrageManager 拆分 + LegExecutionFSM；A5 arb 时钟统一；computeAlpha 拆解；P0-3 细粒度并发契约兑现 | 大 | 结构性工程，宜在 R1-R3 测试护栏建立后进行 |

**优先级原则**：R1 五项均为小改动但直接改变实盘行为（止损单能否发出、频控是否失明、热更新是否崩坏、tick_size 尺度是否正确），建议最先做；R4 是结构性工程，必须有 R3 的测试护栏兜底。

---

## 附：本报告的证据来源

- 主链路逐行阅读：UftFutuMmStrategy.h/.cpp、StrategyCoordinator.h/.cpp、FutuQuoter.h/.cpp、FutuRiskMonitor.h/.cpp、RiskCoordinator.cpp、FutuPortfolio.h/.cpp、FutuRuntimeOps.cpp、OrderRouter.h/.cpp、QuotePolicyChain.h、TradingState.h、SpreadOptimizer.h/.cpp、RiskLiquidator.h、CMakeLists.txt
- 子系统并行深度审计：signals/ 全部 14 文件、arb/ 全部 16 文件 + 外围（ArbExecutionBridge/MarketMakingEnhancer/SelfTradePrevention）、毒性三模块、性能两模块、配置/热参数/MonitorBridge/LockFreeQueue/SpinLockGuard/TdSpiOffload/EventDispatcher/OrderApiGuard/FutuComponentFactory
- 交叉验证：recordOrder/recordCancel 全部调用点 grep、`_last_soft_warn_ms` 声明核查、`setContract` 零调用核查、watcher 线程链路核查

*所有行号基于 2026-08-20 工作区代码状态；修复后请同步更新 README v7.x 架构描述与 AGENTS.md。*
