# WtOptionCore 吸收 quantbox/optiontrader 优秀设计的评估与升级方案

**日期**: 2026-08-24（Phase A/B/C 已于同日执行完毕，状态见 §六 执行记录）
**输入**: `.opencode/plans/quantbox_vs_wtoptioncore_analysis.md`（下称"对比plan"，撰写于迁移早期，部分结论已过期）
**验证基线**: wondertrader HEAD + 2026-08-24 修复批次（61 files, +568/−8707）之后的工作区
**原项目位置**: `/mnt/d/gf_pc/WonderTrader/quantbox/` — 框架核心在 `quantbox/quantbox/`（strategy/stratlib/risk、servers/tdcore、trading/、strategy/multimarketmaker/ordermanager），optiontrader 业务层在 `quantbox/optiontrader/optiontrading`

---

## 六、执行记录（2026-08-24）

**变更**: 41 files, +1,700 / −262（含 08-24 修复批次累计）
**验证**: 完整依赖树 Release 构建 ✅；test_enhancements **12/12 PASS**（新增 6 个 wiring 测试套件）；test_ctg_ranking 全绿

| 项 | 状态 | 关键落点 | 偏差说明 |
|----|------|---------|---------|
| A1 今昨仓记账+护栏 | ✅* | enqueue_position 四元组贯通；PositionOffsetMgr **绝对量语义修正**（原 prevol+newvol 双重计数为新发现 bug）；OQM `applyOffsetGuard` 平仓方向按可平量截断（`orderManager.*.enable_offset_guard`） | *onOrderSent 冻结簿记未接执行器：WT 回调无今/昨拆分，采用"全量记 close-today 的保守合并视图"+发单前预防性截断替代；getOrderBreakdown 保留待更细数据源 |
| A2 RiskLimitsEx 通电 | ✅ | OTG 级 PreTradeCheckFn 注入每个 OQM（filter chain 之前）；`checkRiskLimitsEx()` 每 compute 周期 Greeks/日亏检查 → `_limitsBreached` 锁存并入 panic OR 链 | — |
| A3 拒绝重试闭环 | ✅ | OQM 重试背窗抑制（backoff 内 diffing 不补发）+ 到期消费；CTG drain 尾部直驱 isRetryPending 合约（绕过 check_markets 盲区） | — |
| A4 quote_style | ✅ | OQM Config.QS_PAIRED/QS_BUYSELL + `sendSingle()` 抽象 + 可注入 SendSingleFn；BUYSELL 走 stra_buy/sell（offset 交框架 action policy） | — |
| B1 保证金护栏 | ✅ | `margin:{enable,fut_rate,opt_short_rate,max_margin,warn_ratio}` 配置驱动估算；5s 节流；超限锁 panic、超 warn 比 WARN；日志明示 estimated | — |
| B2 三竞态防护 | ✅ | 新增 OrderAnomalyGuard.h：unknown-fill 计数告警(3次升级)、DONE-before-FILL 分类、overfill 检测；executeQuote/Order 登记、on_trade 先分类（Unknown 只更新仓位/风险不进 OQM 簿记） | 未做 pending-late-fill 缓存表（分类即可满足当前路由需求，缓存表留待需要时加） |
| B3 short call/put 限额 | ✅ | OptionsShortLimitFilter（provider 惰性聚合全品种净空头；REJECT/MODIFY 截断）；FilterContext.rightFlag 由 OQM cfg 注入；riskLimits.maxShortCall/PutPerSymbol 配置 | what-if 用 FilterContext.potentialPosition 天然承载，未另建 simulate API |
| B4 跨月风险开关 | ✅ | COP config `use_cross_expiry_corr`（默认 true）；false 时 total_risk 剔除 delta_risk2/vega_risk2 与协变项——即 §5.2 要求的灰度回滚开关 | 二维收缩矩阵本体未引入：hedge_ratio/corr 参数链路已在 08-24 修复中闭合，先以开关管理口径，矩阵化推迟到有实测分布数据后决策 |
| B5 FIFO PnL + MTM | ✅ | PnlTracker 增加 FIFO deque 匹配（对手方头部配对实现已现）、加权入场价（带符号簿记，穿零自动重建基准）、dumpLedgerCsv；`pnl.fifo_mode` 配置经 OTG 应用到新建 tracker；非 FIFO 路径零改动 | 会话级全量 ledger 导出暂缓（逐合约 dump 接口已备） |
| C1/C2 Guard 增强 | ✅ | broker 过期冻结(staleFreezeSec)、clampLimit 钳制(broker 赢)、乐观调整+undoWindowSec 回滚（fill 解释差异时撤销） | — |
| C4 Phase 相位机 | ✅ | OrderState.Phase(New/Live/CancelPending/Dead) 与 bool 字段同步维护 | debug assert 断言未加（bool 兼容期保留），Phase2 清 bool 后收紧 |
| C5 计数器落盘 | ✅ | session end 经 OTG::dumpCountersCsv 输出 outputs_option/oqm_counters_&lt;date&gt;.csv | mmap 持久化维持不做（§3.3） |
| C3 多档 Harmonizer | ☑️ 关闭 | 按 §四 C3 的可选条款关闭，保留单档 diffing | MultiMarket 多级扩展启动时再评估 |

**新增测试**（test_enhancements）：OptionsShortLimitFilter(REJECT/MODIFY/链采纳)、quote_style 双腿+guard 截断+phase 迁移、pre-trade 阻断+重试背窗/到期恰好一次、FIFO 已现 PnL 手推对照(=38)+加权入场价穿零、Guard 过期/钳制/undo、AnomalyGuard 三分类。

**遗留提醒**：① SHFE buy_sell 路径需 SimNow 实测 action policy 拆分行为后再切生产配置；② margin/opt_short_rate 为估算参数，上线前按交易所公开参数校准；③ README 尚未刷新至本轮现实（列入 W6）。

---

## 一、结论摘要

对比 plan 的**问题识别基本准确，但完成度描述已系统性过期**：它列出的多项"重要缺失"实际上在迁移过程中或 08-24 修复批次中已经落地。以当前代码为准：

| 判定 | 数量 | 说明 |
|------|------|------|
| ✅ 已完成（含 08-24 新增） | 7 项 | 过滤器链、FillPriceChecker、持仓不一致检测、Cautious flipping、Scale factor、订单统计、拒绝重试(半) |
| ⚠️ 有骨架未通电 | 4 项 | PositionOffsetMgr、RiskLimitsEx、QuoteAPI 交易所差异、拒绝重试无消费者 |
| ❌ 真实缺口 | 12 项 | 保证金、IssuedOrderTracker 竞态处理、short call/put 限额、矩阵风险模型、FIFO ledger、MarkerToMarket、Harmonizer 多档位、状态机、多策略 claim、广播协调、因子模型、mmap 持久化 |

**总体完成度约 45%**。剩余工作的正确姿势不是"补齐搬运"，而是**用 WtOptionCore 已验证更优的架构载体（异步单线程 + 组合式 + 配置驱动）重新承载 quantbox 的领域逻辑**，其中三项需要根据 WT 框架现实做"降维改造"而非 1:1 移植（详见 §3.2）。

---

## 二、对比 plan 逐项复核（23 项）

> 状态图例：✅ 完成｜🟡 半成品/断线｜❌ 缺失｜📄 plan 结论过期

### P0 组

| # | 功能 | plan 判定 | 实际状态 | 证据 |
|---|------|----------|---------|------|
| 1 | PositionOffsetManager 平今/平昨/开仓 | 关键缺失 | 🟡 **类完整、链路断线**：getOrderBreakdown/getNumCloseableToday 接口+单测齐全；但冻结入口 `onOrderSent` 生产零调用、`on_position` 四元组仍传 newvol×4（HftOptionStrategy.cpp:1503）、解冻 `isCloseToday` 恒 false | PositionOffsetMgr.h/cpp |
| 2 | 头寸不一致检测 | 重要缺失 | ✅ **已完成**：PositionGuard 内部fill累计 vs broker 对比、阈值停牌、冷却告警；08-24 补齐 setGetTimeFn 接线 + broker/fill 双侧喂数。缺手动 reconcile 暴露 | HftOptionStrategy.cpp:905/1495 |
| 3 | TdRiskControl 预交易限制链 | 重要缺失 | 🟡 **RiskLimitsEx 已实现 10 项检查但零调用点**（checkPreTrade/checkGreeks/checkPostTrade 无生产调用）；账户/品种两级聚合维度缺失 | RiskLimitsEx.h/cpp |
| 4 | 保证金检查 | 重要缺失 | ❌ 无 | — |

### P1 组

| # | 功能 | plan 判定 | 实际状态 | 证据 |
|---|------|----------|---------|------|
| 5 | LocalPositionOffsetManager claim | 重要缺失 | ❌ 无 | — |
| 6 | PositionManager 矩阵风险模型 | 重要缺失 | ❌ 无（erc.corr_exp 跨月相关系数是雏形但只作用于 risk_shift 标量） | CompositeOptionPricer erc |
| 7 | FillPriceChecker | 中等缺失 | ✅ **已完成并通电**（08-24）：onOrderSent 记录委托价（:1830-1848）、成交偏离 Warning/Panic、panic 锁存 OR 合并、终结清条目 | FillPriceChecker.h/cpp |
| 8 | 拒绝重试 | 中等缺失 | 🟡 **半成品**：OQM 有 m_retryPending/getRetryDelayRemaining/400ms delay 配置，但无任何消费者触发 retryUpdateOrders | OptionQuoteManager.cpp:514 |
| 9 | IssuedOrderTracker 中央注册表 | 重要缺失 | ❌ 整体无；但其**三个竞态防护**（DONE-before-FILL、order-not-found、overfill）可在 OQM 层轻量补齐，无需搬注册表 | — |
| 10 | TdOptionsRiskControl short call/put | 重要缺失 | ❌ 无 position stacking what-if 能力 | — |

### P2/P3 组

| # | 功能 | plan 判定 | 实际状态 | 证据 |
|---|------|----------|---------|------|
| 11 | IOrderFilter 可组合链 | 重要缺失 | ✅ **已完成且优于 plan 描述**：RiskFilterChain 5 过滤器（3模式持仓、软硬撤单限、软限减仓放行、MODIFIED 截断采纳）。仅缺第 6 个 MaxOpenOffsetFilter | RiskFilterChain.h:47-91 |
| 12 | FillDequeLedger FIFO PnL | 中等缺失 | ❌ PnlTracker 仍是增量式 | — |
| 13 | MarkerToMarket 加权入场价 | 中等缺失 | ❌ 无 | — |
| 14 | PortfolioPositionTracker 过期检测/钳制 | 中等缺失 | ❌ 无（PositionGuard 有对账无过期检测） | — |
| 15 | 广播头寸乐观协调 with undo | 中等缺失 | ❌ 无 | — |
| 16 | OrderHarmonizer 归并 | 中等缺失 | 🟡 部分：OQM getMissingPriceLevelSize 是单档位 diffing；MultiMarket 仅支持 best 一档，多档位归并无从谈起 | OptionQuoteManager.cpp |
| 17 | TdOrderState 状态机模式 | 中等缺失 | ❌ BaseOrder 简单枚举（且属死代码边缘） | — |
| 18 | OrderStatisticsCollector | 中等缺失 | ✅ QuoteStatistics 回调驱动覆盖（双边时长/义务/延迟/成交撤单拒率/会话汇总） | QuoteStatistics.h/cpp |
| 19 | CtaRiskModel 因子风险模型 | 低缺失 | ❌ 无（建议维持缺失，见 §3.3） | — |
| 20 | 订单计数 mmap 持久化 | 低缺失 | ❌ 无 | — |
| 21 | Cautious flipping | 低缺失 | 📄 **plan 过期——早已存在**：OQM `cautious_flipping` 配置位 | OptionQuoteManager.h:62 |
| 22 | Scale factor | 低缺失 | 📄 **plan 过期——早已存在**：setScaleFactor 运行时缩放 | OptionQuoteManager.h:94 |
| 23 | 交易所特定报价 API | 中等缺失 | 🟡 enable_quote_api 开关在；缺 SHFE/CZCE"顶单"模式与 quote_id 约定差异处理 | OQM Config |

---

## 三、升级方案总纲

### 3.1 设计原则

1. **领域逻辑照搬，执行载体重造**：quantbox 的价值在风控语义与竞态经验，不在其 mutex+回调+boost 框架。一切移植件必须落在 async worker 单线程模型内，禁止引入新的锁竞争点。
2. **优先"通电"而非"新建"**：四个半成品（§二 ⚠️）的边际成本最低、收益立现，排在所有新移植之前。
3. **尊重 WT 框架分工**：凡 TraderAdapter/action policy 已下沉的能力（今昨仓拆分、localid 管理），策略层只做记账与护栏，不重复造轮子（详见 §3.2 三条降维决策）。
4. **每个移植件必须带 wiring 测试**：本仓库最大教训是"编译通过、单测通过、生产空转"。验收标准 = 单测 + 一条从事件源到行为改变的集成断言。

### 3.2 三条关键降维决策（与 quantbox 原设计的偏差声明）

**D1 — 今昨仓处理：不自拼 offset，改为"记账+护栏"**
quantbox PositionOffsetManager 自己把订单拆成 (CLOSETODAY/CLOSE/OPEN) 三段下发。而 WT 的 `TraderAdapter::buy/sell` 已经内置 action policy 自动按 closeToday→closeYesterday→open 拆分（TraderAdapter.cpp:680/970），`stra_quote` 则硬编码 OPEN（:1285-1299）。
⇒ 正确路径：① 商品期权做市在 SHFE 上走 buy/sell 分离路径（OQM 增加 sendBuySell 分支），offset 交给框架；② PositionOffsetMgr 保留但职能收缩为"可平量记账 + 冻结 + 超平护栏"，不再生成 offset 指令；③ quote API 仅用于支持双边报价指令的交易所（CFFEX 等）。

**D2 — IssuedOrderTracker：不搬注册表，只吸收三个竞态防护**
WT 已有全局 localid 与回报分发。中央注册表的注册/装饰器/遍历职责与 OQM 重复。
⇒ 只移植三件事进 strategy 的 on_order/on_trade 路径：(a) DONE-before-FILL → pending-position 缓存，FILL 后到时补记；(b) order-not-found fill → dummy 记录 + ERROR 告警（不 PANIC，避免误杀）；(c) overfill（remaining < fill_qty）检测告警。

**D3 — 矩阵风险模型：不引入 Eigen，做二维解析收缩**
NxN 合约级矩阵对期权做市过度（同标的相关结构本质上是"跨月相关性"一维）。
⇒ 用 expiry×direction 聚合的协方差修正替代：`net_risk(e) = own(e) + Σ_{e'≠e} ρ(|e−e'|) · own(e')`，ρ 用 erc.corr_exp/lambda_vega_decay 参数化（参数通道已在 08-24 修复中打通）。保留未来升级为因子模型的接口位（IRiskAggregator）。

### 3.3 明确不移植清单（附理由）

| 项 | 理由 |
|----|------|
| CtaRiskModel 因子模型 | 期权做市的风险由 Greeks 主导，因子收益估计适用于股票/CTA组合；投入产出比低，接口位预留即可 |
| Eigen 依赖 | 引入重型三方库违背 WonderTrader 轻量依赖传统；所需运算不超过 3×3 手写 |
| DefaultOrderManager 全量 1607 行 | 其中 ~60% 是 longbeach TradingContext/订阅体系粘合；有价值子集已列入上面各条 |
| 多账户/PortfolioServer/GUI/db/messaging | 架构代际不同，WT 有自己的监控与数据体系 |
| Lua fee provider (FillFeesDotLuaFeeProvider) | WT 用 commInfo.calcFee 统一口径 |

---

## 四、分阶段实施设计

### Phase A：通电（1 周，最高 ROI）

#### A1 PositionOffsetMgr 收缩重接线（对应 D1）
- `enqueue_position` 携带四元组 `(prevol, preavail, newvol, newavail)`，AEP AsyncEvent.position 扩展字段
- `cbs.on_position` 透传真实值；executor 成功发单后调 `onOrderSent(isBuy, qty, isCloseToday)`——isCloseToday 判定规则：`SHFE/INE 且 direction 为平且 newavail_today > 0`，从 `_positions` + on_position 的今仓字段推导（OTD 侧缓存 todayPos/prevPos）
- 解冻处传入真实 isCloseToday（替换两处硬编码 false）
- CTG 发单前消费 `getOrderBreakdown` 做**超平护栏**：breakdown.open > 0 且可用资金/开仓限不足时截断
- 验收：单测模拟"挂平仓单→未成交→broker 可平量不变"；集成断言 onOrderSent 后冻结量变化

#### A2 RiskLimitsEx 接入预交易链
- 注入点：OQM::updateOrders 过滤链之前新增 `ILimitChecker` 位（setter 注入，与 RiskFilterChain 并列）
- 检查顺序：RiskLimitsEx.checkPreTrade(price/qty/notional) → RiskFilterChain → PositionGuard
- Greeks/DailyLoss 检查挂到 on_batch_complete 的 compute 后段（非热路径），违规 → pricer panic
- 补账户级聚合：maxTotalPosition/maxLossPerDay 从 OptionRisk 聚合值读取
- 验收：test_enhancements 增加 wiring 用例（构造超限单 → 断言 executeQuote 未发生）

#### A3 拒绝重试闭环
- CTG::drainPendingQuotes 循环尾部：对 `isRetryPending && getRetryDelayRemaining()==0` 的合约追加一次 updateOrders(false)
- 重试次数上限沿用 m_rejectMaxRetries=3，成交后清零逻辑已有
- 验收：注入 mock executor 首次拒、二次成，断言重试恰好一次

#### A4 交易所报价差异最小集
- OQM Config 增加 `quote_style`: `paired`(默认) / `buy_sell`；SHFE/INE 商品期权配置为 buy_sell
- buy_sell 分支：sendQuote 拆为 `stra_buy(bid) + stra_sell(ask)` 两条独立单，各自 trackOrder（复用 B21 机制）
- 验收：单测断言两种 style 的下单调用序列

### Phase B：关键移植（2-3 周）

#### B1 保证金护栏（P0-4）
- 数据源：配置驱动 `marginConfig: { product: { margin_rate_open, margin_rate_short_opt } }`（交易所公开参数，无法从 commInfo 获得）
- 计算：`est_margin = Σ_fut qty×price×rate + Σ_opt shortQty×(marking + underlying×0.1×delta_abs...) ×rate`——采用交易所标准空头期权保证金简化式，标注为估算口径
- 挂点：RiskLimitsEx 新增 `checkMargin(estMargin, limit)`；on_batch_complete 每 N 秒评估一次，超 90% warn、超 100% 触发 pricer widen、连续 3 周期超限 → panic
- 验收：数值单测（给定持仓/价格断言保证金公式）+ 阈值触发用例

#### B2 IssuedOrderTracker 三防护（对应 D2）
- 位置：HftOptionStrategy::on_order/on_trade 入口处新增 `OrderAnomalyGuard` 小类（~150 行）
  - DONE-before-FILL：isCanceled&&leftQty==0 时若本地无此 localid → 存入 `m_pendingLateFills[localid]`（TTL 5s）；on_trade 先查此表再走正常路由
  - order-not-found fill：OQM onFill miss 时记录 ERROR 日志 + 计数器，10 分钟内 >3 次触发 StopQuoting 级告警（不自动 panic）
  - overfill：on_trade fillQty > totalQty−filledOfOthers → WARN + QuoteStatistics.onReject 对冲计数
- 验收：三类乱序事件的时序单测

#### B3 Short call/put 限额 + what-if（P1-10）
- 新增 `OptionsShortLimitFilter : IRiskFilter`：聚合同标的全部 short call / short put 净额（从 OptionRisk 按 expiry 遍历持仓符号），超限 REJECT/MODIFY
- what-if 能力：FilterContext 已天然是 what-if（potentialPosition），补充 `OptionRisk::simulateDeltaAdd(qty)` 常量时间估算供 filter 使用，不落盘
- 配置并入 riskFilters.option 节点
- 验收：构造 short call 超限场景断言拦截；stacking 不污染实际仓位

#### B4 二维风险收缩（对应 D3）
- OptionExpiryGreeks 增加 `adjustedDelta()/adjustedVega()`：读 erc 的 corr_exp/lambda_vega_decay/hedge_ratio_* 做跨月修正（参数链路 08-24 已闭合）
- CompositeOptionPricer risk_adjustment 改用 adjusted 值（feature flag `use_cross_expiry_corr` 默认 off，灰度切换）
- 验收：双 expiry 数值用例（ρ=0 与 ρ=0.7 对照）

#### B5 MarkerToMarket + FIFO Ledger（P2-12/13，合并且降配）
- PnlTracker 增加可选 FIFO 模式：bid/ask 两个 deque，对手方头部匹配实现已现 PnL；会话结束时 dump CSV（不做 mmap 持久化，CSV 足够复盘）
- weightedEntryPrice 并入 PnlTracker 字段（增仓加权、减仓递减、清零），AttributePublisher 增加两个属性输出
- 验收：交叉成交序列的手推对照单测

### Phase C：健壮性与体验（2 周，可与 B 并行）

#### C1 PortfolioPositionTracker 精华：过期检测 + 钳制
- PositionGuard 增强：broker 头寸 10s 无更新 → WARN + 冻结该合约新开仓（不减仓）；`|internal−broker| > clampLimit` 时以 broker 为准钳制内部值并告警
- C2 广播协调 undo：on_position 到达且与内部差 ±N 时，先暂存调整；2s 内有 fill → 回滚调整。实现在 PositionGuard（~80 行）
- 验收：乱序/丢失回报的时序单测
#### C3 多档位 Harmonizer（P2-16，可选，视 MultiMarket 多级扩展排期）
- 前置：MultiMarket 支持 levels[]（现仅 best）。先做数据结构，再移植双指针归并到 OQM::updateSide
- 若短期不做多档位，此项关闭，保留单档 diffing（现状已可用）

#### C4 状态机整理（P2-17）
- 不复活 BaseOrder 死类；在 OQM::OrderState 增加 `enum class Phase { New, Live, CancelPending, Dead }` + 合法迁移断言（debug 下 assert，release 下日志），替代散落的 bool 组合（active/cancelPending/acknowledged）
- 验收：非法迁移路径的单测枚举

#### C5 计数器持久化（P3-20 降配）
- resetCounters（08-24 已加 session 复位）之上增加会话级 CSV 落盘（session end 写出 numCancel/numNewOrders/...），满足交易所报送对账需求即可，不做 mmap

### Phase D：与既有技术债合并的主线路线图

| 周 | 主线 A（本方案） | 主线 B（08-24 报告 Phase 2 遗留） |
|----|----------------|--------------------------------|
| W1 | Phase A 全部四项 | scanner 子系统决断（建议移出编译止血） |
| W2 | B1 保证金 + B2 三防护 | HftOptionStrategy::setupAsyncCallbacks 拆分为具名方法（配合 A1/A2 注入点重构一次到位） |
| W3 | B3 short限额 + B4 二维风险 | 配置 schema 校验器（unknown key WARN） |
| W4 | B5 FIFO/MTM | FAST 路径线性化第一刀：GVV eval 闭式/查表 |
| W5 | C1/C2/C4 | timer 风暴治理 + worker busy-spin/绑核 |
| W6 | C5 + 文档（README 全面刷新至当前现实） | wiring 测试体系成型（每移植件一条集成断言入库） |

**里程碑验收**：W4 结束时达成"quantbox 风控语义覆盖率 ≥85%（除明确不移植项）"；W6 结束时空转模块数为 0（每个 RiskLimitsEx/OffsetMgr/Retry/统计都有生产调用点与测试证明）。

---

## 五、风险与回滚

1. **A1 今昨仓语义风险最高**（涉及真实资金合规）：buy_sell 路径先在 SimNow 验证 action policy 拆分行为，灰度开关 `orderManager.option.quote_style` 保留 paired 回退。
2. **B4 风险口径变更**用 feature flag 默认关闭，A/B 跑一个交易日对比 risk_adjustment 分布后再切。
3. **B1 保证金为估算口径**：日志明示 "estimated"，不得作为强风控唯一依据，仅联动 widen/panic。
4. 所有 Phase A/B 改动集中在 OQM/Strategy/PositionGuard/RiskLimitsEx 六个文件内，不动 Grid/Pricer 数学内核，保证定价链路零回归（以 test_ctg_ranking + test_enhancements + 新增 wiring 测试守护）。
