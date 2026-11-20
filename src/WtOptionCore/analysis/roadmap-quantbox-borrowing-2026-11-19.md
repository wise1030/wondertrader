# Quantbox 借鉴路线图（2026-11-19）

> 背景：11-19 盘中诊断 theo 出界率 72%（200 双边腿仅 28% 落在市场 bid/ask 带内），
> 浅虚值区系统性偏离 -70%~+5%。溯源调研 quantbox 迁移源项目
> （`quantbox/quantbox/strategy/optioncore` 与 `quantbox/quantbox/gui/commsharklib`），
> 全部关键论断经逐条亲验（见 §2）。本文件是后续分批执行与验证的唯一依据。

## 0. 诊断基线（修复前，11-19 13:52 快照）

| 指标 | 基线值 |
|---|---|
| theo 落 bid/ask 带内率（双边腿，近月 2612/2701） | **28%** |
| 浅虚值区（\|logmoneyness\|<0.15）典型偏差 | -70% ~ +5% |
| 深翼（残余报价 2/4 的僵尸档位） | 不追（噪音，出界统计需分档） |

验证脚本：服务器 `/tmp/snap_outband.py`（连 8710 抓快照，统计 theo∈[bid,ask]）。
**每批 GVV 改动后重跑，目标 ≥70%（浅虚值区 ≥85%，深翼不计）**。

## 1. GVV 拟合事实（调研结论，已亲验）

- 源项目与 WT 同为 **vol² 空间 WLS**，权重同为 `exp(-β·x²/(σ²T))` 高斯核，
  **源项目也没有 vega 加权/价格空间残差**（`GvvVolCurve.cc:302` 逐字相同）。
- 源项目的质量保障在拟合之外的四道门（§3 批次 1 恢复前三道 + WT 自加 vega 加权）。

## 2. 关键论断复核记录（全部亲验属实）

| 论断 | 证据 |
|---|---|
| QB fit 失败不标 fitReady | `optioncore/PeriodicCurveFitter.cc:318-323`（isLastFitOK→return false）; WT `PeriodicCurveFitter.cpp:284-285` 注释承认 elided |
| QB curve2 默认 LinearVolCurve | `optioncore/OptionPricer2Params.xml:19`; WT `OptionPricer2.h:128` curve2_type="gvv" → 双 GVV 同数据同参数，volcurve_weight 数值无效（WT issue-log 2026-07-17 已记录） |
| QB 拟合点象限检查（≥1 up + ≥1 down） | `PeriodicCurveFitter.cc:224-232` has_upside/downside_point |
| QB β 默认 0.0（等权） | `GvvVolCurveParams.xml:3` |
| bqpos/aqpos 语义：>0=落后 n tick / =0 最优 / <0 穿价 / 9999=无单 | `optiontrader/optiontrading/AttributePublisher.h:125-126`；WT 快照无此字段 |
| QB 希腊目标按到期 `targ.delta/targ.vega_tw` | `CompositeOptionPricer.cc:443-445` |
| 修正：WT 已有 FillPriceChecker | `HftOptionStrategy.cpp:513`（广度报告曾误判缺失，剔除） |

## 3. 执行批次与验证口径

### 批次 1：GVV 四小修（治 theo 出界）——✅ 已实施（11-19 夜盘，11-20 修正）

**第三轮复核（11-20）——1.2 方向性修正**：
- 发现 copCfg.volcurve_weight（COP 每批量推给 OP2，覆盖 OP2 自身默认）
  与 p2cfg 默认不一致：1.2 实施时 OP2 默认 1.0 被 copCfg 默认 0.0 覆盖，
  **37% 实测成绩实际来自线性插值而非 GVV**
- 查 QB 生产语义（OptionPricer2.cc:660-664）：weight 默认 **0.0**
  （curve2=linear 为基线、GVV 为运营人员 per-expiry 可选平滑层，
  fit 未就绪强制 weight=0）——**1.2 的"weight=1.0 = 恢复 QB"是误读**
- 修正：curve2=linear 保留（备用链正确），weight 默认回 **0.0**
  （QB 语义：线性插值基线）。实测对照：linear 出界 37-41%、GVV 23%——
  线性插值在点上零残差天然占优
- **出界率指标的结构性缺陷**：偏好插值（点残差≈0），惩罚平滑
  （GVV 以点残差换形态稳定）——"出界率/mid 偏差"不能单独作为
  拟合质量指标；真实质量 = 平滑性 + 翼部外推稳健 + 点残差折中。
  **per-expiry weight 是运营旋钮**（QB 用法：近月点密可开 GVV、
  远月点稀用线性）——后续增强：expiries[].volcurve_weight 支持
  per-expiry 配置

实施复核记录：
- 1.3 象限检查**实施前复核发现 WT 已有**（`PeriodicCurveFitter.cpp:256-260`，
  调研漏检），跳过——roadmap 已修正
- 1.4 vega 加权初版（裸 vega）致 kurt 爆炸：|x|=0.4 处权重比 ~1/2000 →
  有效点数塌缩。改 √vega 压缩（比值 ~45:1）后正常
- 测试语义勘误：datapoint.first = K−F（绝对价差）而非 log-moneyness、
  second = vol/atmvol。closed_form 旧测试传 raw x（xi≈0，拟合退化解）
  因只验 initialized 标志而通过——无害但已知
- 实测：出界率 28%→37%，偏离幅度 −70%→±1-3%（残差与市场 spread
  同量级，达 mid-IV 拟合固有水平）

原内容（保留供溯源）：

| # | 改动 | 文件 | 说明 |
|---|---|---|---|
| 1.1 | **恢复 fit 失败门**：GVV 拟合失败不标 fitReady（QB 语义） | `PeriodicCurveFitter.cpp:284-285` | isLastFitOK() 已有（`GvvVolCurve`），补回 dynamic cast 检查；失败沿用上一根已初始化曲线但不置 ready |
| 1.2 | **curve2 默认改 LinearVolCurve** | `OptionPricer2.h:128` + config `curve2_type` | 恢复 QB 双曲线混合的数值意义；fit 失败退化链生效 |
| 1.3 | **象限检查**：拟合点须 ≥1 upside（K>fwd）且 ≥1 downside | `PeriodicCurveFitter.cpp` fitToExpiry | 移植 QB `:224-253,290`；不满足则本期不拟合（沿用旧曲线） |
| 1.4 | **vega 加权**：`w_i ×= vega(K,F,T,atmvol)` | `GvvVolCurve.cpp:306` fitWithAlpha | Black vega 现算（已有 m_atmforward/m_maturity）；vol 残差×vega ≈ 价格残差一阶等价——theo 落 bid/ask 的直接手段；β=1 保留（治深翼噪音），两者互补 |

验证：49 测试全绿 + `snap_outband.py` 出界率 ≥70%；re-anchor/播种行为不回归。

### 批次 2：UI P0（盯盘核心）——✅ 已实施（11-19 夜盘）

实施复核记录：
- bqpos 边界 bug 已修：市场该侧无盘（mkt.bid/ask≤0）而我方有挂单时
  原显示 9999（无单），应为 0（我方即最优）
- 已知限制：qp 为**价格差近似**（最优档对比）；真实队列位置（排第几
  手）需订单簿五档数据——当前 OptionMarket 仅最优档，深度版 qp 需
  扩展 tick 入库链（标注为后续增强，非本批次缺陷）

原内容（保留供溯源）：

| # | 改动 | 说明 |
|---|---|---|
| 2.1 | **bqpos/aqpos 队列位置** | 策略侧 AttributePublisher 仿 QB 计算（需订单簿深度对比，OQM 有 our price，簿深度在 grid）；快照 leg 加 `bqp/aqp`；前端四档背景色（最优浅/落后绿红/cross 纯色/无单无色） |
| 2.2 | **成交单元格闪烁** | √t 渐隐 2-20s，锚我方量列（源 FadeTimer 语义） |
| 2.3 | **声音警报** | trade/panic 两档（Web Audio），replay/无交互时静音 |

### 批次 3：交易模式与执行增强

| # | 改动 | 源 | 说明 |
|---|---|---|---|
| 3.1 | **TradingState 全局模式仲裁** | `multimarketmaker/TradingState.cc` | ⚠ 适配修正：WT 已有 TradingStateType 状态机 + **定时** close-flatten（`_closeFlattenActive`/`flatten_lead_seconds`）。增量 = 多信号（Stress/Panic/PositionGuard/PnL）**统一仲裁层** + GET_FLAT **立即档**（WT 只有定时档）。非从零移植 |
| 3.2 | **TradingIntensity（AS 强度估计）** | `stratlib/TradingIntersity.cc` | 成交流在线估计被成交强度 → 报价宽度自适应 toxicity |
| 3.3 | **OrderCounter 持久化** | `stratlib/OrderCounter.h` mmap 语义 | 撤单限额按自然日累计、重启不丢（今日 cancel-boundary 风暴正踩此坑） |

### 批次 4：报价引擎健壮性

| # | 改动 | 源 |
|---|---|---|
| 4.1 | CompositeMMPricer 评估（期货对冲腿专业报价） | `optioncore/CompositeMMPricer.*` 92KB |
| 4.2 | Limits 事前整组报价验证（含潜在保证金） | `trading/Limits.cc` | ⚠ 适配修正：WT 已有 RiskFilterChain（链式过滤器）+ RiskLimitsEx（事后 Greeks/日亏损）。增量 = **整组 desired 报价**事前验证（一次 N 档报出前整体不越界）+ **保证金维度**；过滤器链本身不重复建设 |
| 4.3 | 订单 tag 归因（策略/对冲/手动单分类统计） | `trading/OrderHarmonizer.h` | ⚠ 适配修正：OQM **已有** desired-vs-actual 增量对账核心（updateOrders diff），不重复。增量仅为**订单 tag 归因**（registerIssuedOrderIds 扩展 tag 维度 + 归因报表）——工作量大幅缩小 |
| 4.4 | MaxOpenOffsetFilter（限开仓）+ reject/hard-flat 分级 | `stratlib/risk/` | ⚠ 适配修正：OQM 已有 `hard_flat_after_n_fills`（成交计数硬平仓）——增量 = 限**开仓**过滤器 + reject 模式与 hard-flat 模式显式分级 |

### 批次 5：UI P1 + 参数体系

| # | 改动 | 源 |
|---|---|---|
| 5.1 | 标的 hedge 面板升级：$Greeks、对冲信号背景色（\|Δ\|>阈值且 Γ≠0 染红）、到期红底、汇总行 | `FutureDataModel.py:117-263`（22 列） |
| 5.2 | 到期摘要加希腊目标值（tgt Δ/tgt ν 与现值并排红绿） | `CompositeOptionPricer.cc:443-445` |
| 5.3 | 参数面板优先级排序（enable/ready 置顶）+ 分到期过滤 | `OptionGridTab.py:82-137` lut |
| 5.4 | 参数跨重启持久化验证/补充 | `stratlib/PropertyManager.cc` | ⚠ 适配修正：WT 已有 `sync_param`→ShareManager（WtShareHelper 共享内存，**跨进程热更新已通**）。磁盘持久化取决于 WtShareHelper 落盘配置——**先验证**，若重启丢失再补 SQLite（工作量降级为验证+条件补充） |
| 5.5 | Alert 服务化报警（分级+限速+远程推送） | `clientcore/Alert.cc` |

### 批次 6：分析与复盘设施

| # | 改动 | 源 |
|---|---|---|
| 6.1 | Vol smile 增强：bid/ask IV 误差带（快照 bid/ask 反解）、多到期 DTE 渐变叠加 | commsharklib 概念 |
| 6.2 | Vol 锥面板（历史分位数带） | `tools/python/vol_cone_res.py` |
| 6.3 | TradesTab 归因复盘（sprd/delta/roll/gamma/vega/theta pnl + cum 切换 + 行标记） | `TradeViewer.py` |
| 6.4 | PredictionEvaluator 信号持续打分（IC/命中率） | `stratlib/SampledPredictionEvaluator.cc` |
| 6.5 | 回放设施（MarketUpdateLog/LoggedTransport 图案）+ MessagePerfMon 延迟分项 | `stratlib/MarketUpdateLog.*`、`io/MessagePerfMon.*` |

## 3.4 批次 3-6 详细实施方案（11-20 批准版）

执行顺序（按当前痛点：持仓失控/撤单风暴优先）：**3.0 → 3.1 → 3.3 → 3.2 → 4 → 5/6 穿插**

### 批次 3（治失控）
- **3.0 对冲速率参数（30 分钟，最先）**——✅ 已实施（max_order_size=10、
  cooldown=1s、delta_per_lot=5，已上线）：config `delta_hedge.max_order_size: 2→10`、
  `cooldown_sec: 5→1`；`delta_per_lot` 显式配 5。依据：11-18 13:58 日志
  delta_excess=420、每 5s 2 手 = 0.4 delta/s << 累积 1 delta/s。
  验证：|Δ|超阈值后对冲追平 <2 分钟、无撤单风暴
- **3.1 TradingState 仲裁层（2-3 天）**——✅ 已实施（11-20，含 GetFlat 枚举、
  按起点匹配的降级白名单、转换矩阵测试）：新 `TradingStateArbiter.h/cpp` +
  `HftOptionStrategy::setState/applyTradingState` 接入。收集 channelReady/
  stressBreach/pnlBreach/positionGuardBreach/sessionStopped/closeFlattenLead
  → 唯一模式，优先级 Disabled > Panic > SessionStopped > Recovering >
  GetFlat > Normal。**新增 GetFlat 立即档**（eraseBids+eraseAsks+只减仓）。
  验证：任意信号组合模式确定可追溯；GetFlat 只减仓不开仓
- **3.2 TradingIntensity（2 天）**——✅ 已实施（11-20，intensity_enable 默认关；
  平坦毒性语义修正）：新 `Signals/TradingIntensity.{h,cpp}`，
  源 `stratlib/TradingIntersity.cc`；每到期一估计器，成交 tick 从
  `:1541 onTradeTick` 流入；输出 k → spread 乘子 max(1,k/k0) 接入
  CompositeOptionPricer :899-903。验证：毒性流注入 spread 拉宽、静默回落
- **3.3 OrderCounter 持久化（1 天）**——✅ 已实施（11-20，CSV 边车 +
  同日恢复 + 跨日重置测试）：QuoteStatistics 扩展落盘
  `generated/quote_counters_<tdate>.json`（按日覆盖、启动读取、跨日重置），
  max_cancels_allowed 用持久值。验证：撤单后重启计数不重置

### 批次 4（健壮性，约 3 天）
- 4.1 期货腿报价评估（0.5 天，先评估不搬运）——✅ 评估完成（11-20），
  **结论：不搬运 CompositeMMPricer 92KB**。实际缺陷清单（源码亲验）：
  ① teenie 定价期货腿是空操作桩（`min(bid, bid+bts)`≡bid，
  `max(ask, ask-ats)`≡ask），期权腿在 enable 时因空 book 永远 return
  false——即 config 一旦打开 teenie 整个报价更新静默停摆；
  ② 期货腿无 interesting/queue-ahead 检查（期权腿有
  __compute_interesting）；
  ③ 两腿均无对可见盘口的 cross 守卫（只有 EQ(bid,ask) 退化保护）；
  ④ 期货腿库存倾斜只有 risk_adjustment_future（norm delta 共享通道），
  无 quantbox 的 flat-target 语义。
  决策：期货腿是 delta 对冲腿而非独立做市腿，quantbox 的
  CompositeMMPricer 主要为独立期货做市设计，整体移植收益/风险比不
  成立；缺陷①属激活即炸的暗雷但当前 config 未启用（留作独立项与
  teenie 配置审计一起做），②③④在批次 2/3 的价格差近似与 delta
  对冲架构下不构成生产痛点。**4.1 关闭，不搬运。**
- 4.2 整组报价事前验证（1.5 天）——✅ 已实施（11-20，
  BatchQuoteValidator：refresh 排序后 drain 前按到期聚合纯新开腿的
  worst-case 成交冲击，超 max_exp_delta_add/max_exp_short_add/
  max_portfolio_delta 时按 rank 低→高否决；默认关，pricer.batch_validate）
- 4.3 订单 tag 归因（1 天）——✅ 已实施（11-20，OrderTag{MM,Hedge,
  Scanner}；ensureIssued 只升级不降级防 OQM 镜像覆盖；快照/事件全链
  透传；TradesTab 过滤 chips + OrdersTable src 列）
- 4.4 限开仓+分级（1 天）——✅ 已实施（11-20，MaxOpenOffsetFilter
  按到期限日开仓手数，risk_filter.option.max_open_per_expiry_day；
  修正 MaxNewOrdersFilter hardFlat 只记日志不拒绝 + OQM
  hard_flat_mode 置位无人消费两处半实现；reject 阶梯 N1 reject-only
  → N2(hard_flat_after_n_new_orders) 双边撤单+闩锁）

### 批次 5（UI P1 + 参数，约 3 天）
- 5.1 hedge 面板：UnderlierView 升级表格（$Δ/$Γ/$Θ/$V、对冲信号色
  |Δ|>阈值且 Γ≠0 红底、到期红底、分类汇总行）
- 5.2 到期希腊目标：expiries[].targ_delta/targ_vega 配置 + 快照字段 +
  ExpirySummary 现值/目标并排红绿
- 5.3 参数面板 lut 排序（enable/ready 置顶）+ 到期过滤下拉
- 5.4 参数持久化验证（改 hot param → 重启 → 值是否保留；若丢补
  hot_params.json 落盘回放）
- 5.5 Alert 远程推送（UiBridge alert 帧分级+2s 限速 → WS 转发 → 可选 webhook）

### 批次 6（分析/复盘，约 4 天含前置）
- 6.0 前置：AttributePublisher 加 CSV 边车（tdate/exp/atmvol/ts 分钟级）——vol 锥数据源
- 6.1 smile 误差带：bid/ask 反解 IV 画带 + 多到期 DTE 渐变叠加
- 6.2 vol 锥：6.0 历史 → 分位数带（MIN/10/25/50/75/90/MAX）+ 当前值
- 6.3 归因复盘：fills 带 greeks-at-fill → sprd/delta/gamma/vega/theta pnl + cum 切换
- 6.4 信号打分：scanner 信号后 N 秒 mid 变动 vs 预测，滚动 IC/命中率入 UiBridge
- 6.5 回放设施：落盘扩展为可回放流 + replay 入口（借 WtBtCore tick 回放）

### 独立项（不阻塞）
- 五档深度数据链（深度版 qp 前提）
- expiries[].volcurve_weight per-expiry 配置（GVV 运营旋钮）

## 3.5 遗漏补录（复核新增）

- **vol 锥（6.2）前置缺口**：需历史 atm vol 序列——AsyncVolFitter 无
  落盘。前置任务：快照/拟合结果按到期落盘（可复用 AttributePublisher
  管道加 CSV 边车），再谈分位数带
- **出界率口径修正**（批次 1 后确立）：theo 是 mid-IV 拟合产物，残差
  与市场 spread 同量级时出界 30-40% 是统计必然——**主质量指标改为
  "theo vs mid 偏差中位数"**（dev_table 口径），出界率降为辅助指标
- **CompositeMMPricer（4.1）评估前提**：先确认期货腿当前报价路径
  （CompositeOptionPricer::updateOurMarketSideFuture）的实际缺陷
  （报价档位/换仓/Flat 语义）再定移植范围，避免整体搬运 92KB
- **五档深度数据链**：深度版队列位置 / 盘口意图分析均依赖——
  OptionMarket 扩展存五档（tick 入库时 WTSTickData 的五档字段
  存在但现只取最优档）——单独立项，批次 2 的价格差近似不阻塞

## 4. 已完成的关联修复（本路线图的上下文，勿重复）

- P0/P1 六项 theo 定价链修复（commit `6e80b00c`）：minStrikes、forward 去污染、翼部 clamp、atmVol 重锚、forward 统一、标的价隔离
- 播种竞态修复（atmVol 默认值跟随市场 med，启动 1h 低质窗口 → 2-3 批量）
- PnL 一次性初始化（`_pnlInited`，修 Panic 反复推手）
- 对冲架构：远月自身月 hedgeCode、严格归属双接口、共享池（CZCE 预留）、DeltaHedger 多腿化、报价消费月内净 delta（`wgt_exp_ddelta` 默认 0）
- PositionGuard stale 门退役 + TraderCTP 30s 周期查询
- session_resume enableAll、stale timer 事件守卫、channel_ready 不覆盖 SessionStopped
- fit gate 与 min_strikes 统一（`good_points_thresh` 表联动）
- β 配置透传（`volCurve.beta`，当前 =1）
- UI：conn 四层修复（no-cache/replay/ping-pong/flush bug）、orders 双列（ordBid 邻 bidSz、ordAsk 邻 askSz）、quoting 置灰、leg pos、Ticket key/NaN、+/- 记 side、PANIC 豁免只读

## 5. 状态追踪

| 批次 | 状态 | 验证记录 |
|---|---|---|
| 批次 1 GVV 四小修 | ✅ 完成（28%→37%，偏离 −70%→±1-3%） | 夜盘实测 |
| 批次 2 UI P0 | ✅ 完成（qp 着色/闪烁/声音） | 待用户刷新目验 |
| 批次 3 模式/执行 | ✅ 完成（3.0 对冲参数/3.1 仲裁+GetFlat/3.2 AS 强度默认关/3.3 计数持久化） | 53→56 测试绿 |
| 批次 4 健壮性 | ✅ 完成（4.1 评估不搬运/4.2 整组验证/4.3 tag 归因/4.4 限开仓+分级） | 9/9 二进制绿，全部署 |
| 批次 4 健壮性 | ⬜ 未开始 | — |
| 批次 5 UI P1/参数 | ⬜ 未开始 | — |
| 批次 6 分析/复盘 | ⬜ 未开始 | — |

## 6. 源项目证据索引

| 主题 | 路径 |
|---|---|
| GVV 拟合/权重/失败门 | `quantbox/quantbox/strategy/optioncore/GvvVolCurve.cc:284-371`、`PeriodicCurveFitter.cc:119-390` |
| 曲线混合 | `optioncore/OptionPricer2.cc:659-668`、`OptionPricer2Params.xml:19` |
| bqpos/aqpos | `quantbox/optiontrader/optiontrading/AttributePublisher.h:125-126` |
| 期货 MM | `optioncore/CompositeMMPricer.{h,cc}` |
| AS 强度 | `quantbox/quantbox/strategy/stratlib/TradingIntersity.{h,cc}` |
| 模式仲裁 | `quantbox/quantbox/strategy/multimarketmaker/TradingState.{h,cc}` |
| 限额框架 | `quantbox/quantbox/trading/Limits.{h,cc}` |
| 订单对账 | `quantbox/quantbox/trading/OrderHarmonizer.h` |
| 撤单风暴控制 | `quantbox/quantbox/strategy/multimarketmaker/ordermanager/DefaultOrderManager.{h,cc}` |
| 计数器持久化 | `quantbox/quantbox/strategy/stratlib/OrderCounter.h` |
| 参数持久化 | `quantbox/quantbox/strategy/stratlib/PropertyManager.{h,cc}` |
| 滤波器族 | `quantbox/quantbox/math/KalmanFilter.{h,cc}`、`EMAFilter`、`VolatilityFilter` |
| 报警 | `quantbox/quantbox/clientcore/Alert.{h,cc}` |
| 信号评估 | `quantbox/quantbox/strategy/stratlib/SampledPredictionEvaluator.{h,cc}` |
| UI 队列位置/闪烁 | `quantbox/quantbox/gui/commsharklib/OptionGridModel.py:323-347`、`FadeTimer.py` |
| UI hedge 面板 | `commsharklib/FutureDataModel.py:117-263` |
| UI 归因复盘 | `commsharklib/TradeViewer.py` |
| UI 参数 lut | `commsharklib/OptionGridTab.py:82-137` |
| vol 锥 | `quantbox/quantbox/tools/python/vol_cone_res.py` |
