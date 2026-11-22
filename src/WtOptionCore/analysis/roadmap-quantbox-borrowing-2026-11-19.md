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
- 5.1 hedge 面板——✅ 已实施（11-20，HedgePanel：按到期 $Δ/$Γ/$Θ/$V
  表 + TOTAL 汇总行；|$Δ|>hedgeDeltaThreshold 且 Γ≠0 红底；DTE≤2 琥珀底）
- 5.2 到期希腊目标——✅ 已实施（11-20，expiries[].targ_delta/targ_vega；
  修复 m_prop_exp_{delta,vega_tw}_targets 死输入接线；快照 gamma/theta
  + targ 字段；ExpirySummary 绿/黄/红偏差着色）
- 5.3 参数面板排序+过滤——✅ 已实施（11-20，ExpirySelector
  enable+ready 置顶排序 + jump 下拉）
- 5.4 参数持久化——✅ 已实施并验证（11-20，SETPARAM 白名单落盘
  generated/hot_params.json；on_init 注册后/sync_param 前回放使持久值
  成为框架默认；实测重启后 "1 params restored"；command/qmode/manual
  排除防重启 footgun）
- 5.5 Alert 远程推送——✅ 已实施（11-20，UiEventType::Alert 分级
  0/1/2 + pushUiAlert 2s 限速；订单异常回调挂 level-2；前端 Toast；
  中间件 WTOPTION_ALERT_WEBHOOK 可选推送 level≥1）

### 批次 6（分析/复盘，约 4 天含前置）
- 6.0 前置——✅ 已实施（11-20，AnalysisRecorder：volseries/pfseries 分钟级 CSV 边车，按日分文件，60s 节流；analysis.{enable,interval_sec}）
- 6.1 smile 误差带——✅ 已实施（11-20，前端 Black-76 二分反解 bid/ask IV 画带 + fitted theo + DTE 渐变多到期叠加）
- 6.2 vol 锥——✅ 已实施（11-20，/api/series?tdate=recent 多历史 + 按到期分位带表 + 当前值；API 实测联通）
- 6.3 归因复盘——✅ 已实施（11-20，fills 携带 issue 时刻 greeks（registry ctx 透传）；per-fill edge + cum 切换；pfseries 组合希腊分解 Δ/Γ/ν/Θ 累计 vs 实际 pnl）
- 6.4 信号打分——✅ 已实施（11-20，PredictionEvaluator 加 scanner 收敛打分（theo-mid 方向 30s 验证，滚动 100 窗 Pearson IC）+ fwd30s hitRate 访问器；快照 pred 对象；前端 PredChips）
- 6.5 回放设施——✅ 部分实施（11-20，QstreamRecorder 可回放报价流 qstream_<tdate>.csv（默认关，analysis.qstream_sec）+ analysis/qstream_check.py 离线回放校验。引擎级 tick 回放（WtBtCore ParserX 数据源自举）留作后续项）

### 独立项（不阻塞）——✅ 全部完成（11-20）
- 五档深度数据链——✅ 异步断点补齐（TickData/TickDataRef 5 档数组 →
  OptionMarket B12 存储 → 快照 bd/ad（>1 档才编码）→ 前端 Σ5 徽标）。
  CTP 期权 feed 通常只 1 档，深度显示随 feed 自动出现
- expiries[].volcurve_weight——✅ per-expiry GVV 混合权重（默认 -1
  回退全局；setGvvWeightPerExpiry 覆盖）
- teenie 配置审计——✅（4.1 发现：空操作桩启用即冻结报价；构造器
  强制 OFF + ERROR 日志）
- 引擎级 tick 回放——✅ 端到端实测（qstream_to_dsb.py：RAW_V2 12 字节
  头，20 字节头会 RAW 透传错位段错误；dsb_roundtrip.py 52,439 tick
  往返 PASS；WtBtCore 无会话品种 skip+WARN 修复——原段错误；实测
  34.6 万 tick 铜期权日回放 enabled=true options=36 underlying=106620；
  操作手册 analysis/replay.md）

## 3.4b 六批次全面复核记录（11-20 深度审计）

**方法**：逐批次对照项目原有设施，查重复造轮子/次优路径/遗漏。

**修复（本轮已修+部署）**：
1. 批次 5.1 hedgeDeltaThreshold 编码错位——被编码在 risk 对象内而前端
   在顶层读取，红底判定静默退化为 thr=0。移至顶层（commit 835fb4d4）
2. 批次 5.2 targ_delta/targ_vega 未入 ConfigValidator 白名单（启动
   WARN 噪音）。补齐（同上）

**新诊断处置（11-20 待办已实施 #3/#4/#5，commit 6d9f3540）**：
3. ✅ 已清理：evaluateTradingState 输入收集移除（残余死代码，git 历史
   保留设计记录）；生产生效的 shouldAllowTransition 守卫保留
4. ✅ 已实施：OQM 计数同日重启恢复——发现项目原有 dumpCountersCsv
   （C5 导出）只有导出一半；补 OQM::restoreCounters + OTG::
   restoreCountersFrom（后写覆盖先写）+ session begin 接线；跨日无
   文件自然归零=交易所按交易日口径。max_cancels_allowed 门启用拼图
   完整（58 测试绿，新增 restore 语义测试）
5. ✅ 已对齐：analysis.outputDir 配置项（AnalysisRecorder/
   QstreamRecorder 与 optionValueWriter.outputDir 统一）；分工仍明确
   （合约级 theo/iv vs 到期级+组合级分钟序列），不做类合并
6. 记录在案：TradingIntensity 未接入 SignalFactory 可插拔框架——输出
   通道不同且默认关，扩 toxicity 信号族时再统一（不迁移）
7. 记录在案：OrderTag 未用框架 userTag 通道——registry tag 与镜像注册
   /迟到 fill 归因同一事实源，保留；透传 userTag 记为后续优化
7. 批次 4.3 OrderTag 未用框架 userTag 通道（on_order/on_trade 回调自带
   userTag，策略入队时丢弃）。registry tag 的优势：与镜像注册/迟到
   fill 归因同一事实源。记为后续优化（透传 userTag 更便宜）

**合理性结论**：无真正重复造轮子。4.4/5.2/teenie/五档深度/引擎回放
均为"补完项目既有半实现或断链"；4.2 批量验证是逐单风控的真实新
维度；3.3/5.4 持久化补的是框架不管的状态恢复。两处编码/白名单
错误已修。实施路径（修既有→补维度→配置灰度→实测）符合最小侵入。

## 3.4c 二次逐功能复核记录（11-20，30 个功能点逐一过）

**修复（本轮，commit 068ea572，已部署）**：
1. 6.3 Attribution edge 少乘合约乘数——px/theo 是每手权利金，金额需
   ×volscale（cu=5 时 edge 低报 5 倍）；从 meta.contracts 查表
2. 6.3 theta 分解多乘 -1——引擎 theta（BlackCalc::thetaPerDay）已为
   负值语义，-1 翻转把时间损耗显示为幻影收益
3. 6.0 AnalysisRecorder append 模式同日重启重复每个重启前分钟（污染
   vol 锥分位数）——改 truncate 全日重写 + loadDay 启动回灌 + 测试
4. 5.5 告警限速压力不可见——suppressed 计数并入下次成功 alert 后缀
5. VolCone 未用变量清理

**复核通过项（证据要点）**：
- 3.1 降级守卫：全量 grep 所有 setState reason（12 个）逐一核对白名
  单——无静默拦截；hotcmd_getflat_off/hotcmd_start 是死白名单项（无
  害）
- 5.1 HedgePanel $Δ 无需乘 volscale：OptionRiskData::update 证据——
  头寸希腊已乘 contractSize（吨当量），delta×fwd 即名义金额
- 4.4 MaxOpenOffsetFilter openAdd 边界（减仓/反手/纯平）验证 ✓
- 4.2 veto 双清（desired market + PendingQuote）✓；retainDrops 交互 ✓
- 5.4 replay 窗口（registerAll 后、sync_param 前）✓
- 深度链 static_assert 编译期保护 ✓
- 黑名单核对：3.1 白名单多余项无害

**新记录（不立即动）**：
- 4.4 hard-flat 闩锁无手动解除通道（resetCounters 仅 session begin；
  误触发需等日切）——启用 hard_flat_after_n_new_orders 前加 hot cmd
- 6.4 scanner 打分验证用 ATM call mid（非命中 leg 本身）——已知简化

## 3.4d UI pos 不显示——根因修复与双层问题记录（11-20，commit 3a2daa66）

**A 层（真 bug，已修）**：od->setPosition（UI leg pos 的唯一来源，
buildLeg 读 od.getPosition()）坐在批量层 enabled&&marketFresh 门内。
日内通道断线（SimNow 4097 → channel_lost → Disabled）或未恢复时段，
broker snapshot 已恢复的 _positions 不写入 OD → UI pos 恒 0（84 手
遗留持仓显示为 0）。修复：持仓真相同步移出门控。
**同类排查一并解除**（同一门内"真相同步"类）：
- _risk->update() 同门 → 断线期组合 Δ/Γ/ν/Θ 不反映恢复持仓（内部
  有 per-RiskData dirty 检查，门外空转近零成本）
- checkRiskLimitsEx / checkMarginLimits / checkDeltaHedge 同门 →
  断线恰是持仓刚恢复、最需抓超限的窗口（三者自带输入保护：
  udlPx<=0 return / channelReady 下单检查 / 节流）
- 无问题项：UTD pos（provider 实时读 _positions）、PositionGuard/
  Offsets（on_position 直喂）、UI 快照构建（无门）、批量由事件驱动
  （position/channel 事件必达唤醒，静默期无数据需同步）

**B 层（动态发现固有行为，记录）**：进程重启后网格为空（期权合约靠
tick 动态发现），开盘前 UI 整页无行——expiries=0、pos 无处显示。
夜盘开盘 tick 到达即自愈。若要求开盘前也显示合约骨架，需启动期按
contracts.json 期权链预建 grid（后续项，当前 basefiles 无期权链）。

**验证路径**：夜盘开盘后 UI 应出现合约行且 pos 显示恢复持仓（enabled
与否无关）。9/9 测试绿。

## 3.4e 全项目深度审计报告（11-20，含前端 UI）

**范围**：后端 29,779 行 C++（WtOptionCore 38 文件）+ 前端 2,351 行
（React19 SPA 21 文件）+ Python middleware + 部署配置。

**一、架构总评**
线程模型三段清晰且边界经多轮实战验证：trader 回调（async 入队，SPSC
ring+溢出合并）→ 单 worker（定价/风控/落盘/快照 POD 填充）→ IO 线程
（UiBridge socket，持锁不跨调用）。UI 快照 POD 视图（无锁跨线程）+
中间件 WS 广播的分层是本系统最成熟的部分。策略→CTG→OTG/OQM→OQM
的执行链职责分明。

**二、重复/冗余功能清单（定性）**
1. Signals/ 可插拔框架（824 行）vs COP 内嵌 alpha——**双路径设计**：
   computeValue 分支"configured→插件 / 未配置→内嵌 fallback"。生产
   无 signals 段 → 插件层按设计休眠（已在 SignalFactory.h 头注固化）。
   非重复 bug，但 824 行休眠代码是维护面，迁移/删除留待信号族扩展时
   决策
2. AnalysisRecorder（到期级+组合级分钟序列）vs OptionValueWriter
   （合约级 theo/iv）——粒度互补，outputDir 已对齐（#5），不合并
3. QuoteStatistics（3.3 持久化）vs OQM counters（#4 恢复）——两套
   计数器语义不同（聚合统计 vs 交易所限额门输入），各自持久化合理
4. TradingIntensity vs Signals/VegaFlow 等——毒性度量 vs flow 信号，
   通道不同（spread multiplier vs skew 权重）
5. DeltaHedgeExecutor（执行）vs OptionHedgeCoordinator（决策+多腿）
   ——职责分层，无重叠

**三、孤立/未接入组件（本轮处置）**
1. **死配置 key 实锤**：orderManager option/future 段的
   cancel_timeout_ms/max_orders_per_code/ttl_ms/stp_enabled/
   hard_flat_after_n_fills(future)/leave_outer_orders(future)——
   loader 全部不读（FutuCore 模板抄入）。已从服务器 config 删除 12 行
   （备份 config.yaml.bak_deadkeys）。注意：stp_enabled 无效意味着
   跨合约自成交防护不存在，仅 OQM 无条件 self-cross STP（双边互扫）
   生效——功能足够（做市双边报价场景）
2. **白名单缺失实锤**：expiries[].enable、volCurve.beta（loader 真读
   但白名单缺 → 每次启动 UNKNOWN WARN 刷屏 240 条）——已补，
   实测启动噪音 240 → 0
3. order_delay_ms：loader 读但显式 WARN "not implemented"——显式
   声明，保留
4. 前端：无孤立组件（21 文件全部可达）；types 与快照编码双向对照
   无孤儿字段；hooks/sound 全部消费

**四、最优路径判定**
- 修既有半实现 > 造新轮子：本路线 8 处"半实现补完"（hardFlat 双处/
  targets 死输入/teenie/深度断链/计数恢复/theta 语义等）持续验证
  该原则
- 灰度默认关（batch_validate/intensity/qstream/targ/max_open）+
  实测后启用——运营安全
- 剩余大件（WtDataWriter 原生 dsb 录流、开盘前合约骨架预建、
  Signals 迁移决策）均已入档为后续项，无隐性债务

**五、实测基线（本轮审计收尾时）**
夜盘开盘：enabled=true options=202 strikes=101 underlying=95,290、
启动 ConfigValidator 噪音 0、9/9 测试绿（58 用例）、tsc 0 错误。

## 3.4f 夜盘运行事故复盘（11-20 晚，commits f78d08a1 + 部署验证）

**事故**：夜盘开盘 45s 后 58 个 OQM 集体 REJECT-ONLY、冻结报价被
逆向选择吃掉（21:45:56-21:46:01 连续 8+ 笔 10 手成交）、pnl 异常。

**根因链（三层）**：
1. 【代码，已修】REJECT 阶梯语义错配——输入用 m_numNewOrders（每笔
   新委托递增，做市调价即计数）而非 m_numReject（entrust 拒绝）。
   reject_max_new_orders=3 → 每合约 3 笔新单即冻结。纠正为
   m_numReject（quantbox 原意"连续 N 笔委托被拒"）；
   MaxNewOrdersFilter 同步纠正 + FilterContext.numReject 新字段
2. 【代码，已修】pnl 基准 preClose ≠ 交易所昨结算——initPosition
   种子改 preSettle（TickData 新增字段），preClose/preSettle 每日
   偏离 × 持仓 × 乘数 = UI pnl 与 CTP 的固定偏移
3. 【环境，需用户处理】SimNow 账户限仓/资金：累计开空到限后期货
   对冲单全部被 CTP 拒（错误码 31 空消息）→ 裸 delta 500-850 吨
   持续裸露、期权开仓也大量被拒。**策略侧兜底已验证**：REJECT-ONLY
   按设计止损报价；新增 HEDGE 拒单 UI 告警（registry tag 归因，
   hedge 单不经 OQM ladder 原本不可见）。用户侧可做：SimNow 资金/
   限仓调整，或降低 delta_hedge.max_order_size / 期权报价量

**验证**：修复后重启——PnlTracker 以 preSettle 基准恢复（84 手
cu2612C104000 pos=84 等全部可见）、REJECT-ONLY 仅 4 次（真实拒单
兜底）、成交恢复正常节奏、9/9 测试绿。

**pnl 口径说明（现状，非 bug）**：UI pnl = 本地盯市
（preSettle 种子 + mid 估值 + FIFO/加权成本辅助），与 CTP
"持仓盈亏"（昨结算盯市）基准已对齐，但估值价仍为 mid（期权
spread 大时与可变现值有差），与"平今盈亏"含手续费口径亦有差。

## 3.4g HEDGE 拒单告警落地与退避三连修（11-20 深夜，446c6094）

**告警工作正常**（用户在 UI 收到 HEDGE order rejected——hedge 单
不经 OQM ladder，此告警是裸 delta 的唯一可见通道）。

**根因**：SimNow 账户限仓/资金——累计开空到限后期货 open 10 手
全部被 CTP 拒（错误码 31 空消息），裸 delta 500-850 吨。需用户
处理 SimNow 账户，或调低 delta_hedge.max_order_size / 期权报价量。

**退避三连修**（告警风暴与 CTP 轰炸的治理，实测数据）：
1. 单腿 check() 的退避门被多腿路径绕过（生产 checkMulti→fireLeg）
   → 门移至 fireLeg
2. 期权 OQM terminal（每次调价的撤单/成交终态，每秒数十个）与
   hedge terminal 共用回调分支，onTerminal(false) 高频清零 streak
   → 三态 TermAction{Reject/Reset/Ignore}：Reject=entrust 拒单
   streak++；Reset=hedge 单正常终态；Ignore=无关单不动
3. 双回报桥：insert-reject 同时走 on_entrust(false) 与
   on_order(canceled)——_hedgeRejectedIds set 桥接防 streak 被清
**部署实测**：streak 单调增长 1→10 不再清零；fire 间隔 1s → 稳定
32s（1s×2^5 cap）；entrust 拒单 130/分钟 → 4-7/分钟（-95%）。
附带：fire 日志带 backoff=2^N 诊断后缀；streak 轨迹 DEBUG 日志。

## 3.4h 运行复核三修（11-21 凌晨，MM tag 断链/readonly/pnl 口径）

1. **MM tag 断链（实锤修复）**：quote_style=buy_sell 时全部做市
   报价走 sendSingle → OrderGateway → executeOrderIds（默认
   Unknown）——MM 标记的双边路径从未被走到。gateway order leg
   显式传 OrderTag::MM。部署实证：快照 orders tag=1
2. **readonly 切换**：ui.readonly 配置硬门（hello 帧下发）——服务
   器 config 已切 false（用户要求可操作），前端刷新生效
3. **pnl 口径标注**：GreeksPnlBar 加 (local mark) + tooltip（昨结
   算基准/mid 估值/含费）。"不准确"的三层定性：真实亏损（报价冻结
   事故，已修根因）/ 基准偏移（preSettle 已修）/ 口径残差（估值侧
   与手续费，标注说明）
4. **资金不足实锤**：CTP 拒单消息解码 = "资金不足，合约缺少资金
   [687803.47](31)"——SimNow 账户缺口约 69 万（错误码 31 在该
   环境即资金不足变体）。需要用户充值 SimNow 或降仓位规模

## 3.4i UI 四问题检修（11-21 凌晨，f79f490e + 后续）

1. **RESUME 不能恢复（根因修复，端到端实证）**：shouldAllowTransition
   的 per-origin switch 覆盖全部枚举 → 末尾 hotcmd_ 前缀放行不可达
   → RESUME(Disabled→Normal) 被拦。修复：hotcmd_ 前缀统一前置放行
   （操作员命令优先于一切状态）。实测 STOP→Disabled、RESUME→
   Normal enabled=True ✓
2. **per-expiry 报价热开关（新功能）**：EXPIRYENABLE,<expiry>,<on|off>
   全链（UiBridge 翻译 → CommandBus → setExpiryEnabled → COP）；
   热关=报价 size 归零撤单、热开=冷 expiry 完整初始配置；前端
   CommandPanel per-expiry ON/OFF 开关行（2702/2703/2704 可热开，
   SimNow 资金不足期开多了会加拒单——量力）
3. **同合约多条挂单显示（定性）**：配置为每 side 单笔（无多
   ladder）。单帧去重后多数 code=2 条（bid+ask 正常）；12 个翼部
   put code 残留 4-10 条=REJECT-ONLY 冻结期未撤旧单（跨调价档）+
   SimNow cancel 回报丢失的 registry 残留。即时清理：UI cancel_all
   或重启（recovery sweep 撤全）。REJECT-ONLY 进入时撤双边已修
   （f79f490e，防新增残留）。后续项：registry vs OQM 对账清理
4. **readonly → 可操作**：ui.readonly 配置硬门已切 false（快照实证
   readonly 不在 snapshot、hello 下发 false），前端刷新生效

## 3.4j 第二轮 UI 检修（11-21，8491f7e2）

1. **cancel_all"不起作用"还原**：操作时点命中两层窗口——①旧版
   readonly 拦截；② 23:59:51 用户自行 STOP（Disabled）+ 00:00 起
   SimNow 日切停服（CTP 物理不可撤）。cancelAllLive 的通道 fence
   早退加 WARN 可见化（重连后 recovery sweep 补撤）。通道正常时
   cancel_all 端到端实测 ACK ok=true
2. **per-expiry qmode 热控（新功能）**：QMODEE,<expiry>,<on|off|close>
   全链（复用 CTG::onSetQMode 单合约能力）；前端 expiry ctrl 行每
   expiry 一组 [quoting ON/OFF + qmode on/off/close] 三键组——
   到期维度独立控制报价启停与模式
3. 遗留观察：翼部 put 多档挂单残留待日盘 recovery sweep 清理（重启
   后首个 channel_ready 自动撤全）

## 3.4k 前端命令失效第二根因（11-21，CommandBus args 语义）

UI 点击（cancel_all/qmode_expiry/EXPIRYENABLE）ACK ok=true 但零效果
的第二根因：**CommandBus::dispatch 传给 handler 的是全 tokens
（args[0]=verb 本身）**，而新命令（EXPIRYENABLE/QMODEE）按 args[0]
=expiry 解析 → strtoul("QMODEE")=0 → "bad expiry" 静默失败（ACK
ok=true 是 handler 误返回——参数错但路径对，取消类 handler 无感）。
修正 args 下标（args[1]=expiry/args[2]=mode）。
**部署功能实测**：cancel_all 108 条残留 → 1；qmode_expiry 2701 off
→ 该月 live 29 → 0；qmode on 恢复 ✓。旧 handler（CANCEL/QMODE 等）
本就按全 tokens 写——不受影响。

## 3.4l UI 架构重组落地（11-21，ac4cd35d——EXPIRY BOARD）

按深度审计方案落地（纯前端，后端零改动）：
- **ExpiryBoard** 取代 ExpirySelector/ExpiryEnableRow/UnderlierView/
  HedgePanel 四组件：行序=配置序（F1-Fn 固定绑定，回退排序漂移）、
  全行可选可看（off 月不再禁点——查看与控制解耦）、列含 $Δ/$Γ/$Θ/ν
  /futPos、TOTAL 汇总、红/琥珀行级着色、und 入行 tooltip
- 选中行控制条（单实例）：quoting ON/OFF + qmode 三键 + ALL；
  ConfirmButton 两击确认覆盖 OFF/off/close 类危险动作，PANIC 零摩擦
- ExpirySummary 瘦身为详情条（books/flags/sprd/targets）
- HealthPanel 拆分：HealthBar 3 项 + Analysis 完整 12 项网格
- readonly 双模式定稿：监控模式禁用灰可见、操作模式全功能
- 全局 qmode 按钮移除（ctrl 条 ALL 取代）；OrdersTable 静默返回改提示
- 后端零改动；9/9 测试 rc=0；build ✓ 已部署

## 3.4m UI 四维度终审（11-23，a2233b99，已推送）

四维度（功能合理性/布局/后端联通/显示正确性）全面审阅：
- **快照 schema 对齐**：16 顶层键+嵌套对象与前端 types 逐项核对全
  通，无字段缺失/拼写漂移
- **命令链路全实证**（11 条命令逐一测过）
- 修复：GetFlat 状态色（orange，与 Disabled 灰区分）；conn_state
  诊断帧接入（footer 双段链路灯——浏览器↔middleware↔UiBridge）
- 复核无 bug 项：SmileBand 变量遮蔽、TOTAL colSpan、GreeksPnlBar
  空值安全、fills dedup key
- 遗留记录：TradesTab tag 过滤状态不持久（低风险）；GetFlat 色
- 全部推送 origin（74+2 commits）

## 3.4n UI 深度审计第二轮（11-22，453ff500/bef8484b，已部署）

逐文件通读前端源码 + 抓服务端真实快照做数值交叉验证，修复：
- **假成功链路**：`socket.send()` 无返回值，断线时 Ticket/撤单/热参数
  仍提示 "sent"；现返回 boolean，全部写命令检查并提示 NOT sent
- **确认态串月**：ConfirmButton 的 armed 状态跨 label/disabled 变化
  保留，可把 A 月的确认意图落到 B 月；现变化即重置
- **BOARD 阈值单位**：红色规则原比较 `delta × fwd`（CNY 数百万）与
  `threshold_delta=30`（吨）——永不触发；第一轮改用原始 delta。
  **复核发现仍非引擎口径**：引擎按 `eg->totalDelta()`（期权+当月期货
  腿）逐月、按 `getPortfolioDelta()`（含全部期货对冲）组合触发，而快照
  只有纯期权 delta——已于本轮（3.4o）补 `undDelta`/`pfDelta` 字段对齐
- **跨日残留**：页面跨日重启保留昨日 fills（SIM 秒去重可吞今日同秒
  成交）、Analysis 环图混入昨日数据；现按 `state.tdate` 清空
- **实际挂单 vs 目标报价**：UnderlierCard 补 live 期货挂单（来自
  snapshot.orders），原 `ourBid/ourAsk` 明确标注为 pricer 目标价
- **VOL 锥未接通**：前端只请求单日 numeric tdate；改 `tdate=recent`，
  middleware 返回 `days`
- **footer bridge 永久 unknown**：middleware 从未下发桥接状态；现
  WtOptionClient 状态回调 → `conn_state(scope=bridge)` 广播 + 新客户端
  补发当前状态
- 其他：cancel all 两击确认/断线禁用、bid 五档深度补齐、Grid 空表
  colSpan 25/27、qmode 徽章空数据、三 Tab 保持挂载（Grid 键盘门控）、
  tag 过滤 sessionStorage 持久化、移除非热参数 `wgt_exp_ddelta`
- 验证：tsc ✓、vite build ✓、9/9 测试二进制 rc=0（test_uibridge 旧
  二进制与 16:44 动态库 ABI 不同步，重编后通过）；线上 WS 实测帧序
  `hello → conn_state(connected) → ack → snapshot`；`/api/series`
  非法 tdate 返回 400。服务端 `generated/volseries_*.csv` 尚未生成
  （周末 0 合约），下个交易日 recorder 产行后锥形图即有数据

## 3.4o 审计复核修正（11-22 晚，已部署）

对 3.4n 的复核发现两处自身未闭环 + 一处语义错误：
- **断线禁发未贯彻**：BOARD ControlStrip 四个命令（expiry_enable /
  qmode on/off/close）与 Grid 双击 qmode 切换仍裸调 sendCommand；
  现统一 readonly/disconnected 门禁 + 失败提示（ExpiryBoard 接入 onNotify）
- **TOTAL 行语义错误**：3.4n 称"组合 delta（含期货对冲）"，实际
  `snapshot.greeks.delta = getPositionGreeks()->delta()` 为纯期权
  delta（期货对冲在 `m_pfuDelta`）；对冲打平后 TOTAL 仍可能红
- **修复（后端+前端）**：
  - 快照新增 `greeks.pfDelta = OptionRisk::getPortfolioDelta()`（期权+
    期货对冲，引擎 hedge 触发同一量）
  - 快照新增 `expiries[].und.undDelta = pos × contractSize`（当月期货腿
    delta，与 `OptionExpiryGreeks::totalDelta` 口径一致）
  - BOARD 行级红色改比 `|Δopt + Δleg|`，TOTAL 改比 `|pfDelta|`
  - UnderlierCard 显示 Δleg
  - 顺手修 `bd/ad` 编码含最优档的注释漂移：改发 i≥1（beyond best），
    前端 "+" 聚合不再重复计算最优档
- 流程纪律补录：libWtOptionCore.so 重编后，依赖它的 test_* 二进制必须
  同步重编（本轮 test_uibridge 假阳性即源于 ABI 不同步）

## 3.4p GVV 调翼能力补齐（11-22 夜，4f871503，已部署）

quantbox GVV 调翼四手段逐项对照后的补齐（分析见 3.4o 后讨论）：
- **β 翼部权重衰减**：能力已透传（`volCurve.beta`），服务端 config 实为
  beta=1（早先已配）；本地 dist config 补录并对齐
- **`volcurve_weight` 热调**（quantbox GUI 的 gvv_weight 旋钮）：
  SETPARAM 白名单 + sync_param + applyHotParams →
  `setGvvWeightDefault`（新增，重推全部活跃月份，逐月覆盖仍优先）；
  快照 params 回显 + UI 热参数面板补行；顺手修复
  `setGvvWeightPerExpiry` 只写映射不重推的历史问题。实测
  set 0.5→ack ok→恢复 0.0→回显正确
- **α 搜索区间透传**：`volCurve.alpha_dn/alpha_up`（<0 用内核默认 [0,2]），
  p2cfg → AsyncVolFitter → createVolCurveByType 全链
- **外部参数直灌通道（DB→setParameter）**：按运营需求暂缓
- 验证：9/9 二进制 rc=0、线上 SETPARAM roundtrip 实测、日志 0 error

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
| 批次 5 UI P1+参数 | ✅ 完成（5.1 hedge 面板/5.2 到期希腊目标/5.3 排序过滤/5.4 参数持久化实测/5.5 alert 推送） | 9/9 二进制绿，全部署 |
| 批次 6 分析复盘 | ✅ 完成（6.0 vol/pf 边车/6.1 smile 带/6.2 vol 锥/6.3 归因/6.4 信号打分/6.5 可回放流+离线校验；引擎级回放留后续） | 57 测试绿，9/9 二进制绿，API 实测 |
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
