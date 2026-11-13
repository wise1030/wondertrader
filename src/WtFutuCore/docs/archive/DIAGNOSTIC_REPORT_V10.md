# WtFutuCore 全面诊断报告 V10

**诊断基线**: HEAD = `a1f78a57`（2026-08-26 提交）。只读诊断，未改任何代码。
**诊断方式**: 五路并行深审（做市报价链 / 信号因子链 / 套利子系统 / 执行+风控链 / 性能设施·配置·测试·文档），逐文件带 行号证据 复核；关键发现经主会话二次抽查确认。
**与前轮关系**: 已对照 AGENTS.md 全部历史记录（V8-R1~R5 / R4b / V9-R6a,b / 去大锁 WS-A,E,F / 08-24 复核修复包 A-D批 / 08-26 小修包），**本报告只列当前 HEAD 上仍存在的新发现**；已知裁决项单独归档于 §7。

---

## §1 总体结论

| 维度 | 评分 | 说明 |
|---|---|---|
| 架构分层 | ★★★★☆ | 壳层(UftFutuMmStrategy)→装配(FutuModuleAssembler fail-fast)→运行时事件(FutuRuntimeOps)→协调(StrategyCoordinator/PolicyChain)→领域模块，单向依赖成立；做市报价 GLFT→策略链→报价引擎 解耦清晰 |
| 并发安全 | ★★★★☆ | WS-A 停机域收编后无成片裸奔区；新发现两处卫生级缺口（日切清环锁域、bridge._now_ms 明域标量）；锁层级审阅无成环 |
| 报价/风控分离 | ★★★★☆ | delta(策略)/position(风控) 语义边界落地且有单测护栏；RiskLiquidator 冒用 Source::CLOSEOUT 是 Source 口径的最后漏网 |
| 交易执行 | ★★★★☆ | B+ 槽状态机闭环严谨；**撤单侧 Router 与 Tracker 不对称**是无 zombie 保护的最大残余面 |
| 信号因子 | ★★★★☆ | 方向语义全链路一致（ toxic_side→停 ask 在位）、VPIN 有界、权重归一均已核验在位；剩余风险集中在**时间维度工程卫生**（跨间隙/session 不复位） |
| 套利子系统 | ★★★☆☆ | 结构债大幅缓解后，剩余问题集中在三盲区（生产独有路径+回测不可见+无单测）：告警根因、残腿对冲不对称、通道级撤销粒度 |
| 配置治理 | ★★★☆☆ | 三层权威矩阵清晰、热参加固到位；但 loader 存在 **NaN 全量穿透**缺陷，src↔dist 出现应急调参后的同步漂移 |
| 测试护栏 | ★★★☆☆ | 15 文件 ~130 用例覆盖热参/毒性方向/B+/WS-A 等事故面；核心缺测：QuotePolicyChain 六策略表驱动、FutuQuoter/RiskCoordinator/Closeout 族直测、回归判据脚本化 |
| 文档 | ★★★★☆ | 08-24 重写后准确率高（GLFT 公式/流程描述抽查吻合）；行号锚点轻中度漂移 + 个别描述失真 |

**一句话总评**: 经五轮迭代，系统的事故高发面（订单槽、停机域、毒性方向、回测判据、配置语义）已基本闭环且护栏成型；**本轮发现的重心从"正确性结构缺陷"转向三类问题——① 治理不完全的已知根因（告警风暴、clip 过冲）、② 生产独有路径的"最后一公里"（撤单侧、套利灰度路径、同步对冲）、③ 工程卫生（时间域、复位缺口、NaN 穿透、配置漂移）**。无 P0；两个 P1 均为"防线存在但可被绕过"，属可控增量修复，无结构性返工需求。

---

## §2 架构与分层评估（五大子系统）

### 2.1 做市报价 ✅ 分层清晰度高
- 数据流单向：GLFT(SpreadOptimizer 纯函数、seqlock 参数) → QuotePolicyChain 六策略 → QuoteRequest 数据包 → FutuQuoter 三路径(A双边/B1义务/B2灵活) → stra_* 引擎调用。FutuQuoter 无向上回调业务层，反向依赖仅 QuotePolicyChain.h:36-39 直接 include 两个业务头（已被批次 A4 边界声明承认，可接受）。
- B+ 订单槽生命周期（发送不清槽→终态清槽→空槽门禁→zombie 升级闩锁）经逐路径核对**无新裂缝**。
- 遗留姿态项见 §7。

### 2.2 信号因子 ✅ 微结构正确性闭环，时间维度是短板
- 方向一致性核验通过：OFI(+ =买压)、book imbalance(+ =bid 重)、trade net_flow(+ =buy)、toxic_side==1→停 ask（知情买方吃我方 ask）。updateToxicity 单写者模型恢复在位；computeAlpha 现 ~145 行线性。
- **短板**：全部差分类估计器（OFI prev-state/Momentum last_mid/Vol 返回序列/TickInferer _prev_state/LeadLag 历史）无跨间隙守卫；coordinator.resetSession() 运行期零调用方 → 午休/小节休息/开板第一帧确定性注入伪信号（详见 P2-6）。

### 2.3 交易执行 ✅ MM/arb/hedge/closeout 四路分流清晰
- MM 走 FutuQuoter→tracker 全生命周期；其余走 OrderRouter（频控+STP+来源审计）；IOrderSink 窄接口落地；finalizeOrder 对拒单无回调路径双侧幂等兜底。
- 残余：Router 撤单侧"标记即完事"，无 B+ 同款重试/zombie（P2-14）；Router pending_cancel 计数排除规则成为 closeout 结算门禁的漏洞向量（P1-2b）。

### 2.4 风控 ✅ 结构好，日切竞态是新发现的最后卫生项
- WS-A 后 `_halt_*`/`_closeout_state` 全触碰点均在 `_halt_domain_lock` 内（逐点 grep 核验达标）；D1 ERROR 单轨化后的双保险矩阵复核闭环（ERROR 态 canQuote=false 且 isTradingHalted=false 两契约分别由 requoteAfterFill :1274 / processTradeFill :342 / RiskCoordinator :118 兜住）。
- TradingState CAS 转移矩阵合法，H→N 单路恢复白名单成立；唯一后门 tryResumeFrom(RISK_HALTED) 现无调用方（建议固化拒绝）。
- 新缺口：resetDaily 清频控环在 `_halt_domain_lock` 内而生产者持 `_rate_lock`（P2-13）。

### 2.5 套利 ⚠️ 结构债缓解后，生产盲区浮出为主要风险
- 信号闸门族（confidence 统一键→B-3 双族→C0→B4 防双发→B5 过冲→B1 intent 广播）数学与符号互洽性复核通过；孤儿腿异步队列路径（A2 修复版）正确。
- 主要问题集中在：sync hedge-on-fill 路径与 A2 不对称（P2-7）、告警链根因（P1-1）、pair 级盲撤（P2-10）、以及整个 shutdown/retry 家族落在"实盘独有+回测 disabled+无单测"三盲区（P2-12）。

---

## §3 Bug 修复建议

### P1（两个，均为"防线被绕过"型）

#### P1-1 告警风暴根因未治理：CORRELATION_BREAK 无 warmup 门控 + CRITICAL 稳态区不受节流 —— 08-26 节流包只做了症状层
- **根因一**：`arb/SpreadRiskManager.cpp:171-179` 判据仅 `state.correlation < max_correlation_break`，**无 `state.is_active`（≥min_samples）前置条件**。calculator 预热期 correlation 恒 0 → 从首 tick 起每周期必发 WARNING。这正是 08-26 "单日 3.4 万行刷屏、估值器输入待查"的机制本体。
- **根因二**：`UftFutuMmStrategy.cpp:200-215` 节流键 `pair#type` 仅作用于 WARNING。而 `SpreadRiskManager.cpp:154-157` 在 drawdown ∈ [0.8×,1.0×]×portfolioStopLoss 区间生成 CRITICAL STOP_LOSS，且每 arb cycle（~5ms）全量重发（SpreadArbitrageManager.cpp:833-843）→ 大亏后回撤长停该区间时以 ~200 行/秒刷新日志并外发 EventNotifier，**换了个等级重演同一事故**。
- 附带：预热期假阳性 permanently 占用 60s 节流窗，首个真实破裂反而可能被静默 60s。
- **建议**: ① CORRELATION_BREAK/DIVERGENCE 增加 is_active 前置；② 节流改"WARNING 60s + CRITICAL 独立键短窗节流（如 10s）+ EMERGENCY 不限"；③ RiskManager 内可加 Δ<ε 不重发。补单测："预热期零告警"。

#### P1-2 Closeout clip 四个无守卫过冲向量 —— 解释 08-26 实盘 "20 手 clip → 31 变 -9"
- **a. stale-delta 复用窗口**：`CloseoutExecutor.cpp:210-222` inflight 归零判定与 `:225-227` 重算 `_remaining` 同 tick 衔接，完全依赖"Trade 回报先于该单终态 Order 回报到达"。CTP 终态先到时第二 clip 按旧持仓假设发出满额。
- **b. inflight 门禁被 pending_cancel 排除规则提前打开**：`OrderRouter.h:168-174` getActiveCountBySource 排除 pending_cancel 单，而 cancelAllBySource/cancelByPair "标记即算清"。B+ 已实证 doCancel 同步失败会被静默吞掉——撤不掉的单在交易所继续存活成交，计数却已归零 → 代际重叠下单。
- **c. DRAIN 超时强行续跑**：`CloseoutExecutor.cpp:134-141` 超时后无视仍活跃订单进入 ASSESS/EXECUTE，旧单后续成交不参与 remaining 核销（:511-551 只按 delta 差记账）。
- **d. close+open 明文放行 + ±0.5 取整抬升**：`:289-293` 注释自认"`|remaining|>|anchor 持仓|` 时允许 close+open——设计意图"；`:285-286` batch 下限强制 ≥1 手。组合口径 remaining 折叠非 anchor 合约 delta，anchor 单腿超卖翻空抵组合净额正是 "31→-9" 的签名形态。
- **建议**: ① 重算 `_remaining` 改锚定引擎真实仓位 `stra_get_local_position`，并提供可选配置 "close-only 模式"（默认保现组合语义）；② 结算门禁要求 "router count==0 且关联 trade 流静止 ≥1 tick"；③ pending_cancel 单计入 count 或引入撤单确认超时回收；④ 删除 `:279-280` last-resort 全量派发（改升档价格而非放量）。

### P2（按子系统分组，共 16 项）

#### 做市报价链

**P2-1 事件驱动补挂用陈旧时间戳作 place_time → 安静盘口"挂出即 STALE"的挂撤 churn 循环**
- `StrategyCoordinator.cpp:1409-1411` refreshQuotes 的 `now` 传 `q.timestamp`（CachedQuote 缓存的上一报价 tick 时刻），而同函数入参 `now_ms`(:1264) 与限频表(:1302) 用的是新鲜 replay 时间；processQuoting 主路径正确传 tc.timestamp(:1189)。该 now 经 trackMMOrder 写成 place_time。
- 影响：静默数分钟后触发的事件驱动补挂，新单 birth 为数分钟前 → 下一 tick 即被判 STALE(max_age 10s) 撤单 → 终态再触发补挂，形成以 requoteAfterFillMinIntervalMs 为周期的重复挂撤，虚增撤单率并撞 CTP 流控。
- 建议：`:1411` 改传 `now_ms`（一行修复，时间域同为 replay ms）。

**P2-2 taker 减仓 blocked/rate_limited/失败分支不写冷却戳 → tick 频率重试 + 多层 WARN 刷屏**
- `RiskCoordinator.cpp:90-106` 仅成功发单分支写 `_last_taker_reduce[code]`；三个早退分支仅 warn("will retry next cooldown")——实际下 tick 即重试；每次尝试叠加 OrderRouter 内部 warn（OrderRouter.cpp:56-66/:102-105），其中 STP 被双重告警。util≥1.3 且持续冲突时（恰是事故态）以 tick 频率 spam。与本仓两次治理过的"每 tick 告警刷屏"同类。
- 建议：早退分支同样记录冷却戳；blocked 类日志并入 1s 节流。

**P2-3 路径A(双边接口)：allow 抑制产生的 qty=0 单腿仍原样提交 stra_quote**
- obligation 定价 allow 清量（FutuQuoter.cpp:93-100），但 handleBilateralQuote(:267-274/:581-583) 对 0 量腿不剔除，allow 仅用于 tag 选择；头注释声称"风控通过价格偏移体现"（FutuQuoter.h:430）但实现层无该换算。路径A 默认关闭故休眠；启用前必须裁决 0 量腿语义（跳过/价差表达）否则将向交易所提交无效单腿。

#### 信号因子链

**P2-4 信号栈无跨间隙/跨 session 复位守卫（伪信号注入）**
- coordinator.resetSession()/resetDaily()（StrategyCoordinator.h:371-372）运行期**零调用方**——FutuRuntimeOps::onSessionBegin(:602-677) 只复位 risk/portfolio/trading_state/arb/closeout；SignalAggregator::reset()/MarketDataContext::reset()/ToxicFlowDetector::reset() 生产路径均不可达。
- 所有差分估计器（OFI prev-state OFISignalSource.h:68-138、Momentum `_last_mid`、Vol 返回序列、TickInferer `_prev_state`、LeadLag 历史）无 time-gap 判据。
- 影响：午休/小节休息/开板第一帧把多小时陈旧状态当作突变——整档挂单量记为一笔高置信"大单"污染 trade/毒性通道；vol 单个跳空收益使 realized_vol 数十 tick 超标（widen/pause 误触发）。生产靠 cron 双重启掩盖了跨日部分，日内长间隙真实暴露。
- 建议：各源 update 入口加 `ts-prev_ts > gap_ms(5000)` → 丢帧并重置 prev-state；onSessionBegin 补调 coordinator.resetSession()（内部扩展至 aggregator/book/toxicity）。注意 RollingScaleTracker::initScaleTrackers 的 emplace-no-op 陷阱需一并修（SignalAggregator.h:725-735 改 `[type]=...`）。

**P2-5 TradeFlow fallback 通道 valid/confidence 恒真 —— 清淡期稀释 alpha 并污染 consistency**
- signals/TradeFlowSignalSource.h:76-89：else 分支无条件 valid=true，confidence≥0.5；而 TradeFlowAnalysis 构造默认 buy=sell=0.5 → 零成交也满置信参与加权。连带 computeAlpha consistency 把 ≥0 一律计正（SignalAggregator.h:616-623），ICWeightTracker majority_sign==0 时取 -1 的缺省偏置（ICWeightTracker.h:446）叠加放大清淡期权重/置信系统性偏移。
- 建议：fallback 按 n>3 才置 valid；consistency 对 |v|<eps 不计分子分母。

#### 套利子系统

**P2-6 同步 hedge-on-fill 对冲簿记不校验受理 + markLegRejected 不换算 ratio（与 A2 已修路径不对称）**
- `ArbExecutionBridge.cpp:435-443`：submitSell/Buy 返回 rr 只查 .rejected 打 warn，随后无条件 `hedged_qty += hedge_qty`——rate_limited/self_trade_blocked/localids 空（引擎失败）四种情况全部记成"已对冲"，累计达 original_qty 后 erase(:453) 该 pair 残腿失去后续保护且无重试。对比 executor 异步路径 A2 已改为受理判定+有限重试。
- `:214/:262/:274` markLegRejected(pair, order.qty) 存失败腿自身手数，`:421-424` 直接用它封顶**另一合约**的对冲量，ratio≠1 时错配（r1=1,r2=3 例：leg1 10手失败 → leg2 成交30手只能回补10，裸 20 手）。今日开仓路径出厂即放行 B-3 open；首个非 1:1 ratio pair 即触真金敞口。
- 建议：① rr 受理判定补齐、失败不累加并转有限重试（或复用 orphan 队列通道）；② markLegRejected 携带换算后的计划对冲手数；③ TestUnits 补 ratio=3 双向封顶断言。

**P2-7 B5 过冲冷却未约束 open 通道 —— "冷却期抑制一切信号"契约只在 close 兑现**
- SpreadArbitrageManager.h:405-406 契约写明 applyB3Gate 检查冷却抑制一切信号；实际 open 分支(:585-711)全程无 isInOvershootCooldown 检查，只有 close 分支(:521)有。过冲熔断后下一信号周期(~5ms) open 门照样放行 derived 反号后的大 gap 重建仓，放大刚被熔断的敞口。出厂 arb_close=false 时连坐过滤使其休眠，**C1 灰度第一步即激活**。
- 建议：open 入口加同样早退 + 单测"冷却内 open 必 NONE"。

**P2-8 StatisticalArb STOP_LOSS 分支数学不可达 —— 安全网静默缺位**
- StatisticalArbStrategy.cpp:73 composite_signal 先钳位 [-1,1]；:198 判据 `abs_sig > stop_loss_threshold`（默认 1.5）恒假；configure(:105) 只接 max_position，Init(:135-152) 不读 statistical 子参数 → 无法通过配置降到 ≤1。选 SA 为主策略的 pair 无 z 类止损，只剩 TIMEOUT_EXIT。建议阈值默认 0.95 或钳位前比较原始值，statistical 子参数纳入 loadConfig（pairsTrading 节读入体为空的 P3-2 一并收口）。

**P2-9 FAK 止损腿被自成交改价钝化 —— 被动改价+FAK=秒死单，止损循环空转**
- AsyncArbitrageExecutor.cpp:400-443 自成交处置统一退价（BUY→min_sell-tick），不分 order_flag。STOP_LOSS 是 FAK(:491)，吃价目标 ≥ 我方 MM 挂单价高频成立（两 arb 合约同时被双边报价），退到自家 quote 后面 FAK 立即杀单 → 零成交；z 仍极端则周期重复直至行情越过 quote 屏障。簿记干净但止损保护失效。
- 建议：FAK(taker flag) 跳过被动改价改 pay-through 越我方 quote 一 tick；至少对该组合打显式 warn 观察。

**P2-10 超时/过冲清理的撤单原语仍是 pair 级 —— cancelByPair 通道盲撤**
- OrderRouter.cpp:218-259 以 `_oid_to_pair==pair_id` 收集后全撤；timed_out(:390)/overshoot(:399) 消费点均整对清场。账本释放已是通道级（A3），但在途订单撤销没有等价粒度：close 止损在飞时 open 通道 120s 超时会把它一并撤掉（close 门不检查 open in-flight），重掀敞口窗口。概率低但正是 A3 要消灭的事故类别。
- 建议：cancelByPair 增加 is_close 过滤参数（registerPairOrder 时记录 order.is_close），清理路径按通道传参；短期至少在 AGENTS 固化"双通道并发在场为已知残量"。

**P2-11 bridge `_now_ms` 明域标量 Md写/Td读 + 墙钟 fallback 双时钟域混用**
- ArbExecutionBridge.h:101 普通 uint64_t，MdSpi onTick 写、TdSpi 路径(:430/:469/:248 等)读；多处 `_now_ms>0 ? _now_ms : TimeUtils::getLocalTimeNow()`。两域差三个数量级（exchange pseudo-epoch ≈1.75e15 ms vs wall ≈1.78e12 ms）：首帧前 Td 触及 fallback 会把墙钟基准写入 created_time/last_attempt，其后与 exchange 域相减必然"立即过期"，残腿保护提前失效。x86-64 下崩溃风险低，按本项目定级标准属待收编形式 UB。
- 建议：改 std::atomic<uint64_t> relaxed；删除墙钟 fallback 或统一哨兵值。

**P2-12 SpreadArbitrageManager::reset() 全仓零调用 —— 会话边界清理契约悬空（B7 失效）**
- Manager.cpp:846-882 实现（注释自证"B7 fix：否则 stale in_flight 冻结整个 session"）是死代码：RuntimeOps onSessionBegin 只调 bridge.resetSession() 与 orch.resetSession()，不经 manager。跨夜盘→日盘连续运行时 in_flight/intent/冷却/_last_signals 全部跨 session 残留，只剩 120s 超时单保险；四策略内部统计与 calculator EWMA 也不断崖。cron 双重启缓解了大部分窗口，但这是"写了防线没接线"。灰度 arb_close 前必修。
- 建议：onSessionBegin 接线 `_spread_arb_manager->reset()`（live 并发注意挂 Session 域 PendingCommand 随 drain 执行）。

**P2-13（挪入风控）** 见下组。

#### 执行/风控链

**P2-13 resetDaily 在错误锁内清空 SPSC 频控环 → 日切竞态**
- FutuRiskMonitor.cpp:676-678 `_order_times/_cancel_times/_trade_times.clear()` 在 `_halt_domain_lock` 内执行；生产者持 `_rate_lock`（recordOrder/Cancel/Trade :29-98）。RingBuffer clear 是裸 relaxed store head/tail（LockFreeRingBuffer.hpp:67-70）→ arb 线程/TdSpi 写与 Md 日切清环并发可撕裂环形不变式。附带 checkRiskLimits:256-258 prune 后 size() 读也在锁外。
- 建议：ring 清理改持 `_rate_lock`（嵌套固定序 halt_domain→rate，无反向获取，安全）；size 读挪进临界区。

**P2-14 OrderRouter 撤单侧缺 B+ 同款"重试/zombie"处理（不对称放大器）**
- OrderRouter.cpp:177-191/:193-209/:218-258 三处撤单一律标记+发送即完，无 ack 超时重试；条目仅在 onOrderDone(:278-303) 移除。静默撤单失败场景：记录永久滞留且因 pending_cancel 排除规则从所有 count/getActiveOrders 消失——是 P1-2b 的放大器，也是 `_order_source_map`/`_oid_to_pair` 泄漏源。tracker 侧 B+ 已修到位，Router 侧不对称。
- 建议：条目加 submit_ts+超时升级日志/补发撤单（复用 zombie 语义），或 broker cancel 同步失败立即回滚 pending_cancel 标志。

**P2-15 RiskLiquidator 冒用 Source::CLOSEOUT（A9/P2-3 同类缺陷最后漏网）**
- RiskLiquidator.h:96-105 reduceContract 固定 CLOSEOUT；调用方 IRREVERSIBLE 强平（RiskCoordinator.cpp:222）与通道恢复 AUTO REDUCE（FutuRuntimeOps.cpp:558-563）。影响：(a) 污染 closeout 统计与 executor 的 drain/settle 门（会把风控强平在途误当 closeout 在途）；(b) AUTO REDUCE 终态落入 Orchestrator.onOrderEvent（仅 isCloseoutFlattening 弱保护）；(c) Fix4 REVERSIBLE 豁免集合口径失真。
- 建议：改用已有 Source::RISK_REDUCE 或新增 LIQUIDATION；HALT 清扫清单同步补扫。

**P2-16 arb 部分成交的 pending 投影不对称**
- partial 成交只有 MM 路径有 updateOrderQty（FutuQuoter.cpp:721）；arb 单仅 recordFill 累加 filled_qty，qty 不动直到终态 untrack（UnifiedOrderTracker.h:408-423/638-650）→ 建仓期全源 pending 高估（保守方向：force_obligation/util 提前触发）；closeout/risk_reduce 单不入 tracker 使"T4 全源 pending"注释口径不完全成立。建议 bridge.onTradeFill 对 trackArbOrder 单补 updateOrderQty；文档写明 closeout 除外口径。

#### 性能设施/配置体系

**P2-17 PerformanceMonitor 启用后读侧跨线程读非线程安全 RingBuffer（休眠地雷）**
- calculateStats 迭代 history（PerformanceMonitor.cpp:94-148），调用点在 MdSpi processTick（StrategyCoordinator.cpp:619/:626）；而这些延迟 buffer 的写入方在 TdSpi（FutuRuntimeOps.cpp:202 / UftFutuMmStrategy.cpp:774）。Share/RingBuffer.hpp:13 明示 thread-unsafe。收官⑥的"单写者"论证只覆盖写侧，遗漏 Md 读者。当前 usePerformanceMonitor=false 休眠；一旦打开 p99 读到撕裂 size/head（数值脏读+形式 UB，TSAN 必报）。
- 建议：摘要输出移到写入侧线程属主，或 history 加 spin lock/双缓冲快照；最低限在 setEnabled(true) 注释声明约束。

**P2-18 loader 对数值参数无 finite 校验，NaN 可穿透全部范围检查**
- 全部校验形如 `<=0 || >20` 双向比较（FutuConfigLoader.cpp:210/:258-266/:312 等）——NaN 两支恒 false 全部放行；YAML `.nan` 经字符串 atof 即成 NaN（R5 注释自证路径）。热参路径已有正确范式 `!(val>=lo&&val<=hi)`（FutuHotParamManager.cpp:324）但 loader 未对齐。同类缺口：protectTicks 加载期零校验（NaN/0 静默关价格软保护）、obligationMaxSpreadTicks 与 skewCrossMaxTicks 交叉关系仅注释约定、closeout 三档深度比序(passive<mid<aggressive)未验、performance.warnThresholdNs 负数截断成巨大值。
- 建议：loader 统一 finite 范式 + 最小边界表；protectTicks/深度比序/maxSpread 交叉补 error 级校验。

**P2-19 src↔dist 配置漂移三类（应急调参未按流程回同步）**
- diff 结果：coordinator.yaml requoteAfterFillMinIntervalMs **src=500 vs dist=200**、cancelRetryIntervalMs **src=1000 vs dist=300**（08-26 修改只落 src）；hotparams.yaml sticky_threshold **src=2.0 vs dist=1.0**。dist 是 Debug 回测部署目标——现状使 08-25/08-26 生产参数组合无法在回测侧复现，恰是 HOTPARAM-DRIFT 机制要防的分叉。
- 建议：一次性收敛 `cp src/WtFutuCore/config/{coordinator,hotparams}.yaml dist/WtBtFutu/`；并在 AGENTS §3 流程追加"改 src 应急值当轮同步 dist"硬约束。

**P2-20 TSAN 设施缺失 —— 去大锁收官清单的硬步骤无法按描述执行**
- WtFutuCore/CMakeLists.txt:38-53 只有优化选项，无 `-fsanitize=thread` 通道，也无 `-Wall -Wextra`。而收官清单要求"TSAN 构建跑 TestUnits 全量"。只能手工另建目录改 flags，脆弱易错。
- 建议：加 `CMAKE_BUILD_TYPE=TSAN` 透传给 WtFutuCore+TestUnits；顺手开 warnings 观察一轮（顺带暴露死代码族）。

**P2-21 TscClock 重校准接口零调用 + 校准态非原子（死接口携带 torn-double 隐患）**
- TscClock.h:105-107 `_ns_per_tick`(double)/_calibrated 均 plain static；maybeRecalibrate(:55-64) 会睡眠 10ms 后写 double，toNs 由 MdSpi 每 tick 调（StrategyCoordinator.cpp:613）。全仓 grep maybeRecalibrate 除定义外零调用——AGENTS R5 记录"新增接口待接线"与现状不符。未来接入即 torn-double。另：未校准兜底 0.4 ns/tick 隐含 2.5GHz，现代机器低估 ~30% 延迟刻度。
- 建议：`std::atomic<double>` relaxed；maybeRecalibrate 要么删要么真正接线；兜底系数注明局限。

---

## §4 代码整理建议

### 4.1 死代码/NO-OP 清扫批
1. **NO-OP if 块三连**：StrategyCoordinator.cpp:939-941 / UftFutuMmStrategy.cpp:755-758 / FutuRuntimeOps.cpp:188-191（R3 空壳清理的漏网点）。
2. **GLFTResult.pause_quoting/glft_vol_contrib 零消费者但文档声称接线**：SpreadOptimizer.cpp:204-217 赋值无读取；CONFIG.md/coordinator.yaml 把 "GLFT pause mult≥max×0.9" 列为毒性三层响应之一——文档与实现不一致。二选一：在 processQuoting 消费该标志，或修订文档。
3. **PortfolioContext.contract_delta_util 已死**（C2 后唯一写点 SC.cpp:1071-1074 零读者，真输入是 realized 口径）：删字段 + 勘误 SpreadOptimizer.h:245/.cpp:39 两处注释。
4. **arb 侧重复实现与残字段群**：SpreadCalculator::calculateCorrelation/calculateBeta（:171-289，~120 行与 computeCorrelationAndBeta 重复且零调用）；PairsTradingStrategy CointegrationResult/`_last_signal_time/_entry_zscore`；StatisticalArb `_prev_zscore/_prev_correlation` 等；SpreadFeatureVector/StrategyWeights 结构；SpreadRiskManager::_last_alert_time。
5. **配置陷阱**：spread_arbitrage.yaml 顶层 `maxTotalPosition: 20` 零消费（生效的是 risk_limits.maxTotalPosition:50），同名双键数值不一致——删死键并更新 CONFIG.md 死键清单。
6. **信号层占位群**：ISignalCombiner/LinearCombiner/Registry（create 零调用）；ISignalSource::updateWithContext 空默认；TradeFlowSignalSource::onTrade SSOT 主路径无喂入口；VolatilitySignalSource `_vpin_weight` 死字段+reset() 不清统计埋点；OFISignalSource cum_sum 死变量+ofi_window>128 时 RingBuffer 满环覆盖绕过弹出（SendCumulativeOfi 发散隐患，建议构造期钳制）；TickTransactionInferer pruneHistory 无符号下溢（午夜回绕清空历史）+三访问器零消费整套统计白算——建议整体删除或修判据后再消费。
7. **Live-death 接口**：tracker shouldCancelDueToRate/getStats()、UnifiedTrackerConfig.stp_min_price_gap/stp_allow_same_price（装配了但 checkSelfTrade 只读 stp_enabled——配了不生效，最误导）、StpConfig.allow_same_price/price_adjust_ticks/strategy 零消费、PERF_TIMER/ScopedTimer（宏粘贴缺陷 `_##__LINE__` 不展开 + high_resolution_clock 与 rdtsc 方向不符）、OrderRouter.getRateCounter 引用逃生口、EventDispatcher（零订阅占位，随 WS-C 决策存废）、SessionPhaseManager.phase 组合方法零消费。
8. **std::string 热路径杂项**：tc.code/pctx.code 每 tick 构造、_touch_active set 每次 insert+erase、coordinator 四张 string-key map（V9 §4 已立案低优，维持）。

### 4.2 注释/文档勘误批（docs-only）
- adaptiveParam 仍被列为"三个模块级开关"之一（FutuConfigLoader.cpp:159-160、FutuModuleAssembler.cpp:128-129）——R3 已整删，照注释找开关会扑空。
- UftFutuMmStrategy.cpp:724/:733/:742/:806 五处引用旧宏名 **FUTU_CALLBACK_LOCK**（现行 FUTU_CB_LOCK_BIG）。
- FutuHotParamManager.h:6/.cpp:252 "**26 个**参数" → 27。
- README:760 "p99/p999" → p999 别名已删（PerformanceMonitor.h:38-45）。
- CONFIG.md 示例值三处与 src 漂移（随 P2-19 一并更新），"dist 值"列改名"示例值（快照）"。
- AsyncArbitrageExecutor.h:203-208 "MAIN-THREAD only no locks needed" 与 R6 加的 _oid_pair_lock 矛盾。
- README 行号锚点批量校正清单（§2.2 h:498、§4.1 成员 +8、§3 on_tick +6~22、§4.12 +7）——两文档尾注已声明免责，一次 docs commit 收敛。
- 半衰期单位勘误：estimateHalfLife 返回单位是配对样本数(updates) 非 seconds（SpreadCalculator.cpp:362-409 vs SpreadArbitrageTypes.h:182 注释），README/CONFIG.md 标注典型样本速率防调参错配。
- QuotePolicyChain L2 ColdStart 从 mid 重算会覆盖 L1/L2 已施加拉宽——语义可辩护但顺序是无文档隐式约定，补一行说明。

### 4.3 小额健壮性批
- submitExitLong/Short 缺 price<=0 守卫（OrderRouter.cpp:119-175，对比 :48/:89 有）；checkRateLimit 先扣预算再 STP 拦截（:56-67，被阻塞单消耗限速额度——调换顺序）。
- UnifiedOrderTracker pendingCancelTimeoutMs=0 整体关闭 B+ 重试/zombie 无启动告警（:371 仅 >0 门）——对齐 sticky_threshold=0 的防呆口径。
- 大单阈值比较算子统一 `>=`（TickTransactionInferer.cpp:363 vs MarketDataContext.cpp:162/:224）。
- TrendFollowing RingBuffer 容量 128 < slow_ma_period 可配上限 → configure() 校验无声失效。
- RiskCoordinator:146 恢复日志节流取墙钟与其余 replay 时钟不一致（日志节拍类）。
- FutuHotParamWatcher start 先 spawn 后置位窗口（先置位再 spawn）；解析持久失败 1Hz error 无限刷屏（连续 N 次降频）。
- tryResumeFrom(RISK_HALTED) 后门显式断言拒绝，固化转移矩阵。
- handleLeadLagPush static 调试计数器删除（V9 已立案未做）。
- LeadLag 三时间域并存（actiondate×1e9 / book actiontime / wall）——B5 aging 启用前必须先定义 anchor 陈旧的度量域（午夜回绕 age 为负 fail-open）。

---

## §5 功能完善建议

1. **套利灰度回测获得最小覆盖**（针对三盲区）：
   - 新建 `configbt_arb_grey.yaml`（基于 _ec_5d + arb_close.enabled=true + stop_loss/timeout_exit 打开），双腿 mock 至少产生一轮 STOP_LOSS/TIMEOUT/残腿事件；
   - B-3 gate/executeSignal 二层精判/自成交改价拉出纯函数表驱动单测（gate 输入输出皆 POD，test_v8_r4b 已示范直接构造 Manager 可行）；
   - 部署 runbook 增加 "useAsyncArbThread 必须 true" 检查项（src 权威 coordinator.yaml:22 目前 false，整文件直推远程会静默改变执行拓扑——AGENTS §5 部署清单不含此项核对）。
2. **测试护栏补缺**（按价值排序）：
   - QuotePolicyChain 六策略表驱动（LimitPrice L0-L3 / ColdStart skew 符号 / FillRetreat 保守价 / ArbCloseSync kConflict 双侧）——V9 §五.4 立案至今未落地，现有测试仅 ToxicityPolicy 一个触点；
   - FutuQuoter / RiskCoordinator(checkRisk 十步) / Closeout 三件套 / BilateralQuoteStats 直测（stats 纯逻辑极好测）；
   - LockFreeQueue 满空边界+SPSC hammer、RecursiveSpinLock 递归计数/owner 契约直测；
   - **回归判据脚本化**：笔数 ±1.5% 统计带 + 0 HALT/zombie/error + Delta=0 自动判定脚本（读 trades.csv/Runner.log），替代每轮手工 grep——当前最大人力消耗点。
3. **clip close-only 开关**（随 P1-2 落地）：组合净额口径保留为默认，提供"宁可不过冲"开关供生产切换，附单测锁定两种口径。
4. **alpha 预测性离线评估**：08-26 生产诊断遗留"conf 均值 0.24 偏低，预测性待离线 IC 分析"——落一个从 Runner.log SIGNAL_DECOMP 提取 per-source 方向命中率的小工具，回答信号是否有正 IC，决定权重框架是加权还是降权关闭。
5. **L2 数据管道决策（S9）**：先回测验证 depth_imbalance 与 transaction 推断相关性，再决定是否替换 TickTransactionInferer 口径（相关死代码群清除后重建成本更低）。
6. **告警分级完善**：RiskAlert 增加"同一事实状态翻转才重发"语义（value 进入/离开警戒带），取代纯时间窗节流（配合 P1-1）。

---

## §6 性能与低延迟优化建议

**保持优点勿退化**：F5 计时门控 / F8 exp 上提 / generation 门控快照 / FixedString24 SPSC / alignas(64) / rdtsc 全链埋点 / seqlock GLFT 参数 / 属主域命令通道（快路径 1 次 acquire load）/ 双边统计内存积压定点 flush。

**增量优化（行为中性或减负向）**：
1. P2-1 一行修复本身即是热路径减负——消除安静盘口的挂撤 churn（撤单流控与 tracker 开销双赢）。
2. 毒性 analyze() 每 tick 双算消除：updateSignals(SC.cpp:952) 与 ToxicityPolicy(QPC.h:223) 各算一次，链上下文复用前者的结果（挂 QuotePolicyContext），纯减负。
3. taker 冷却戳（P2-2）消除事故态下的 tick 频率路由尝试与多层 warn 字符串构造。
4. 双边 stats / DIAG 日志的 %采样纪律维持既有（SIGNAL_DECOMP %50 已对齐 toxicity）。
5. 已知低优遗留维持不动：code_idx 化四张 string-key map、_touch_active set——收益占比小，待 _ec_5d_perf.yaml + TscClock p99 埋点的正式 A/B 环境再做。
6. 去大锁收官（独立会话）：quoters 自旋锁持锁穿越引擎调用的持锁时长埋点量化（refreshQuotes p99 + TdSpi onOrder 阻塞计数）是切 none 的前置判据；必要时报单调用移出临界区（决策锁内/发送锁外）。

---

## §7 遗留与既有裁决项盘点（非本轮新发现，防失忆归档）

### 7.1 已裁决/进行中的结构项
- **去大锁收官**：WS-C TradingState 单写者收敛未执行；切 none 的四步验收（TSAN→_ec_5d big vs none 逐比特统计带→refreshQuotes 持锁埋点→生产灰度 p99/p999→删 _cb_mtx）未执行。设计完备（AGENTS 去大锁方案节），阻塞点是 P2-20 的 TSAN 设施。
- **sticky 判定 order_ids[0]×3**（FutuQuoter.cpp:338-343/:443-449/:482-488 vs 路径A 尾向扫描）：依赖"B1/B2 槽内 pendingCancel 同质性"隐式不变量；建议提取公共扫描函数三路径共用。
- **LegExecutionFSM 组件级收敛**：用户裁决不做拆分；本轮 P2-10/P2-7 是其具体功能实例，按功能性修复推进而非重启结构裁决。
- **PerformanceAnalyzer.determineMarketCondition 占位**：待 RegimeTracker。
- **CoordinatorConfig::_raw_variant wireDeps 尾部置空**（防御性，V9-P1-3）仍未实施。

### 7.2 框架层限制（禁止越界，持续推动上游）
- CTP 50/51/31 账户资源类拒单缺分类通道（08-26 复发实证 317 次；actpolicy 平昨优先是临时解）；`orderErrorThreshold=1000` 临时参数待回收。
- membin 容量无写保护（UftStraContext::on_order 越界 + memset 崩溃，08-24 事故）——复发风险随单日订单量回升存在。
- stra_cancel_all fullCode/stdCode 匹配口径回测/实盘不一致。
- WtUftEngine::on_session_end 生产不触发（WtUftTicker.cpp:183）。
- trades.csv 审计账与事件流不一致（BUY 丢失/SELL 重复）。
- HftMocker/UftMocker splitVolume 墙钟播种 → 回测无逐比特判据（判定法=统计带±1.5%+健康度）。

### 7.3 本轮复核通过的既往修复（防误报清单）
delta/position 语义分离、B+ 三段式闭环、WS-A halt 域收编全覆盖、D1 ERROR 单轨矩阵、RISK_REDUCE 区分（router 层）、TscClock lfence/invariant-TSC、热参 27 键边界+crossCheck+drift、GLFT 公式链（EMA/限幅/skew ticks 量纲/realized 口径）、毒性方向与 VPIN 有界、SPSC 四队列线程配对、ThroughputStats 原子化、loader fail-fast(contracts/anchor)。以上均有单测护栏或回测证据在案。

---

## §8 落地路线图

| 批次 | 内容 | 验证 |
|---|---|---|
| **R7-a 小步快跑（热修包）** | P2-1 requote 时间戳一行修；P2-2 taker 冷却戳；P1-1 前半（CORRELATION warmup 门控）+ CRITICAL 节流；P2-15 Liquidator 改 RISK_REDUCE；P2-13 resetDaily 锁序修正；P1-2 最小三项（结算 trade 静止门禁 + 删 last-resort 派发 + pending_cancel 计入 count） | TestUnits 相应新用例；v5 冒烟 0 HALT/zombie/error；_ec_5d 统计带健康度 |
| **R7-b 配置与设施卫生** | P2-18 loader finite 范式+边界补强；P2-19 src↔dist 收敛+流程硬约束；P2-20 TSAN option+warnings；P2-21 TscClock 原子化+死接口处置；§4.2 注释勘误批；CONFIG.md 同步 | cmake configure 变更注意 GLOB 重跑；TestUnits 全量基线不变 |
| **R7-c 套利安全包（arb_close 灰度前置）** | P2-6 同步对冲受理+ratio 量纲；P2-7 B5 open 早退；P2-8 SA 止损可达；P2-9 FAK 越价；P2-10 cancelByPair 通道过滤；P2-11 _now_ms 原子化+fallback 域统一；P2-12 manager.reset() 接线（走 Session 命令域）；P2-16 updateOrderQty 补齐；§5.1 灰度回测配置+B-3 表驱动单测+runbook 校验项 | TestUnits 用例（ratio 断言/冷却 open NONE/通道过滤）；configbt_arb_grey 至少一轮事件覆盖 |
| **R7-d 信号卫生包** | P2-4 跨间隙 guard+resetSession 接线+scale tracker emplace 修复；P2-5 fallback valid 门；§4.3 LeadLag 时间域决策；§4.1 信号死代码群清扫 | 间隙单测（模拟 2h gap 后首帧）；_ec_5d 统计带+TOXIC warning 行口径波动带 |
| **R7-e closeout 专项** | P1-2 其余两项（stra_get_local_position 锚定+close-only 开关；DRAIN 超时不重叠续跑）+ Router P2-14 撤单侧 B+ 化 | Closeout 三件套首建直测（模拟回报乱序/终态先行序列）；实盘收盘窗口观察 clip 序列 |
| **去大锁收官（独立会话）** | WS-C + none 四步验收 + _cb_mtx 删除单提交 | 按既定方案清单执行 |
| **持续** | §5 其余功能项、框架层限制推动、docs 批量校正随每一批搭车 | — |

**纪律提醒**：按 AGENTS §1 流程，任一批次动手前需先将实施方案细化写入 AGENTS.md 并经用户确认；验证判据一律使用"统计带 ±1.5% + 健康度指标"，不做逐比特承诺（UftMocker 墙钟播种已在案）。

---

*报告完毕。本报告由五路并行代码审查 + 主会话二次抽查生成，全部发现附 file:line 证据，可在工作树直接定位核验。*
