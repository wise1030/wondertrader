# 借鉴适配方案：GARCH / 盘口深度 / 扫描器 / alpha 信号（2026-11-25）

> 原则：**充分利用项目已有组件，不重复造轮子**。四项均先方案，未动代码。
>
> 已有可复用资产：`OptionPricer2` + `IVolCurve` 接口、`AsyncVolFitter`（后台拟合）、`OptionMarket` **多档盘口**（MAX_MARKET_DEPTH 档 bid/ask 数组，已在 OptionGrid.onTick 填充）、`OptionScanCoordinator` + `IScanModule`、`ComboOrders`（Spread/Syn/Simul 执行器）、`SignalFactory`（alpha/risk 工厂）、`setupSignals`（`signals.alpha[]/risk[]` 数组加载）、`AnalysisRecorder`/`AttributePublisher`（回放与归因）、`test_replay_benchmark`。

---

## 1. GARCH 波动率（优先级：评估后接入）

### 1.1 借鉴什么
原版 `OptionPricer3`/GarchModel/GarchState：状态化波动率跟踪，用于缓解本项目 Black+GVV 的 bootstrap 尺度爬行（11-18 记录：0.1000→0.1629 十一步爬 1h）与翼部失真。

### 1.2 适配路径（不引入 OptionPricer3 全家）
- **接口复用**：新增 `GarchVolCurve`（实现现有 `IVolCurve` 接口），作为 **curve2 候选**（现 `curve2_type: LinearVolCurve/GvvVolCurve`），与现有曲线并存，**不替换 BlackCalc/OptionPricer2 主链**。
- **拟合复用**：GARCH 状态（条件波动率、参数）由 `AsyncVolFitter` 同一后台管线输出——在现有 fit snapshot（forward/maturity/atmVol/points）上增加 GARCH 状态字段，复用 `SingleFlightTaskRunner`。
- **数据源复用**：`OptionRiskData/OptionData.values().impliedVol()` 与分钟 bar（`AnalysisRecorder` 已落 vol/pf 分钟序列）作为 GARCH 估计输入。
- **回放复用**：`test_replay_benchmark` 做 GVV vs GARCH-lite 的 A/B（指标：theo 与市场 mid 偏差、翼部 medDev、报价成交率）。

### 1.3 分期
- **GARCH-lite（一期）**：只把 GARCH 的条件波动率作为 `atmVol` 的**另一种锚**（经现有 `volcurve_weight` 混合权重），不动曲线族；先离线 replay。
- **GARCH 曲线（二期，若一期优胜）**：完整 `GarchVolCurve` 接入 curve2_type 热键。

**明确不做**：不新写独立 pricer、不改 BlackCalc 主定价路径、不引入 GARCH 参数 GUI 全家桶。

---

## 2. 盘口深度 IBook（小步增强，零新组件）

### 2.1 现状
`OptionMarket` 已含 `bidPrices/bidQty/askPrices/askQty[MAX_MARKET_DEPTH]` 与 `numBidLevels/numAskLevels`（OptionGrid.onTick 已按档填充）；`OptionData::getLevel(i)` 直接返回第 i 档。**深度数据已经在手边，无需接 IBook**。

### 2.2 适配点（按价值排序）
1. **queue-position 精确化**：现 `UiSnapshotBuilder` 的 bqp/aqp 用"价差/tick 数"近似。改为按盘口**累计排队量**（我方价上方各档 qty 求和）——对应 quantbox `bqpos/aqpos` 真义。
2. **sticky_force（原版注释项）**：`base + sticky_force × px_level_mass/100 × tick`， capped `sticky_max_ticks`。用现有多档量实现，评估是否启用（现 sticky 只 sticky_base）。
3. **市场有效性**：teenie 当前"无 best bid 跳过该侧"。可增加**最小深度门槛**（如 best qty < N 不挂，避免被极薄盘口钓鱼）——复用 `getLevel(0).qty`。
4. **freshness**：现只用时间戳；可叠加盘口宽度/深度异常（spread 超阈值或双边量为 0）判为"不可信市场"，并入 `inputs_good`。

### 2.3 验证
- 单测：queue-pos 求和、深度门槛；
- replay：对比 bqp 近似值 vs 真排队量的颜色命中（quantbox queueBackgroundFormatter 语义）。

---

## 3. 扫描器族（复用扫描框架 + 组合执行器）

### 3.1 框架现状
`OptionScanCoordinator`（startAll/stopAll/dispatchOption/dispatchTick）+ `IScanModule` + `ControllableTradingGrid::addScanner` + **ComboOrders 执行器**（SpreadComboOrder/SynComboOrder/SimulComboOrder）全部就绪。目前生产**无扫描器配置**（空转，属合理未启用）。

### 3.2 适配建议（按业务价值选 1–2 个，不铺开）
| 候选 | 业务作用 | 复用点 |
|------|----------|--------|
| **ButterflyScanner** | 蝶式组合：用组合单降 vega/gamma（买方波动率结构） | `SpreadComboOrder` 已有三腿执行、combo 风控 |
| **StrikeSpreadScanner** | 同行权价/跨行权价价差：纠偏错位定价（theo vs 市场），被动成交降 delta/vega | `SynComboOrder`（跨式/价差）执行器 |
| LowBidsScanner / MMScanner / OpenScanner | 深度/开盘类机会 | 视二期需要 |

- 配置：`scanners[]`（config 已有该段白名单，dynamic section）→ `OptionScanCoordinator` 注册；combo 复用现有风控与 `OrderTag::Scanner` 归因桶（liveByTag/fillsByTag 已有）。
- 风控：复用 RiskFilterChain + PositionGuard + riskmon，不另起炉灶。

### 3.3 明确不做
- 不新写 combo 执行器；
- 不把扫描器信号直连下单（必须经过 CTG 排名/TPS/风控链）。

---

## 4. alpha 信号 config 化（`signals.alpha[]`）

### 4.1 现状
- 权重在 params 顶层 `wgt_vegaflow/frontfut_skew/deltaflow/atmsig/rollema`（硬编码路径 `alpha_adjustment`）。
- `setupSignals` 已支持 `signals.alpha[]`（type/enable/params），`SignalFactory` 已注册：`VegaFlow`、`DeltaFlow`、`AtmSig`、`RollEma`、`FrontFutSkew`、`FrontAtmvFlow`（Signals/AlphaSignals.cpp:212-217）。
- **热参面板 `params.schema` 仍暴露这 5 个 wgt_*，与"alpha 权重不进 UI 热参面板"的既定要求冲突**——本次一并处理。

### 4.2 适配
- 配置示例（`signals.alpha[]`）：
  ```yaml
  signals:
      alpha:
          -   type: VegaFlow
              enable: true
              params: { weight: 0.35 }
          -   type: FrontFutSkew
              enable: true
              params: { weight: 0.25 }
          -   type: DeltaFlow
              enable: true
              params: { weight: 0.20 }
          -   type: AtmSig
              enable: true
              params: { weight: 0.15 }
          -   type: RollEma
              enable: true
              params: { weight: 0.05 }
  ```
- **schema 清理**：热参面板移除 `wgt_vegaflow/wgt_frontfut_skew/wgt_deltaflow/wgt_atmsig/wgt_rollema` 五个键；权重改由 `signals.alpha[].params.weight` 承载（运行时权重来源收敛到信号自身 `getWeight()`，而非顶层 wgt_* 同步参数）。
- 兼容：顶层 wgt_* 保留为默认回退（无 `signals` 配置时），与 risk signals 的加载方式一致。

### 4.3 明确不做
- 不把 alpha 权重放回 UI 热参面板（与既定要求一致）；
- 不重复实现信号类型（工厂已有全部 6 类）。

---

## 5. 分期与验证

| 期 | 内容 | 验证 |
|----|------|------|
| P0 | alpha schema 去 wgt_* + `signals.alpha[]` 配置接入 | 启动日志 `Alpha signal loaded`；schema 无 wgt_* |
| P1 | IBook 深度小项（queue-pos 真排队量、深度门槛） | 单测 + replay bqp 对照 |
| P2 | GARCH-lite（atmVol 另一锚，离线 A/B） | `test_replay_benchmark`：theo 偏差/翼部 medDev/成交率 |
| P3 | 扫描器 1 个（Butterfly 或 StrikeSpread）灰度 | combo 执行回放 + SimNow 小仓 |

**全部复用现有组件；新增代码仅限：GarchVolCurve（二期）、queue-pos/深度门槛小函数、scanner 配置接线。**
