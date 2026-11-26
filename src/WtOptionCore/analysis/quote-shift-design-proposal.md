# 报价偏移（Risk Shift）优化方案 —— 基于 quantbox 原版设计考察

> **任务**：考察原版 quantbox `strategy/optioncore` 的报价偏移设计，借鉴并优化当前 WtOptionCore 的实现。**本方案只做设计，未改任何代码。**
>
> 原版位置：`/mnt/d/gf_pc/WonderTrader/quantbox/quantbox/strategy/optioncore/CompositeOptionPricer.{cc,h}`（2709 行）；现版：`wondertrader/src/WtOptionCore/CompositeOptionPricer.cpp`。

---

## 1. 一个关键的事实修正

在写入方案前，先修正上一份审查报告（riskshift-hedge-review）中的一处符号理解错误：

```
bid = mid − (spread − adj) = mid − spread + adj
ask = mid + (spread + adj) = mid + spread + adj
```

`adj` 以**同号**进入 bid/ask 两侧——它的语义是**报价整体平移**（directional quote shift），不是对称缩放：

- `adj > 0`：整个报价上移 → 我们的 bid 更高（激进买入）、ask 更高（被动卖出）→ **净买入方向**
- `adj < 0`：整个报价下移 → 激进卖出、被动买入 → **净卖出方向**

两条活跃通道的语义因此都是正确的：

| 通道 | 公式 | 组合净空头 delta（-1050）时 | 降险动作 |
|------|------|------------------------------|----------|
| `delta_risk2` | `−sign(δ_leg) · normRiskDelta · cost_delta` | call：`+105·cost` → **上移**（激进买 call ✓）；put：`−105·cost` → **下移**（激进卖 put ✓，卖 put 增加正 delta） | 通过被动成交买入 delta ✓ |
| `vega_risk2` | `−vega_risk_norm · cost_vega`（不分腿——买入任何期权都增加 vega） | 净空头 vega → norm<0 → adj>0 → **全报价上移**（激进买入期权补 vega ✓） | 通过被动成交回补 vega ✓ |

**这两条通道不是死代码、方向也对——真正的问题在量纲（见 §3.2）与力度控制。**

## 2. 原版 quantbox 设计考察（事实）

### 2.1 三层结构

| 层 | 原版实现 | 状态 |
|----|----------|------|
| L1 归一化 | `pgn = (pg − risk_target) / risk_tol` → `NormRiskDelta` / `vega_risk_norm`（逐期权，含跨月相关性衰减 `exp(−λ·|δ0−δ1|)`、跨月 vega 目标扣减、wing 衰减 `exp(−λ_wing·|δ−0.5|)`） | ✅ 原版/现版均活跃 |
| L2 微调平移 | `delta_risk2 / vega_risk2`（fee 级平移）+ 交叉项 `h = I_v·I_d·sign(I_d)` 与 markup 封顶 `cov` | ✅ 原版/现版均活跃 |
| **L3 主偏移（二次型 shift）** | `s = −sign(pgn) · distort · pgn²`，clamp `±shift_max`；`delta_adj = s × δ_leg` → `adj` → 整体平移报价 | ❌ **原版即被注释禁用**（cc:2160-2168、2295），WtOptionCore 忠实继承了这个"死通道" |

### 2.2 原版 L3 的设计意图（从注释恢复）

```cpp
// pgn = (pg − risk_targ − delta_targ) / risk_tol          // 三项：当前暴露 − 风险目标 − alpha 期望 delta
// s = −sign(pgn) · distort · pgn²                          // 二次型：偏离越远偏移越陡
// delta_shift = clamp(s, ±shift_max)                       // 封顶
// ed0->setRiskShiftDelta(delta_shift)                      // 写回 ExpiryData → adj → 报价平移
```

- **二次型**：|pgn| 小（限内）时偏移≈0（不打扰正常做市），|pgn| 大时偏移快速加陡——天然的"软限渐进"。
- **`shift_max`**：偏移封顶（原版 lua `risk_tolerances` 表每个 greek 一组 `risk_tol / distort / shift_max`）。
- **风险目标**：`pgn` 减去 `risk_targ`（每到期月 delta/vega 目标，可由运营设定）——**平移不是把风险推向 0，而是推向目标位**。
- 原版还有 `max_risk_d / max_risk_v` 参数（"maximum delta/vega exposure allowed by alphas"）——alpha 侧硬限，现版未移植。

### 2.3 原版参数面

`CompositeOptionPricerParams.xml` 暴露：`risk_tolerances`（lua 表）、`max_risk_d/max_risk_v`、`hedge_ratio_delta/vega`、`lambda_vega_decay/vega_wing`、`sticky_base`、`improve_retreat_ratio` 等。现版把 `risk_tolerances` 表简化成了标量 `risk_prem_opt`（默认 0 → fallback `risk_tol = 10 / 1000`）。

## 3. 现状的真正病灶（量纲错配）

以 cu/202701 实测参数核算（`sprd_fwd=0.02`、`sprd_atmvol=0.15`、`sprd_fut=100`、fee≈2 元/手、tick=2 元）：

| 项 | 公式 | 数值 |
|----|------|------|
| `risk_tol`(delta) | fallback | **10**（应约 1,000 = max_delta 量纲） |
| `normRiskDelta` | `delta_eff / 10`，delta_eff≈−1,050 | **≈ −105**（设计应在 ±1 附近） |
| `delta_risk2` | `−sign(δ)·(−105)·(fee+0.005δ)` ≈ ±210 元 | **≈ ±105 个 tick** —— 报价被平移出市场 |
| `vega_risk_norm` | `−17,661 / 1,000`（fallback 1,000） | ≈ −17.7（同样放大） |
| `vega_risk2` | ≈ ±17.7 × (fee + vega×0.075) | 数百元级平移 |

**后果链**：报价被平移出市场 → 挂单永不成交（UI `bqp/aqp=9999`、日盘 51 信号 / 2 成交）→ "有偏移、无成交、无降险" → 风险只能靠 DeltaHedger 期货硬对冲（SimNow 资金不足全拒）→ vega 超限 -17.6k 挂 5 天。

其余确认的缺口（与上份审查一致）：`wgt_exp_ddelta` 未配置（ddAdj 关闭）、L3 主偏移通道断开、`limits breach → Panic` 四天 0 触发、`risk targets`（`setExpiryTargets` 已在初始/热参/UI cmd 三处接线，但 `targ_delta/targ_vega` 生产均为 0/None——目标位机制空转）。

## 4. 优化方案（借鉴原版 L3 + 修复量纲）

### 4.1 P0 —— 量纲校准（纯配置，无代码）

| 参数 | 现值 | 建议 | 理由 |
|------|------|------|------|
| `risk_prem_opt` | 未配置（fallback 10/1000） | **= max_delta（1000）** | `normRiskDelta` 回到 ±1 量纲；delta_risk2 平移回到 fee 级微调的本意 |
| `wgt_exp_ddelta` | 未配置（0） | 0.5~1.0 起试 | 启用期权层方向性 delta skew（与 delta_risk2 叠加） |
| vega 侧 | fallback 1000 | 与 max_vega 对齐 | 同上 |

> 注意：校准后 delta_risk2/vega_risk2 从"把报价推出市场"变为"fee 级微调"，需 replay 观察报价回市场后的成交分布。

### 4.2 P1 —— 恢复原版 L3 二次型主偏移（核心借鉴，需代码）

接通 `setRiskShiftDelta / setRiskShiftVega`（死通道正是为它预留的接口），公式完全沿用原版：

```
pgn  = (pg − targ) / risk_tol                        # targ 来自现有 targ_delta/targ_vega 热参（已接线）
s    = −sign(pgn) · distort_greek · pgn²             # 二次型，限内≈0、越限越陡
shift_greek = clamp(s, ±shift_max_greek)             # 封顶（tick 数或 theo 百分比）
```

- **delta**：`shift_delta` → `adj().delta_risk = shift × δ_leg`（call/put 自动反向平移，降 delta 方向被动成交）。
- **vega**：`shift_vega` → `adj().vega_risk = shift × vega_leg`（不分腿，买入方向被动成交）。
- **期货腿**：`risk_adjustment_future` 同步使用 `shift_delta`（现只挂 normRiskDelta），使期货 OQM 挂单与 DeltaHedger 同向渐进。
- **参数建议（cu 量纲）**：`distort_delta=0.3~1.0`、`shift_max_delta=2~5 tick`；`distort_vega=0.3~1.0`、`shift_max_vega` 以 ATM vol 的 0.5~1 tick 当量起试；均进热参面板（ParamRegistry，`perExpiry=false`）。
- 与 `ddAdj` 的关系：ddAdj 是"逐月净 delta 的线性 skew"，L3 是"组合风险目标的二次平移"——**保留两者，ddAdj 管月的相对倾斜、L3 管组合绝对风险**，先分别 replay 归因再决定合并。

### 4.3 P1 —— 软限分级（size 维度，借用 pgn 分级）

在 `__computeQuoteSize` 增加风险档位（参数 `risk_shrink_start=0.8`、`risk_shrink_end=1.0`）：

```
|pgn| < 0.8      → size 不变
0.8 ~ 1.0        → 开仓方向 size 线性收缩至 0（降险方向不变）
≥ 1.0            → 开仓方向 size=0，只保留降险方向挂单
```

（`max_pos_*` clamp 保留在外层。）

### 4.4 P1 —— risk target 打通（热参已接线，补语义）

`setExpiryTargets` 已在 `on_init`/热参/UI cmd 三处接线，但生产 `targ_delta/targ_vega` 均空 → pgn 退化为"推向 0"。补充：
- UI 面板对 `targ_delta/targ_vega` 的语义提示（"对冲目标位，风险平移的锚点"）；
- 初始值从 config 逐月 `targ_delta/targ_vega` 读入（config 已有键，生产为空 → 明确默认 0 即"推向 0"）。

### 4.5 P1 —— limits breach → Panic 链路修复（专项排查，独立于本方案但同优先级）

四天 0 触发的嫌疑点：panic 块嵌在 `enabled && marketFresh && shouldCompute` 三重门内，而 `checkRiskLimitsEx` 在门外每批置位——置位与评估的时间窗错配；以及 `_limitsBreached` 长期 true 时 `setState(Panic)` 反复调用被"状态未变化不打日志"掩盖的可观测性问题。修复后与 L3 分级衔接：`|pgn|≥1.2` 才全撤+Panic，0.8~1.2 交给 L3/size 分级。

### 4.6 P2 —— 可观测性

- UI 快照 per-expiry 暴露 `normRiskDelta / vega_risk_norm / shift_delta / shift_vega`（quantbox 原版有 `edpub.delta_shift` 显示语义）——运营可实时看到"偏移在多大程度上工作"。
- 恢复原版 TRACE 风格的 `trace-risk-adj-*` 日志（debug 级、按合约开关）。

### 4.7 验证方案

1. 单元测试：shift 公式纯函数化（`shift = clamp(−sign·distort·pgn², ±max)`），覆盖 pgn=0/±0.5/±1/±3；call/put 平移方向断言；clamp 边界。
2. replay：以 11-24/11-25 数据回放，对比三组（现状 / 仅 P0 校准 / P0+L3）的：报价 vs 市场 bqp 分布、被动成交的降险/增险占比、delta/vega 轨迹、期货腿手数。
3. 上线顺序：P0 配置 → 观察 1 个交易日 → L3 灰度（distort 从小起试）→ 软限分级。

## 5. 与现有机制的关系总结

| 机制 | 保留/调整 |
|------|-----------|
| `delta_risk2/vega_risk2`（fee 级微调平移） | 保留，P0 校准后回归本意 |
| `ddAdj`（逐月线性 skew） | 保留（P0 启用），与 L3 分工：月内相对倾斜 vs 组合绝对风险 |
| L3 二次型 shift | **新增接通**（借鉴原版，主偏移通道） |
| DeltaHedger 期货硬对冲 | 保留为最后手段；L3/期货腿偏移承担渐进降险 |
| 收盘 GetFlat 穿价 | 不变 |
| `max_pos_*` clamp / Panic / REJECT-ONLY 等 | 不变（Panic 修复独立专项） |
