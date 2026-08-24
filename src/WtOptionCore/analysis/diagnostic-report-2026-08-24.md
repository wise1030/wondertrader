# WtOptionCore 综合诊断报告

**日期**: 2026-08-24
**Git HEAD**: 605c295e (fix(P0-P2): 定价链路修复+异步架构重构+实盘验证通过, 2026-07-18)
**方法**: 对照 2026-07-17 两份旧审计逐项复核（区分"已修/仍在/恶化"）+ 四路并行深读全部源码（引擎编排层亲自精读约 4,000 行，定价/风控/数据层由三个审计通道覆盖并经交叉验证），关键结论均附 `文件:行号` 证据。
**范围**: 架构 / 业务逻辑分层 / Bug / 代码整理 / 功能完善 / 高性能低延迟

---

## 一、总体结论

| 维度 | 评分 | 一句话结论 |
|------|------|-----------|
| 架构分层 | 7.5/10 | 组合式架构方向正确，模块边界清晰；但存在 God Object、死代码平行宇宙、scanner 空壳三类结构性问题 |
| 业务逻辑正确性 | 4.5/10 | 定价数学内核正确且多项旧 P0 已修；但"接线债"系统性存在——大量风控/增强模块编译通过、测试通过、生产空转 |
| 可实盘程度 | **不可直接实盘** | 存在 10 项可致崩/资金风险的 P0 缺陷（详见第三章），其中 4 项为本次新发现 |
| 性能 | C+ (68/100) | -O3/-flto 已落地；但 FAST 路径名不副实（每 tick 全量 Brent 求根）、timer 风暴等热路径问题仍在 |

**核心判断**：相比 07-17 审计，工程明显推进——SPSC 多生产者 UB（双队列重构）、total_risk 公式、hedge_ratio 配置断链、fee 注入、forward 粘性、OP2 主路径判空、-O3 编译均确认落地。但修复呈"补丁式"：只堵了报出的洞，没有解决上游根因。两颗新雷（tickSize=1.0 报价栅、到期日永久近似）都源自同一个未修的根因——**合约静态信息供给线断裂**。

---

## 二、旧账复核结果（对照 issue-log-2026-07-17）

### 2.1 已确认修复 ✅
| 旧编号 | 问题 | 证据 |
|--------|------|------|
| P0-A | SPSC 队列多生产者 UB | 双队列架构落地：MD→`_md_queue`(spsc)，trader→mutex vector（OptionAsyncEventProcessor.h:125-135, cpp 全文） |
| P0-B | OP2 单边 strike 空指针（主路径） | OptionPricer2.cpp:257-258, 323-324, 653-654 已判空（但衍生新雷，见 B01/B02） |
| P0-C | PositionOffsetMgr 在 OQM 拦截平仓报价 | OptionQuoteManager.cpp 全文无 offset 拦截逻辑 ✅ |
| P0-D | total_risk 公式漏加 delta_risk/vega_risk | CompositeOptionPricer.cpp:1539-1543 四项齐全 |
| P0-旧 | hedge_ratio_delta/vega、lambda_vega_* 配置断链 | CompositeOptionPricer.cpp:147-151 已拷贝（注释自证 "P0-D fix"）；消费端 :1292/:1401/:1420 闭合 |
| P0-旧 | fee 未注入 | OptionTradingGrid.cpp:84-101（期权）、:229-234（期货）已注入 |
| P0-旧 | isForwardReady 被无条件覆盖 | 粘性语义+超时落地（OptionGrid.cpp:652-703） |
| P0-旧 | tick 去重用指针地址 | 改 string_view 内容去重（OptionAsyncEventProcessor.cpp:315-319） |
| P1-旧 | Debug 构建 / 无 -O | -O3 -march=native -flto 已加（CMakeLists.txt:177-186） |
| P1-旧 | future tick_size=1.0 | UTD 侧已注入真实 tick/contractSize/feePct（OptionTradingGrid.cpp:220-235） |
| P1-旧 | 显式 teardown | shutdown() 已实现（HftOptionStrategy.cpp:53-63） |

### 2.2 仍然存在 ❌（本轮重新确认）
| 旧编号 | 问题 | 现状证据 |
|--------|------|---------|
| P1-B | 8/13 热更新参数不落盘 | on_params_updated 仅 log alpha 权重（HftOptionStrategy.cpp:1781-1785）；COP m_config 为构造期 const 快照，无 setter 通路 |
| P1-C | TPS 计数双重失真 | OQM::updateOrders 正常路径恒 return 1（OptionQuoteManager.cpp:211）；cancel_only 返回**累计** m_numCancel（:56）；窗口重置在 refresh() 尾部（ControllableTradingGrid.cpp:346-350）而非 drain |
| P1-D | updateGvvParams 空 stub → vol≡vol2 | OptionPricer2.cpp:231-240 仍是 `(void)exp; (void)curve;`；PeriodicCurveFitter.cpp:289-290 同一点集拟合两条曲线；`volcurve_weight` 数值无效 |
| P1-E | GammaScalp tick_size=minUnderlyingSpread | GammaScalpOptionPricer.cpp:159 原样保留 |
| P1-F | panic 双源覆盖 | HftOptionStrategy.cpp:1110-1112 每 compute 用 `_pricer->isPanicked()` 无条件覆盖，FillPriceChecker 的 panic 一个周期即被清除 |
| P1-G | secondaryHedgeCodes 仅解析不入聚合 | 只调 registerHedgeInstrument（HftOptionStrategy.cpp:779-783）；addSecondaryHedge 全工程零调用 |
| P1-H | frac_delta 临期衰减未配置 | setExpireDeltaFrac 仍零调用 |
| ARCH-1 | OP2⇄Fitter 空 deleter 循环 | 未变（受控） |
| ARCH-3 | 持仓 5+ 份拷贝无单一事实源 | 未变 |
| PERF-P1a~P1f | busy-spin/GVV eval/增量定价/getMaturity缓存/attrPub节流 | 全部未实施（仅 -O3 落地） |

### 2.3 恶化了 ⚠️
| 项目 | 说明 |
|------|------|
| Scanner 注册 | 旧审计："10 个只有 SpreadScanner 注册"。现状：唯一注册的 SpreadScanner.cpp 因 API 代际断层（调用已不存在的 getTradingDays()/getStrikes()/getStrike()）**被移出编译链**（CMakeLists.txt:69-80 无此项），在编 9 个 scanner 构造签名也与工厂 CreatorFunc 不兼容 → 工厂 map 恒空，任何 scanners 配置必然 "Scanner not found" |
| 死代码 | 新增化石：SpreadScanner.cpp(206)、test_optiongrid/test_blackscholes(643，引用已不存在的 API 无法编译)、config/option_strategy.json 与 option_config_demo.json（与现行 loader 异构） |

---

## 三、Bug 清单（按严重度）

> 标注：【新】= 本次新发现；【旧】= 旧审计已列未修。所有行号基于当前 HEAD。

### P0 — 崩溃 / 直接资金风险（实盘前必修）

**B01【新】updateRiskShiftsVega 对单边 strike 空解引用 → segfault**
- `CompositeOptionPricer.cpp:1338`: `if (!stk->call()->values(0).isPriced() || !stk->put()->...)` —— `call()`/`put()` 未判空直接解引用。
- 动态发现期间单边 strike 是常态（同文件 FAST/SLOW 主循环 :315/:403 都专门防了，此处漏防）。触发条件：配置 GT_vega_tw 且存在未定价合约（开盘必现）。
- 修法：`if (!stk->call() || !stk->put()) { ... } continue/break`。

**B02【新】PeriodicCurveFitter::fitToExpiry 对 call/put 无判空 → segfault**
- `PeriodicCurveFitter.cpp:155-159`: 取 `strike->call()/put()` 后直接 `call->getMid()`。入口为 timer→triggerDoFit→doFit，与 B01 同根因另一入口。
- 修法：循环首行 `if (!call || !put) continue;`。

**B03【新】OptionData.tickSize/multiplier 恒 1.0 → 全部期权报价落在非法价位**
- 根因：`setupGrid()` 中 `IBaseDataMgr* bdMgr = nullptr;` 无条件硬编码（HftOptionStrategy.cpp:507-508，注释声称实盘可取但没有任何获取代码，IHftStraCtx 也确无该接口）→ `__createOption` 的提取分支永不执行（OptionGrid.cpp:289-301）。
- 影响链：tickSize=1.0 → CompositeOptionPricer 四处 `round_to_tick_by_side(bid, tick_size…)`（:993/:1002/:886/:909 等）把报价取整到整数格 → ag(tick=0.5)/IO(tick=0.2) 全部拒单或劣化成交；multiplier=1.0 同步污染 PnL/费用口径。OTD 再把 1.0 传给 OQM。
- 修法（根因）：打通 IHftStraCtx→合约静态信息供给线。on_init 时对每个 option 用 `_ctx->stra_get_comminfo(exchange+"."+optionProduct)` 取 PriceTick/VolScale 注入 OptionData；同时删除误导性注释。cu(tick=1) 侥幸正确掩盖了此 bug。

**B04【旧·确认】ComboOrder 第二腿永远不会发出 → scanner 成交后裸腿**
- 设计上 `SpreadComboOrder::onFill` 负责"腿1成交后发腿2"（ComboOrders.cpp:94-105），但策略侧 fill 路由块是**空的注释循环**（HftOptionStrategy.cpp:1238-1247）。SynComboOrder.onFill 同样无调用点。
- 影响：一旦 scanner 子系统修复并产生信号，leg1 市价吃入后 leg2 永不发 → 单腿方向性敞口，做市引擎变赌徒。
- 修法：在 cbs.on_trade 中按 localid 匹配 `_activeCombos` 各腿并调用 `combo->onFill(...)`；补充超时反向兜底。

**B05【旧·确认】ToxicitySignal 时间源从未注入 → 约 11 笔成交后报价永久 widen 2x**
- `m_curTime` 恒 0：`onBatchEnd(){ checkExpiry(m_curTime); }` 自己传自己（RiskSignals.cpp:85-86）；fill 记录 time=0（:30）→ 窗口过期 `(now-front.time)>window` 永假（:70）历史永不清；全局速率超限后 `m_curTime > m_globalActionEndTime` 即 `0>300` 永假 → **永不恢复**（:89-99）。
- 修法：让策略在 batch 层把 ctx.time 注入信号（重写 onBatchStart 或加 setTime）。

**B06【旧·确认】ExpirationSimulator 四项 PnL 全部符号颠倒 + fee/time 字面量 0**
- ExpirationSimulator.cpp:38-42/67/72 买入记正、卖出记负（现金流向反了），`result.totalPnl`(:81) 与 printSummary 输出全为反号。
- 调用点 `_expSim->onFill(code, isBuy, price, vol, 0, 0)`（HftOptionStrategy.cpp:1251）fee=0 → 到期模拟日报不含手续费，且方向相反 → 基于它的绩效评估结论完全相反。

**B07【新】OQM 生命周期计数器永不重置 → MaxCancel/MaxNewOrders/hard_free 迟早永久锁死全部报价**
- `m_numCancel/m_numFill/m_numNewOrders/m_numReject` 无任何复位路径（grep 全工程确认；session begin 仅复位 QuoteStatistics，HftOptionStrategy.cpp:1397）。
- 这些计数器驱动 hard-flat（OQM.cpp:112-118）、拒新单（:121-125）及 RiskFilterChain 的 fctx（:149-151）→ 默认阈值（cancel_hard=200/new_orders_reject=200）下高频做市几小时内全部合约双向报价被永久拒绝，直到重启进程。
- 修法：on_session(begin) 对每个 OQM 调新增的 resetCounters()；如需日内控制应改滑动时间窗计数。

**B08【新】RiskFilterChain 的 MODIFIED 结果被调用方丢弃 → 所有截断类限制形同虚设**
- 链内确实改 `ctx.qty = ctx.modifiedQty`（RiskFilterChain.cpp:19-21），但唯一集成点只检查 REJECTED（OptionQuoteManager.cpp:152-154/167-169），随后仍用原始 bidQty/askQty 下单（:180-189）。
- 影响：配了 max_order_size 截断模式或 MaxPosition::MODIFY_TO_MAX 的用户以为有保护，实际全额报出。test_enhancements 只测了 filter 本身，没测调用方——单元测试全绿掩盖了 wiring 断裂（本报告反复出现的主题）。
- 修法：execute 后采纳 `fctx.qty` 再进 updateSide。

**B09【恶化·确认】Scanner 子系统三层失效（工厂空 + enable 反转 + 无驱动闭环）**
- 工厂恒空：见 2.3。
- enable 解析写反（HftOptionStrategy.cpp:1554）：`if (!sCfg->has("enable") ? sCfg->getBoolean("enable") : true) continue;` —— key 存在时条件恒 true → **显式启用的 scanner 一律被跳过**，key 缺失反而加载。次生 bug：:1580 `setEnabled(true)` 无条件覆盖 ：1559 正确解析的 config.enabled。
- 即使创建成功，CTG::refresh() 也从不遍历 m_scanners（仅 onUnderlyingUpdate/onOptionUpdate/onTick 空挂点被调）。
- 决策建议：要么投入一周做活（补注册宏+统一签名+enable 修复+fill 路由+taker 风控），要么整体移出编译。最忌维持"看起来有十个 scanner"的现状。

**B10【新】PeriodicCurveFitter 单点 map 双向迭代器 --begin UB**
- `PeriodicCurveFitter.cpp:211-239`: strike_vol 只有 1 个元素时，225 行 `--i_strike` 把 begin 减成 before-begin（UB），228-229 随即解引用；发生在 `<4 points` 早退检查**之前**。
- 修法：重写为单向遍历收集 upside/downside（现有双向指针杂技无必要）。

### P1 — 功能失效 / 链路断裂

**B11 OTD::getPosition() ≡ 0 → auto-close / QM_CLOSE 整条链路失效**
- OptionTradingData.cpp:95-103 依赖 `m_spOptionRiskData`（OTG.cpp:40-43 取了 rd 却**丢弃**，只有一行注释）或 `m_positionProvider`（setPositionProvider 零调用）。
- 下游全灭：COP.cpp:969 `pos!=0→QM_CLOSE` 永不触发；:974-983 CLOSE 态 pos 恒 0 → 立即翻回 AUTO；:1136 头寸感知路径恒 0；StrikeData/AttributePublisher/willBeActive 全部失真。
- 连带发现【新】QM_CLOSE 状态机自身有**死区死锁**：CLOSE 态下 `pos < -thresh` 才开 bid、`pos > thresh` 才开 ask（:980-983），当 `0<|pos|≤thresh` 时双边都不报价，而回 AUTO 又要求 pos==0 → 仓位无法通过交易归零。且 AUTO↔CLOSE 零滞回会抖动。修法：CLOSE 态改为按 pos 方向单边报价（thresh 只用于降 size），另加去抖计时。
- 主报价尺寸用的是 od->getPosition()（活数据），掩盖了此断裂——又是"主链路能跑、支链全死"的典型。

**B12【新】CTG panic 注释与代码矛盾：期货对冲腿也会被 cancel_only**
- ControllableTradingGrid.cpp:534-535 注释"In panic mode, futures still process normally (for hedging)"，代码却是 `utd->updateOrders(pq.isCancel || cancelOnly)` 与期权同样传 true → TPS 耗尽后期货腿被撤单，与 panic 保期货对冲的设计意图相悖。

**B13【旧】panic 双源覆盖**（见 2.2 P1-F）修法：OR 合并 `ctx->panicked = pricer->isPanicked() || fillChecker.isPanicked()`。

**B14【新】FillPriceChecker 整体失联（日志却宣称已初始化）**
- 记录委托价的唯一入口 `onOrderSent` 生产零调用 → m_issuePrices 恒空 → onFill 永远 early-return。executeQuote/executeOrder（HftOptionStrategy.cpp:1740-1762）成功后未回调。init 日志 :322-23 却声称已初始化。附带：条目只在撤单时清除，全部成交后残留泄漏。

**B15【新】RiskLimitsEx 五道预检防线零调用**
- checkPreTrade/checkGreeks/checkPostTrade 生产 0 个调用点（仅测试引用）；真正的下单路径直通 stra_quote/buy/sell。"Enhancement modules initialized" 日志纯属误导。maxLossPerDay 与活的 PnlLimitSignal 功能重叠。
- 决策：接入 OQM 过滤链之前，或删除模块避免虚假安全感。

**B16【新】PositionOffsetMgr 冻结量跟踪四处断线**
- ① onOrderSent（冻结唯一增量入口）生产零调用；② 撤单/成交解冻固定传 isCloseToday=false（:1289-1290/:1210-1212）；③ on_position 把 prevol/preavail/newavail 全部填成 newvol（:1431）——因为 enqueue_position 入口就丢弃了四元组中的三个（:1687）→ 今昨仓可用量语义尽失；④ getOrderBreakdown/checkDiscrepancy/syncLocalToBroker 生产零调用。该模块等价于一组不会被读到的缓存数。

**B17【新】PositionGuard 冷却失效 + 零容忍 → 启停抖动 / 永久停牌风险**
- setGetTimeFn 零调用 → now 恒 0 → 冷却退化为每次都告警；tolerance 默认 0 → "成交回报先于持仓回报"这一常态竞态即触发 disableOnBreach，broker 回报追平再自动恢复 → 高频下反复抖动；持仓回报丢失则永久停牌，且 reconcile() 无手工触发通道（热参命令未接）。

**B18 OptionRisk 组合 Delta 三处残缺**
- ① secondaryHedgeCodes 只注册不聚合（addSecondaryHedge 零调用）；② setHedgePosition 命中第一个同名 code 即 return（OptionRisk.cpp:144-158）而每个到期日都建独立 HedgeData → 多到期共用同一期货对冲时第二个起 delta 永久陈旧；③ `hd->multiplier = 1.0` 硬编码（:139）vs 期权侧按 contractSize 缩放 → 期货腿量纲差合约乘数倍。④ delta 对冲"只算不做"：portfolio_delta 无任何自动下单消费者（composite 主路径无对冲 executor）。

**B19 VegaFlow/DeltaFlow 信号 EMA 时间衰减失效**
- AlphaSignals.cpp:48/:86 以时间戳 0 更新 EMA → decay 恒 1 → 退化为终身累计和，window_sec 配置无效，输出无界。对比 RollEma/FrontFutSkew 用 ctx.time 是对的——同一文件内两种写法并存。

**B20【新】PnlLimitSignal panic 闩锁跨日不复位**
- m_action 一旦 Panic 无复位 API；信号对象只在 setupSignals 创建一次；pricer 每 compute 重设 panic 且 blackout 过期永远被下一次 compute 覆盖 → 一次触及日亏限额当日乃至后续交易日持续停牌（进程不重启时），hot cmd=3 只清一个周期。修法：session begin 时 sig->reset() + 提供手动解除。

**B21【新】quote-API 替换路径丢弃新 localid → 订单跟踪器与真实委托脱钩**
- OQM updateSide 中 quote_api 分支（:274-291 补量、:323-329 缩量重发）不保存 sendQuote 返回的新 id → 旧 OrderState 以旧 localid 残留 active=true → 后续成交/撤单按 localid 匹配 miss → rebuildOrderMarketTracker 高估在市挂单 → avoid_trade 失真（该不该交易判断错乱）。

**B22 TPS 机制双重失真**（旧 P1-C，见 2.2）+【新】cancelOnly 降级时 UT_UPDATE 也被撤单属预期，但 retainedDrops 重试携带的是采集时刻价格，重试时可能已陈旧（drain 用 otd 当前 desired 缓解，需确认）。

**B23【新】OQM TTL 自动撤单 / min_intra_update 限速 / 拒单重试 全部静默失效**
- TTL 与限速要求 m_getTime 非空，但 OQM::setGetTimeFn **零调用**（time_in_force_ms=45000 默认值形同虚设）；reject retry 设置 m_retryPending 后 getRetryDelayRemaining 无消费者 → 重试机制只有一半。

**B24【新】QuoteStatistics 订单数/延迟统计因 wasNewOrder 判定错误恒空**
- OrderState 创建时就赋了目标量（:279-280），wasNewOrder 要求 `o.qty==0 && totalQty>0`（:362-365）→ 恒 false → _ordersSent≡0 → fillRatio/cancelRatio 分母为 0。修法：加独立 acknowledged 标志。

**B25【新】rankOption/rankFuture 因子6 除零商溢出 int32（UB）**
- our_bid_spread = theo − bid 在我方 bid 高于 theo 时为负（做市抢占反抢是常态）→ `max(FP_EPSILON, 负)=1e-10` → 商可达 ~9e9 → int32 截断 UB（ControllableTradingGrid.cpp:408/:477）。修法：对 spread 取 `abs` 并施加半 tick 下限。

**B26【新】MaxPositionFilter 漏掉"翻越到对面仓位"**
- 增仓判定只查同向（RiskFilterChain.cpp:53-55）：多头 10 手卖 100 手（翻空 90）两个条件都不满足 → APPROVED 绕过限额。修法：以 abs(finalPos) > abs(currentPos) 为增仓判据。

**B27 组合单部分成交处理缺陷**
- SpreadComboOrder 首个部分成交即发腿2并发满额，此后尾量成交不再补对冲（ComboOrders.cpp:96-104）；SynComboOrder 部分成交即标 filled，剩余永不补发；期货腿整除向下系统性欠对冲（:267）。

**B28【新】到期日永久停留在 YYYYMM15 近似值**
- 精确到期日两条通路全断：(a) bdMgr 查 ContractInfo——bdMgr 恒 null（B03）；(b) WTSTickData 版 onTick 回填——生产走 TickDataRef 版无回填逻辑。daysToExpiry 最大偏差 ±14 天 → maturity/theta/discountFactor/临期排名加权全失真。国内期权实际到期日（到期月第4个周三）与 15 日常差 1-2 周。

**B29【新】午夜跨日 uint64 回绕击穿 sticky-forward 与 EMA**
- ctxTimeSeconds 为日内秒，夜盘过 0 点后 now < lastValid，uint64 减法回绕成 ~1.8e19 > 5e6 超时阈值 → 立即 setForwardReady(false)，cu/ag 等夜盘品种每天固定断供一次（OptionGrid.cpp:687-693）；EMAFilter 同样丢弃 secs<0 的更新（OptionValues.h:83）。修法：有符号差值或单调时钟。

**B30 GVV 拟合数据质量三连**
- ①【新】updateFitData 陈旧点永久堆积（PeriodicCurveFitter.cpp:314-339）：匹配依赖 forward 恒定（1e-9 容差），forward 漂移后旧点 mismatch 进新数据集且永不衰减 → 污染翼部拟合；
- ②【新】evalThresh 用函数级 static 全局表，多实例互相覆盖（:59/:128-132）；
- ③【旧】doFit_imp 恒 return true（:398-406）→ getLastFitTime 门控与日志语义失真；fitter 自带的事件 vector 全工程无注册者（死链）。

**B31【新】GVV eval 翼部求解失败静默塌缩到 1% vol**
- GvvVolCurve.cpp:166-171: Brent 求根超出可达域抛 "root not bracketed"，catch 吞掉后返回 `MINATMVOL/atmvol`（≈0.067 归一化）→ 深翼 vol 塌缩 1-2 个数量级、greeks 失真、**无日志**。修法：catch 返回 ATM 值 + LL_WARN 限频日志。

**B32【新】MD overflow 静默丢事件**
- OptionAsyncEventProcessor.cpp:257-265: events 满 1024 后 overflow 剩余条目被 `_md_overflow.clear()` 无条件销毁，且不计入 _queue_drops。（trader 批次回插是对的 :274-284。）

**B33 配置静默漂移（运维地雷）**
- full.json 的 `pricer.slow_compute_interval/trade_shock_interval/panic_blackout_interval/*_window/interesting_ticks_*/risk_prem_*` 等 11 个键 loader 完全不解析 → 实际生效默认 slow_compute_interval=15.0 vs 配置意图 0.1，FAST/SLOW 节奏相差 150 倍。
- orderManager 子键名一半对不上：配置用 max_orders_per_code/cancel_timeout_ms/ttl_ms/stp_enabled/reject_max_new_orders，代码读 max_side_orders/time_in_force_ms/max_cancels_allowed → 全走默认值。
- 建议：启动时做配置 schema 校验，未知键 WARN，缺失键 INFO 打实际生效值。

**B34 其他**
- IV 并行段 `#pragma omp parallel for` 无条件执行（OptionPricer2.cpp:423-426），不受 use_parallel_for 门控 → 配置项名不副实 + thread_local bci 各建一份；
- 【新】thread_local bci 缓存以裸 OptionData* 为键永不清除（:358）→ 地址复用时 C/P 与行权价错乱，网格重建场景必踩；
- 【新】LinearVolCurve::fit 空 dataset 即 UB（:125-131）；
- PerfCounter 是死表（start/stop 只设标志，m_totalTime 无赋值）；
- FutureOrderInfo::captureFill 把 order_done 标成 lateFill（语义污染）；checkTimeout 日志单位错（秒当毫秒）；
- 期权 PnlTracker 未设费率（只有期货设了）→ portfolioPnl 期权腿忽略手续费；
- PnlTracker initPosition 依赖首根 preClose>0 tick，成交先于 preClose 到达时以 lastfillpx=0 虚增一次；
- FilterContext.potentialPosition 传的是当前仓而非上面算好的 potentialPos（OQM.cpp:148/163）→ 双边联合上限未约束（保守向不一致）；
- OQM 撤单三层阈值与 RiskFilterChain MaxCancel 并存互不知晓；
- enable_quote_api=true 时 updateSide 依赖"交易所自动替换同价单"假设，非 replace 语义的柜台会同价重复挂单。

### P3 — 整洁 / 死代码（汇总见第五章）

---

## 四、架构诊断

### 4.1 分层现状与评价

```
IHftStraCtx 回调线程                    Async Worker 线程（一切业务逻辑串行于此）
  on_tick ──enqueue──▶ ┌────────────────────────────────────────────────────┐
  on_trade/order ────▶ │ AEP(双队列) → bucket sort → on_* callbacks          │
  on_position/channel ▶ │   ↓                                                │
                        │ HftOptionStrategy::setupAsyncCallbacks 的 7 个lambda │
                        │   ├─ OptionGrid.onTick（发现/快照/per-expiry路由）    │
                        │   ├─ CompositeOptionPricer.computeValues(FAST/SLOW) │
                        │   │    └─ OptionPricer2 + GvvVolCurve + BlackCalc   │
                        │   ├─ CTG.refresh(auto via listener) → rank → TPS     │
                        │   ├─ OQM.updateOrders（过滤链/STP/diffing）           │
                        │   └─ drainPendingQuotes → executeQuote/Buy/Sell      │
                        └────────────────────────────────────────────────────┘
```

**优点（值得保持）**：
1. **组合优于继承**：quantbox 三层继承被拆成 Grid/OTG/COP/CTG/OQM 组件，职责边界清晰、无循环依赖（仅 OP2⇄Fitter 一处受控空 deleter）。
2. **异步单线程纪律**：回调线程只 enqueue（~0.5us），全部业务在 worker 串行——这是正确的 HFT 架构，也事实性消解了大量数据竞态。
3. **抽象接口合理**：IOptionPricer/IVolCurve/ISignal/RiskFilterChain(IRiskFilter)/IScanModule 都是可扩展的点；GammaScalp 通过组合 OP2 复用定价、Signals 经工厂注册（7 alpha + 2 risk 全部健康）、过滤器链式短路——扩展成本低。
4. **风控注入方式正确**：过滤器/守卫以 setter 注入 OQM，pricer 不感知具体规则。

**结构性问题**：

| # | 问题 | 详情 |
|---|------|------|
| A1 | **God Object** | HftOptionStrategy.cpp 已达 1875 行；setupAsyncCallbacks 内 7 个 lambda 共约 560 行，on_batch_complete 单 lambda 约 175 行承担 position同步/PnL/scanner分发/attr收集/计算调度/combo超时/drain 七件事 |
| A2 | **静态信息供给线断裂（根因级）** | bdMgr=nullptr → tickSize/multiplier/到期日三大事实全部退化占位值。B03/B28/B06(fee名义价) 都是它的下游症状。不修根因，同类问题还会回来 |
| A3 | **约定线程安全而非机制安全** | OptionGrid 的 shared_mutex×2 残留为装饰（读路径多处裸读 m_expiries/m_atmFwdCache/m_listeners），安全性完全依赖"都在 worker"这一未成文契约；shutdown 从外部线程 removeListener 时 worker 可能正遍历 listeners。要么文档化单线程契约并删装饰锁，要么补齐锁 |
| A4 | **死代码平行宇宙（~7,800 行，30%）** | 详见 5.1。最危险的是 `wt_option::GvvVolCurve` 同名异构双定义（VolCurve.h vs GvvVolCurve.h，成员布局与常量都不同）——ODR 定时炸弹；且防护只存在于死版本（讽刺） |
| A5 | **三条下单路径风控不一致** | 做市单有 RiskFilterChain+Guard+TPS；scanner taker 单零风控直通 stra_buy/sell；gammascalp 对冲单零风控。至少给 taker 补 Guard（防打穿总仓位）、给对冲单补上限检查（不加 TPS，对冲不能等） |
| A6 | **持仓无单一事实源** | OD/ORD/OQM/Guard(internal+broker)/OffsetMgr(local+broker) 5+ 份拷贝，各自为政导致 B11 这类"某一份恒 0"的断裂难以察觉。建议 ORD 为准（唯一 write 点=addFill+broker 同步），其余从它读 |
| A7 | **Scanner 子系统是有骨架没器官的系统** | 接口/工厂/事件总线/执行通道齐全，但在编 9 个是同一份 edge=|theo-mid| 代码复制 9 次的占位物，唯一真实现被踢出编译（B09） |
| A8 | **wiring 系统性缺失（本报告主题）** | FillPriceChecker/RiskLimitsEx/PositionOffsetMgr/OQM-TTL/Toxicity 时钟/combo onFill/secondary hedge——七个模块"编译通过、测试通过、生产空转"。根因：异步化改造时丢掉了 order-sent/position-四元组/信号时钟这三类事件，且没有 wiring 级测试兜底 |
| A9 | **职责越界残留** | OptionGrid 持 m_fitter/m_optionPricer（数据层持定价层）；ExpiryData::m_hedgeUTD 裸指针对交易层（当前析构顺序碰巧安全）；OTD/UTD 的 executor hooks 期权侧活了、期货侧仍死 |
| A10 | **时钟方案二义** | pricer 链靠 setTime 三级接力传播（忘一级即调度假死），GammaScalp 用 steady_clock——两套方案并存待统一 |

### 4.2 README/文档过期对照（用户特别关注）

| README 声称 | 实际情况 |
|-------------|---------|
| "OptionData: 10档行情" | 异步队列 TickData 只拷贝 L1 五个字段（AEP make_tick），深档从未进入系统 |
| "lock-free SPSC队列" (文件清单注释) | 已是 md_spsc + trader_mutex **双队列**（正文线程模型一节是对的，文件清单注释过期） |
| "Tick 去重 (unordered_map<const char*>)" | 已改 string_view 内容去重 |
| riskFilters 配置字段表 (max_order_size 等) | ✅ 一致；但 orderManager.option 子键表 (ttl_ms/max_orders_per_code/cancel_timeout_ms/stp_enabled…) 与代码读取键名**不符**（B33） |
| Hot-param 13 参数表 | 仅 max_tps/command/qmode/manual_order 生效，alpha 权重/sticky/improve_retreat 只 log（B 旧 P1-B） |
| "RiskLimitsEx/FillPriceChecker/PositionOffsetMgr 风控增强" | 三者生产链路零接线（B14/B15/B16） |
| Scanners 10 种可配 | 工厂恒空 + enable 反转，子系统整体不可用（B09） |
| WtOptEngine/WtOptContext 列为"引擎层 legacy 组件" | 实为不在编译链的死代码；OrderManager/BaseOrder/OptionOrder 同 |
| "pricer 配置含 slow_compute_interval 等" | loader 不解析这些键（B33） |
| tests: test_ctg_ranking/test_enhancements | test_ctg_ranking 名不副实（测的是 check_markets/MultiMarket/EMAFilter 等数据原语，rankOption/TPS 一行没测）；核心定价链零测试 |

---

## 五、代码整理建议

### 5.1 死代码清理（合计 ~7,800 行，30%，一次 PR 完成）
**物理删除**（不在编译链或纯死）：
- ODR 雷（最高优先）：`VolCurve.h/.cpp`（内含同名异构 GvvVolCurve/ConstantVolCurve/LinearVolCurve 第二套）、`CurveFitter.h/.cpp`
- 未编译化石：`Black76.h/.cpp`、`BlackScholes.h/.cpp`、`StandardOptionPricer.h/.cpp`、`tests/test_blackscholes.cpp`、`tests/test_optiongrid.cpp`（引用已不存在 API）、`Scanners/SpreadScanner.h/.cpp`（API 代际断层）
- 编译但不可达：`OptionPricer.h/.cpp`（make_shared<OptionPricer> 全工程零处）、`OrderManager.h/.cpp/_Append.cpp`（~1,355 行，_Append 是 AI 会话残留）、`BaseOrder/OptionOrder`（仅死代码实例化；若保留 ComboOrders 则抽公共基类）、`WtOptionStrategy.h/.cpp`(1,337)、`WtOptEngine.h/.cpp`(475)、`WtOptContext.h/.cpp`(1,134)、`WtOptTicker.h/.cpp`、`OptStrategy.h`、`OptionTypes.h`、`ScannerInfo.h`
- 化石配置：`config/option_strategy.json`、`config/option_config_demo.json`（与现行 loader 异构）
- 死成员/死接口：COP::continueComputeValues_SLOW（还含未判空解引用，谁调谁崩）、QM_*_val 五常量、COP::getVol（无视 strike 恒返 ATM 的误导 API）、CTG m_optUpdateSet/m_udlUpdateSet 写而不读、OptionData 双缓冲（唯一调用者在死文件里）、addDataListener 死链、OTD::getPosition 断链（修 B11 后复活）、UTD 三个 hooks、PerfCounter 死表、ExpiryRiskConfig::init 三处重复默认值合一

### 5.2 结构整理
1. **拆 HftOptionStrategy**：7 个 lambda → 具名私有方法（handleBatchComplete 分出 syncPositions/updatePnl/dispatchScanners/scheduleCompute/manageCombos 五步）；config 读取抽 ConfigLoader。
2. **拆 CompositeOptionPricer.cpp（1747 行）**：computeOurMarkets / risk_shifts / alpha_signals / fast_slow_scheduler 四个文件。
3. **OptionGrid 去 m_fitter/m_optionPricer**（数据层不应持定价层）；IOptionPricer 拆 ITheoPricer + IQuotePricer。
4. **日志统一**：GvvVolCurve fit 失败走 std::cout、OptionRisk.cpp:133 std::cout → WTSLogger + 限频；static s_fwdDiag/s_slowDiag 改 atomic + trace_level 门控。
5. **CMakeLists**：`-march=native` 从发布构建移除（dist 跨机部署 SIGILL 风险），Release 用 `-march=x86-64-v2` 或显式 ISA 清单；IV 并行段纳入 use_parallel_for 门控。
6. **命名/口径**：orderManager 配置键与代码键名对齐（或加别名兼容）；checkTimeout 日志单位修正。

---

## 六、功能完善建议（按业务价值排序）

1. **合约静态信息供给线**（根因修复，解锁 B03/B28）：on_init 枚举 commInfo->getCodes() 建立 code→ContractInfo 映射注入 grid（tick/multiplier/到期日/费率一次性解决）；顺带删除 holidays.json 手工解析改用框架日历。
2. **事件总线补全**（解锁 B04/B14/B16/B05/B19）：AsyncEvent 增加 OrderSent/Acked 事件；enqueue_position 携带 prevol/preavail/newavail 四元组；SignalContext.time 统一下发。
3. **Session 生命周期管理**（解锁 B07/B20）：统一的 onSessionBegin 钩子遍历复位 OQM counters / signals 闩锁 / expSim / reject-retry。
4. **Scanner 子系统生死决策**（B09）：建议先删出编译止血，再按需求决定是否重做；重做必须包含 fill 路由 + taker 风控 + 部分成交处理。
5. **Delta 自动对冲 executor**：OptionRisk 已算出对冲需求但无人执行；在 GammaScalp 之外给 composite 主路径提供可选的对冲执行器（带 Guard 上限、不带 TPS）。
6. **热更新参数贯通**（旧 P1-B）：COP 加 applyAlphaWeights/applyStickyParams；或热参数独立为 atomic 成员绕过 const config。
7. **QM_CLOSE 修复 + hysteresis**（B11）：CLOSE 态按持仓方向单边报价，thresh 降 size 不禁双边；AUTO↔CLOSE 加 N 秒去抖。
8. **配置 schema 校验**（B33）：启动时未知键 WARN / 生效值 INFO 打印，杜绝静默漂移。
9. **测试补齐**（当前最大缺口是 wiring）：优先补 `check_markets→refresh→drain TPS` 集成测试、`__getBestSyntheticPrice` 粘性/超时/午夜回绕单测、OQM 生命周期（quote 替换/localid 更换/session 复位）、filter MODIFIED 采纳。引入 gtest 挂到 TestUnits。
10. **GVV 参数化曲线**（旧 P1-D）：curve1 从 config 读 spotvol/rho/volvol/alpha（setParameter 接口已在），fitter 只 fit curve2 → 让 volcurve_weight 真正生效。

---

## 七、性能优化建议（低延迟专项）

按 ROI 排序：

| # | 措施 | 位置/证据 | 预期收益 |
|---|------|----------|---------|
| 1 | **FAST 路径真正 fast 化**：现在 settleFrac<1 分支使每 tick 对全部合约全量 Black 重定价 + 每 option 一次 Brent 求根（≤100 迭代 pow/log/exp）（COP.updateDistortValues:608→OP2:581-584；GvvVolCurve eval）。恢复 quantbox 语义：仅 dF 线性更新（OP2:599-603 代码已存在只是被短路）；GVV eval 闭式化/预查表 | 定价热路径 | **数量级**（预计 cycle -70%~-90%） |
| 2 | **Timer 风暴治理**：on_tick 每 tick enqueue 一条 Timer（HftOptionStrategy.cpp:1650-1654），bucket 内逐条执行 on_timer——N ticks = N 次 sections 扫描+triggerDoFit+drainPendingQuotes per batch。改 100ms 周期合成定时器 | AEP+策略 | batch 开销 -N 倍 timer 体 |
| 3 | updateRiskShiftsVega O(N²) 且预热期（任一腿未定价）每 batch 满跑（:1334-1339 置 flag 与优化初衷相反）→ 限频 5s + 按 expiry 分桶预聚合 | COP | 预热期消除尖刺 |
| 4 | Worker 唤醒：cv wait_for(100us) → spin(pause)+cv 混合 + SCHED_FIFO + 绑核；has_events() 恒真死函数删除 | AEP:243 | 唤醒 <1us，消 P99 尖刺 |
| 5 | getMaturity/intradayFraction 每 expiry 每 cycle 预算一次（现每合约每 SLOW 周期走 TimeUtils::getCurMin→localtime tz 锁） | ExpiryData/COP:1466 | FAST -0.3~-0.6ms |
| 6 | attrPub collect 随 publish 节流（现每 batch 全量 collect 1816 合约即使 publish 被 1.5s 节流） | HOS:1052-1055 | -0.4~-0.9ms/batch |
| 7 | clearLastTrades 每合约两次 map find（COP:917/1617）→ onFill 打时间戳批量清理 | COP | -100us/cycle |
| 8 | ExpiryRiskConfig 热路径 5-8 次 find/option → computeOurMarkets 开头取一次 erc 引用透传；read 路径 `[exp]` 兜底插入改 find | COP:851/697/1506/1519 | -数十 us/cycle |
| 9 | internalId 已分配无处使用（OptionGrid.cpp:313）→ PendingQuote/attrPub/thread_local bci 全部改数组索引，消灭 string key 哈希 | 全路径 | -50~-200us/cycle |
| 10 | on_tick 回调 `std::string(code)` 临时串（AEP:324）→ 传 string_view/const char* | AEP | 微 |
| 11 | OpenMP 一致性：IV 段纳入 use_parallel_for 门控；thread_local bci 裸指针键迁入 OptionData | OP2:358/423 | 消潜伏雷 |
| 12 | mimalloc/tcmalloc + PendingQuote 对象池；-ffinite-math-only 评估（保 NAN 哨兵语义需谨慎） | 构建 | -30~-80us/cycle |
| 13 | BlackCalc 缓存 logK/F、sqrtT 合并公共子式；补 stdDev<eps 守卫后删头注释里的 QuantLib bug 自曝 | BlackCalc | computeValue -30% |

---

## 八、行动路线图

### 第 1 周 — 实盘阻塞清零（P0）
1. B03 根因：合约静态信息供给线（tick/multiplier/到期日/费率）
2. B01/B02 两处单腿判空；B10 迭代器 UB
3. B07 session 复位 OQM counters；B08 filter MODIFIED 采纳；B26 翻仓漏洞
4. B09 scanner：先移出编译止血 + enable 反转修复
5. B06 ExpirationSimulator 符号/费用；B12 panic 期货腿矛盾
6. B29 午夜回绕（改有符号差值）

### 第 2 周 — 风控链路通电（P1 核心）
7. B04 combo fill 路由 + 部分成交；B05/B19 信号时钟注入
8. B11 getPosition 供给 + QM_CLOSE 死区/hysteresis；B13 panic OR 合并
9. B14 FillPriceChecker onOrderSent 接线；B16 OffsetMgr 四元组接线（或明确降级删除）
10. B21 quote-API localid 更替；B23 TTL getTime 接线；B24 wasNewOrder 标志
11. B17 Guard 冷却/容差/手动 reconcile

### 第 3-4 周 — 结构清理 + 性能兑现
12. 第五章死代码一次性删除（ODR 雷优先）；HftOptionStrategy/COP 拆分
13. 第七章 #1/#2/#3 三大热路径改造
14. B30/B31 GVV 拟合数据质量；B33 配置 schema 校验
15. 第六章 #9 测试补齐（wiring 测试体系）

### 持续
16. A5 三路径风控统一、A6 持仓单一事实源、A10 时钟统一
17. scanner 子系统重做决策、delta 自动对冲 executor、GVV 参数化曲线

---

## 九、结语

这个项目的抽象设计是合格的甚至优秀的——组合式组件、异步单线程、可插拔 pricer/filter/scanner 的扩展点都对。它的问题不在设计图，而在施工验收：**七个模块处于"编译通过、单测通过、生产空转"状态**（FillPriceChecker、RiskLimitsEx、PositionOffsetMgr、OQM-TTL、combo onFill、Toxicity 时钟、secondary hedge），说明缺少 wiring 级验收手段；两颗新雷（tickSize=1.0、到期日近似）说明补丁式修复没有触及"静态信息供给线"的上游根因。下一步的最高杠杆动作依次是：① 打通合约静态信息供给线；② 补全事件总线并建立 wiring 测试；③ 对 scanner 子系统做出"做活或删除"的决断；④ 让 FAST 路径名副其实。做完这四件事，该项目才具备与其架构设计相称的实盘资格。
