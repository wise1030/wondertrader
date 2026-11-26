# 报价偏移（Risk Shift）优化方案 v2 —— 业务适配版

> **v2 修订说明**：v1（quote-shift-design-proposal.md）完成了原版考察，但存在"照抄倾向"。本版从业务实际出发逐条复核，修正 v1 的三处设计偏差，形成分阶段落地方案。**未改任何代码。**
>
> 业务基准（用户语义）：对冲是 delta/vega 风险暴露过大时的**主动降险过程**；手段是通过报价偏移让降险方向的腿更容易被动成交；期货硬对冲是风险极端时的最后手段。
>
> 业务现实（代码/配置/日志证据）：cu 期权，乘数 5，tick 2 元；期货对冲成本 = sprd_fut 100 元（≈0.05%），期权降险成本 = 期权点差数倍于期货；SimNow 资金不足（error 31，474 次/日）；vega -17.7k 挂限 5 天；Panic 链路 0 触发。

---

## 0. v1 → v2 的三处修正（借鉴≠照抄）

| # | v1 的照抄倾向 | 业务现实 | v2 修正 |
|---|---------------|----------|---------|
| 1 | 恢复原版 L3，delta/vega 二次型 shift 并列全开 | **delta 与 vega 的降险工具经济学完全不同**：delta 有期货对冲（0.05% 成本，立即成交）；期权降 delta 要付期权点差（数倍成本）。vega 则只有期权报价一条路 | **vega L3 为主力通道（P1a 先行）；delta L3 降级为条件激活**（期货对冲被阻/小额残余时才启用），见 §3.2 |
| 2 | 沿用原版"超限 → 全停（Panic）" | 超限时把双边全撤 = **连降险腿一起锁死**，与"降险过程"的业务语义直接矛盾 | **breach 改为方向性抑制**：停增险边、保降险边；Panic 只留给失控（≥1.2×限）/毒性流/成交价异常，见 §3.4 |
| 3 | shift 公式直接搬 `s = −sign(pgn)·distort·pgn²`，参数原样起试 | cu 与 IO 量纲/点差结构完全不同；SimNow 即时撮合下过头的 bid 会直接吃单，违背"被动成交"语义 | 参数本地化（§4）；shift 上限**相对市场**封顶（`bid ≤ mkt_ask − 1 tick`），见 §3.3 |

另确认两处 v1 正确、保留的判断：量纲错配（`risk_tol` fallback 10/1000 vs cu 的 1000/10000）是报价被推出市场的根因；`delta_risk2/vega_risk2` 方向语义正确（v1 曾误判为死代码，实为 fee 级微调通道）。

---

## 1. 降险工具分层（业务驱动的新架构）

按降险成本与工具可得性分层，取代原版"全通道并列"：

| 风险 | 第一梯队（持续） | 第二梯队（条件激活） | 最后手段 |
|------|------------------|----------------------|----------|
| **delta** | 期货对冲 DeltaHedger（0.05% 成本，即时） | 期权层 delta 偏移（ddAdj + L3-delta），**仅当**：① 期货对冲被阻（reject streak ≥ 3 / 资金拒单）；② 残余 delta < hedger 阈值（30，小额不值得开期货单） | GetFlat 收盘穿价 |
| **vega** | **期权报价偏移（唯一工具）**：L3-vega 主力 | vol 曲线整体 shift（二期，影响定价一致性，需谨慎） | 无（vega 没有期货工具） |

> 这一分层回答了"借鉴什么"：借原版的**归一化框架（pgn）+ 二次型偏移 + shift_max 封顶**三件套，不借它"delta/vega 并列全开"的通道配置——那是 IO 业务（期权点差相对窄、期货对冲受现货对冲约束）的产物。

---

## 2. 复用现状中已被验证正确的部分（不重做）

| 现有机制 | 状态 | 处置 |
|----------|------|------|
| `delta_risk2`（带 `sign(δ_leg)` 的 call/put 反向平移） | 方向正确、量纲错 | P0 校准后保留为 fee 级微调 |
| `vega_risk2`（不分腿平移） | 方向正确、量纲错 | 同上 |
| `risk_adjustment_future`（期货腿方向偏移） | 方向正确、量纲失控（±5250 元） | P0 校准后保留 |
| `ddAdj`（逐月净 delta 线性 skew） | 未配置（0） | P0 启用，管"月内相对倾斜" |
| `updateRiskShiftsVega` 的相关性衰减/目标扣减框架 | 参数全 0，退化为自身 vega | 保留框架，参数可选校准 |
| DeltaHedger（阈值/cooldown/backoff/in-flight/mid 被动价） | 工作正常 | 保留为 delta 第一梯队 |
| `setExpiryTargets`（targ_delta/targ_vega 热参接线） | 已三处接线，生产值为空 | §3.5 打通语义 |

## 3. 设计方案

### 3.1 P0 —— 量纲校准（纯配置，重启生效）

| 参数 | 现值 | 建议 | 效果 |
|------|------|------|------|
| `risk_prem_opt` | 未配置（fallback 10/1000） | **1000**（= max_delta） | `normRiskDelta` 回 ±1 量纲（当前 ±105）；delta_risk2 从"±105 tick 推出市场"回归 fee 级微调 |
| vega risk_tol | fallback 1000 | **10000**（= max_vega） | `vega_risk_norm` 回 ±1.8（当前 ±17.7） |
| `wgt_exp_ddelta` | 0 | 0.5~1.0 | 启用月内相对倾斜 |

> P0 单独生效即可把报价拉回市场（解除"有偏移、无成交"状态），是后续一切的地基。**风险提示**：校准后报价重新贴近市场 → 成交率上升 → SimNow 无资金环境下买入方向拒单增多，属预期行为。

### 3.2 P1a —— vega L3 主力通道（借鉴原版二次型，业务适配）

**为什么 vega 优先**：vega -17.7k 挂限 5 天无任何处置路径；且 vega 没有期货工具，报价偏移是唯一手段。

```
pgn_v        = (vega_pf − targ_vega) / risk_tol_vega          # 现有框架，targ 走热参
s_v          = −sign(pgn_v) · distort_v · pgn_v²              # 原版二次型：限内≈0，越限加陡
shift_vega   = clamp(s_v, ±shift_max_v)                       # 封顶
adj_vega     = shift_vega × vega_leg                          # → 报价整体平移（不分腿）
```

- `distort_v` 以 **ATM spread 分数**表达（业务可读）：`shift_raw = pgn_v² × k_v × typical_spread`，`k_v` 建议起试 0.5（即 pgn=1 时偏移半个点差），`shift_max_v` 起试 **3×ATM tick 当量**。
- **叠加位置**：`adj().vega_risk`（原死通道接口），与 `vega_risk2`（校准后的 fee 级项）并存，二期归因后决定合并。
- **方向性正确性**：净空头 vega → pgn_v<0 → s_v>0 → 报价上移 → bid 激进买入期权（补 vega）✓；vega 与 delta 不同——买 call 买 put 都补 vega，平移不分腿是**业务正确**而非设计偷懒。
- **delta 噪声的代价**：补 vega 的被动成交会带来随机 delta（买 call +δ / 买 put −δ）→ 由第一梯队期货对冲吸收。**这正是分层架构的分工：vega 偏移制造的小额 delta 噪声由廉价的期货对冲清理，而不是让期权偏移去做 delta 的主力。**

### 3.3 P1b —— delta L3 条件激活 + 被动性封顶

```
激活条件（满足其一）：
  A. 期货对冲被阻：rejectStreak ≥ 3 或近期 error-31 密度超阈
  B. |delta_excess| < hedger 阈值（30）：小额残余，期货单不值得开
否则：delta 交给期货对冲，期权 delta 偏移保持 0（避免与期货对冲抢单、付期权点差做期货能做的事）
```

- 公式同 vega（`pgn_d = (delta_eff − targ_delta)/risk_tol_d`，二次型，clamp）。
- **被动性封顶（新增，业务关键）**：偏移后的 `bid ≤ mkt_ask − 1 tick`、`ask ≥ mkt_bid + 1 tick`——保证是"挂等成交"而不是变成吃单。SimNow 即时撮合下尤其重要；真要吃单应该走 DeltaHedger/GetFlat（有明确的授权语义与成本核算）。
- **sticky 降权**：`|pgn| > 0.5` 时 `__apply_sticky_params` 的粘滞权重线性降 0（防粘滞阻碍偏移落地）。
- 期货腿 `risk_adjustment_future` 同步使用同一 `shift_delta`（校准后量级合理），期货 OQM 挂单与 DeltaHedger 同向。

### 3.4 P1c —— breach 处置：方向性抑制取代全停

按 `|pgn|` 分级（delta/vega 各自独立）：

| 档位 | 处置 | 实现点 |
|------|------|--------|
| `\|pgn\| ≥ 0.8`（软限） | L3 偏移启动 + 开仓方向 size 线性收缩至 0（`__computeQuoteSize` 加风险因子） | §3.2/§3.3 + size 通道 |
| `\|pgn\| ≥ 1.0`（限位） | **方向性抑制**：增险边完全撤单/不发；降险边保留且偏移拉满 | CTG refresh 分边处理（`panicClear` 改造为 `directionalClear`） |
| `\|pgn\| ≥ 1.2`（失控） | 全撤 + Panic（blackout 300s 重评） | 现有 Panic 通道 |
| 毒性流/成交价异常 | 现有 pricer panic / FillPriceChecker 不变 | — |

> 与现状的关键差异：现在 `max_vega` REJECT → `_limitsBreached` → 理论全停（实测 0 触发的失效链路）。v2 把"限位"从"全停"改为"停增险、保降险"——**全停恰恰会锁死降险通道**。原失效链路作为 1.2 档的兜底继续存在，修复其触发错配（panic 块嵌在 `enabled && marketFresh && shouldCompute` 三重门内的时窗错配）作为独立专项。

### 3.5 P1d —— 目标位统一（防两套机制打架）

现状：DeltaHedger `target_delta` 未配置（=0），L3 的 `targ_delta/targ_vega` 热参为空（=0）——恰好一致，但**若运营单独改 targ_delta 热参，L3 拉向 targ 而 hedger 仍拉 0，两套机制反向拉扯**。统一：

- `delta_hedge.target_delta` 与 `targ_delta` 热参改为**同一数据源**（targ 热参优先，config 初始化缺省 0）；
- UI 面板对 `targ_delta/targ_vega` 增加语义说明（"对冲与偏移的共同目标位"）。

### 3.6 P2 —— 可观测性与验证

- 快照 per-expiry 暴露 `normRiskDelta / vega_risk_norm / shift_delta / shift_vega / 档位`，UI Grid 着色（quantbox `edpub.delta_shift` 显示语义）。
- TRACE 日志（原版 `trace-risk-adj-*` 风格）按合约 debug 开关。
- **验证三步**：① shift 纯函数单测（pgn=0/±0.5/±1/±3、clamp、call/put 方向）；② replay（11-24/25 数据）三组对照：现状 / P0 / P0+P1——观测 bqp 分布回归、降险成交占比（方向性 fill 统计：降险腿 fill / 增险腿 fill）、vega/delta 轨迹、期货腿手数变化；③ SimNow 观察 1 个交易日（注意资金拒单仍会限制买入方向的验证，**完整的降险买入验证需要资金恢复后进行**——SimNow 阶段重点验证报价位置与撤挂行为）。

---

## 4. 落地顺序与工作量

| 阶段 | 内容 | 改动面 | 依赖 |
|------|------|--------|------|
| P0 | 3 个配置项 | config.yaml only | 无（重启生效） |
| P1a | vega L3（shift 计算 + adj 接线 + 热参） | CompositeOptionPricer + ParamRegistry | P0 |
| P1b | delta L3 条件激活 + 被动性封顶 + sticky 降权 | CompositeOptionPricer | P0 |
| P1c | 方向性抑制 + size 分级 + Panic 阈值改造 | CTG / HftOptionStrategy / Pricer | P1a/b |
| P1d | 目标位统一 | HftOptionStrategy + config | 无 |
| P2 | 可观测性 | UiSnapshotBuilder + 前端 | P1a |

## 5. 明确不做（与原版的 deliberate 差异）

1. **不照搬原版 `max_risk_d/max_risk_v` 的 alpha 硬限**——现版已有 RiskLimitsEx 独立限位体系，重复设限只会增加打架点。
2. **不让期权 delta 偏移与期货对冲并列常开**——成本经济学决定的分层（§1）。
3. **不引入原版 lua `risk_tolerances` 表**——ParamRegistry 热参体系已存在，参数走热参 + config，不再引入第二套配置机制。
4. **vol 曲线整体 shift 缓行**——影响定价一致性（theo 全部移动会改变与市场的 iv 偏离），先做报价层平移，定价层 shift 二期评估。
