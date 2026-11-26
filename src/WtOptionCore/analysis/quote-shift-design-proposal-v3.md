# 报价偏移（Risk Shift）优化方案 v3 —— 成本模型修正版

> **v3 修订说明**：修正 v2 的核心错误——降险成本模型搞反了。
> **正确的业务逻辑**：期权降 delta 通过**报价偏移 + 被动成交**实现——成交挂在**我们自己的报价**上，降险的同时**赚取点差**（不是支付点差），且期权手续费低于期货；正常做市条件下报价偏移就是 delta/vega 降险的**主力通道**。期货对冲是**风控层面的兜底**（偏移来不及/不够时的硬手段），不是日常首选。
> 这意味着原版 quantbox 的通道并列结构是正确的；v2 的"delta 期货为主、期权偏移条件激活"**作废**。未改任何代码。

---

## 1. 降险成本模型（修正后）

| 工具 | 点差成本 | 手续费 | 时效 | 定位 |
|------|----------|--------|------|------|
| **期权报价偏移 + 被动成交** | **赚点差**（成交价含我们的利润） | 低（期权费） | 被动等待，承载能力受 TPS/点差约束 | **主力**（delta + vega） |
| 期货对冲（DeltaHedger） | 付点差（sprd_fut=100 元 ≈ 0.05%）+ 期货手续费 | 高 | 立即 | **风控兜底**（偏移不够快/不够量时） |
| GetFlat 收盘穿价 | 穿价成本（明确核算） | 高 | 立即 | 收盘特例 |

推论：
- 报价偏移的**承载力**受两个约束：点差空间（偏移多少个 tick 仍能被动成交）与 TPS/挂单容量。超出承载的风险增量（急速行情、大单冲击）才轮到期货兜底。
- **期货兜底的触发阈值应该反映"偏移承载已不足"**，而不是现在 `threshold_delta: 30`（=6 手期货当量）这种日常小额水平——30 的阈值让日常小额 delta 全部走期货硬对冲（今天 140 次 fired、29,886 条卖单日志），正是主次颠倒的实证。

## 2. 工具架构（v3 定稿）

| 风险 | 主力（常开） | 兜底（风控触发） |
|------|--------------|------------------|
| **delta** | 期权报价偏移：`ddAdj`（月内相对倾斜）+ **L3-delta**（组合二次型平移，常开） | DeltaHedger：阈值与 L3 分级挂钩（§4），只在偏移承载不足时上 |
| **vega** | **L3-vega**（唯一工具，常开） | 无（vega 没有兜底工具，更要求偏移通道可靠） |

与原版 quantbox 的关系：**回到原版的并列结构，但补上原版没有的三件事**——① 量纲校准（risk_tol 与品种限位对齐）；② 二次型 shift 的参数化落地（原版只有被注释的骨架）；③ 触发分级与被动性封顶（业务保护）。

## 3. 现状复核结论（不变项 + 修正项）

### 3.1 不变项（v2 已确认正确）
- `delta_risk2`（`−sign(δ_leg)·normRiskDelta·cost`）：call/put 自动反向平移，方向正确
- `vega_risk2`（`−vega_risk_norm·cost`，不分腿）：买任何期权都补 vega，方向正确
- `adj` 语义 = **报价整体平移**（同号进入 bid/ask）
- 量纲错配是"报价被推出市场、有偏移无成交"的根因（fallback risk_tol=10/1000 vs 应为 1000/10000）

### 3.2 修正项
- ~~"期权降 delta 成本高，期货优先"~~ → 被动成交赚点差，期权偏移是主力；期货是兜底
- ~~"delta L3 条件激活"~~ → L3-delta 常开；改为改造 **DeltaHedger 的触发阈值**（详见 §4）
- `setRiskShiftDelta/setRiskShiftVega` 死通道 + L3 二次型骨架：借鉴接通（不变）
- Panic 链路失效（四天 0 触发）+ 方向性抑制分级：不变（§5）

## 4. DeltaHedger 兜底触发改造（v3 新增，核心业务修正）

现状问题：`threshold_delta: 30`（6 手期货当量）+ cooldown 1s → 日常小额 delta 全部硬对冲，偏移通道形同虚设（11-25 DeltaHedger fired 140 次，其中绝大多数 delta_excess 在几十到几百——偏移完全可承载的量级）。

**改造：兜底阈值与偏移承载挂钩（`|pgn|` 分档）**

| 档位 | delta_excess（normRiskDelta 口径） | 期权偏移（主力） | 期货兜底 |
|------|-----------------------------------|------------------|----------|
| `\|pgn_d\| < 0.8` | 正常做市带内 | ddAdj + L3-delta 常规偏移，被动成交 | **关闭**（threshold_delta 动态提升到 ∞） |
| `0.8 ~ 1.0` | 软限 | 偏移加陡（二次项生效）+ 开仓边 size 收缩 | **预备**（cooldown 缩短、单量准备） |
| `1.0 ~ 1.2` | 限位 | 偏移拉满（shift_max）+ 方向性抑制（停增险边） | **触发**（threshold 降回 30 或更低，立即兜底） |
| `≥ 1.2` | 失控 | 全撤 + Panic | 兜底全力 + 人工介入 |

实现要点：
- `DeltaHedgeExecutor::decide` 增加"档位门"：`eff_threshold = base_threshold × g(pgn)`，`g` 在偏移承载内为 ∞（或极大值）、限位处为 1——即**期货对冲的阈值随偏移承载状态动态伸缩**，配置保留 `threshold_delta` 作为限位档的兜底阈值（建议重校为 200~300，40~60 手期货当量，避免限位档高频碎单）。
- 兜底强触发条件（不看档位、立即触发）：偏移通道失效（连续 reject/资金拒单导致降险买单无法落地）且 `|pgn_d|` 持续上升——此时期货是唯一可用降险工具。
- vega 无兜底工具 → vega 档位到达 1.0 后**只允许偏移拉满 + 停增险**，并强提示运营（UI Alert），因为不存在"更快"的替代。

## 5. 分级处置（承 v2，微调）

| 档位 | 期权偏移（主力） | 期货兜底 | 状态 |
|------|------------------|----------|------|
| <0.8 | 常规 | 关闭 | Normal |
| 0.8~1.0 | 加陡 + 开仓边收缩 | 预备 | Normal（告警） |
| 1.0~1.2 | 拉满 + 方向性抑制（停增险边、保降险边） | 触发 | 可保持 Normal 或 GetFlat 语义 |
| ≥1.2 | 全撤 | 全力 | Panic（修复后的链路，300s 重评） |

毒性流 / 成交价异常 / margin guard：现有独立通道不变。

## 6. 设计细节（承 v2，补充被动成交语义）

1. **被动性封顶**：偏移后 `bid ≤ mkt_ask`（允许 touch，不穿透）、`ask ≥ mkt_bid`——touch 价在价格优先上仍占先，成交后依然是"我们挂的单被吃"（赚点差）；穿透即变主动吃单，那是对冲的语义，必须走兜底通道。
2. **sticky 降权**：`|pgn| > 0.5` 时粘滞权重线性降 0。
3. **delta/vega 联动**：两者同时超限时独立偏移可能出现同一腿双向激励（如净空头 vega + 净多头 delta：买 call 补 vega 也补 delta ✓ 一致；买 put 补 vega 但减 delta ✗ 冲突）。一期保留原版交叉项 `h = I_v·I_d·sign(I_d)`（含 markup 封顶）+ 监控分腿成交的方向占比；二期再评估联合效用函数。
4. **targ 对齐**：`targ_delta/targ_vega` 热参 = L3 平移锚点 = `delta_hedge.target_delta` 同源（防 L3 与兜底拉向不同目标）。
5. **SimNow 环境约束**：无资金环境下偏移的降险买单仍会被 error-31 拒（被动成交也需要资金），机制验证以 replay 为主、SimNow 只验证报价位置/撤挂行为；完整降险成交验证需资金恢复。

## 7. 参数建议（cu 量纲，全部进热参面板）

> **⚠ 量纲修订（2026-11-25 14:2x 实盘教训）**：`vega_adj = shift × vega_leg` 中 vega_leg 单位是元/1%vol（ATM ≈100-200），shift 的"元"值会被放大两个数量级——首版 `shift_max_vega=30` 实际平移 ≈3-6k 元，触发涨停价 REJECT 风暴（14:14-14:21，每分钟 20-147 条，riskmon 涨停检查拦下未进市场）。**已实盘缩参**：`distort_vega=0.02, shift_max_vega=0.05`（平移 ≈7 元）；delta 通道 δ 无量纲不受影响，缩为 `distort_delta=3, shift_max_delta=10`。二期应把 vega shift 迁移到 vol 空间定义（`adj = shift_vol(%vol) × vega_leg`），语义纯净。

| 参数 | 实盘值（已修正） | 语义 |
|------|------------------|------|
| `risk_prem_opt`（=risk_tol_delta） | **1000**（=max_delta） | normRiskDelta 归一到 ±1 |
| `risk_prem_vega` | **10000**（=max_vega） | vega_risk_norm 归一到 ±1 |
| `wgt_exp_ddelta` | **0.05** | 月内相对倾斜（expNetDelta 千级，0.05 已可触底到贴 mid；0.5 会立即饱和） |
| `distort_delta` / `shift_max_delta` | **3 元 / 10 元** | L3-delta 二次型 + 封顶（×δ≤1 → ≤10 元 = 5 tick） |
| `distort_vega` / `shift_max_vega` | **0.02 / 0.05** | L3-vega（×vega≈150 → ≤7.5 元平移） |
| `risk_shrink_start` | **0.8** | 开仓边 size 收缩起点 |
| `threshold_delta`（兜底档） | **250**（bail_threshold_delta） | 期货兜底在限位档的实际阈值 |
| `bail_pgn` | **0.8** | 期货兜底武装的 |pgn_d| 门 |
| `panic_severity_ratio` | **1.2** | 全撤 Panic 的失控比 |
| `enable_teenie_pricing` | **true** | 排队优先（原版 MMPricer 语义） |

## 8. 落地顺序

| 阶段 | 内容 | 改动面 |
|------|------|--------|
| P0 | 量纲校准 + ddAdj 启用（config） | config.yaml |
| P1a | L3-delta/L3-vega 接通（二次型 + 死通道接口）+ 热参 | CompositeOptionPricer + ParamRegistry |
| P1b | 被动性封顶 + sticky 降权 | CompositeOptionPricer |
| P1c | 分级处置 + DeltaHedger 兜底阈值改造 + Panic 修复 | HftOptionStrategy / CTG / Coordinator |
| P2 | 快照/TRACE 可观测性 + replay 归因 | UiSnapshotBuilder + 前端 |

验证：shift 纯函数单测；11-24/25 replay 对照（现状 / P0 / P0+P1）观测——报价回归市场（bqp 分布）、**降险方向被动成交占比**（核心 KPI：降险腿成交/增险腿成交）、delta/vega 回归目标的轨迹、期货兜底触发次数（应大幅低于当前 140 次/日）。SimNow 灰度一个交易日看报价位置与撤挂行为。

## 9. 明确不做

1. 不引入原版 lua `risk_tolerances` 表（走热参体系）。
2. 不搬 `max_risk_d/max_risk_v`（RiskLimitsEx 已覆盖）。
3. vol 曲线整体 shift 缓行（定价一致性风险）。
4. **不把 DeltaHedger 恢复为日常低阈值对冲**——30 的阈值是"主次颠倒"的根源，兜底定位要求阈值由偏移承载状态动态决定。
