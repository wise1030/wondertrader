# Risk-Shift 对冲机制审查（2026-11-25）

> **审查目标**：验证"标的对冲应通过 risk shift（报价偏移 + 被动挂单成交）在 delta/vega 上主动降险"这一业务逻辑在当前策略中的实现情况、有效性与改善空间。**只分析，未改任何代码。**
>
> 数据来源：`src/WtOptionCore`（CompositeOptionPricer / OptionQuoteManager / DeltaHedgeExecutor.h / OptionHedgeCoordinator / RiskLimitsEx / HftOptionStrategy）、生产 `config.yaml`、11-24/11-25 运行日志。

---

## 1. 期望的业务语义（基准）

1. 对冲的本质是**风险暴露过大时的主动降险过程**，不是例行程序。
2. 首选手段是 **risk shift**：当 delta/vega 向限位漂移时，报价整体向"降风险方向"偏移——
   - 降风险一侧的挂单更激进（更容易被动成交掉风险）；
   - 增风险一侧的挂单更被动（不容易接新的风险）；
   - size 随风险接近限位收缩。
3. 期货硬对冲（主动单）是 risk shift 不足以降险时的**最后手段**。

## 2. 现状实现盘点（代码地图）

### 2.1 期权报价层 `CompositeOptionPricer::computeOurMarkets`（:918 起）

```
bid_spread = max(0, spread − alpha − adj − ddAdj)
ask_spread = max(0, spread + alpha + adj + ddAdj)
```

| 通道 | 设计意图 | 实际状态 |
|------|----------|----------|
| `alpha`（信号层） | 流向/波动信号驱动的双边缩放 | ✅ 工作（wgt_* 已配置） |
| `adj().delta_risk = shift × og` | 逐期权 delta 风险偏移 | ❌ **死代码**：`setRiskShiftDelta` 全库无调用点，恒 0 |
| `adj().vega_risk = shift × og` | 逐期权 vega 风险偏移 | ❌ **死代码**：`riskShiftVega` 无任何写入点，恒 0 |
| `adj().delta_risk2 = −sign(og)·normRiskDelta·cost` | cross-expiry 归一化偏移 | ⚠️ 有值但**量级 ≈ 手续费**（cost = fees + 0.5·fwd_spread·δ），相对权利金（数千元）可忽略，tick 取整后基本被抹掉 |
| `adj().vega_risk2 = −vega_risk_norm·cost_vega` | 同上（vega 版） | ⚠️ 同上，量级不足 |
| `ddAdj = −wgt_exp_ddelta · expNetDelta · sign(cDelta)` | **方向性 delta 偏移**：净多头月 → call ask 更激进（被动卖出降 delta）、put bid 更激进（被动买入降 delta） | ❌ **生产未配置**（`wgt_exp_ddelta` 缺省 0），整个方向性通道关闭 |

**结论：期权报价层当前没有任何生效的方向性 risk shift。** 双边同向的 `alpha/adj` 只做"风险高→双边后退"的对称缩放，不产生"吸引降风险成交"的单边激励。

### 2.2 期货腿报价 `computeOurMarketsFuture`（有方向性，但量级失控）

```cpp
adj = −normRiskDelta × |0.5 × futureSpread|
ask_spread += adj;  bid_spread −= adj;
```

- 方向正确：净空头 delta（需买期货降险）→ bid 更激进（贴 mid）、ask 高挂。
- **量级失控**：`normRiskDelta = delta_eff / risk_tol`，`risk_prem_opt` 未配置 → fallback `risk_tol=10`（该默认适配 CFFEX IO 的 300× 量纲）。cu 的 `delta_eff` 达 ±1,000+ → normRiskDelta ≈ ±105 → adj ≈ ±5,250 元，而 `futureSpread`（sprd_fut=100）一半才 50 元。**偏移比 spread 大两个数量级**：实际效果退化为"bid 恒贴 mid、ask 恒高挂"（或反向），不是连续可调的偏移。

### 2.3 期货硬对冲 `DeltaHedger`（DeltaHedgeExecutor.h + OptionHedgeCoordinator）

- 阈值触发（`hedgeDeltaThreshold=30`）→ 多腿（每月 exclusive + shared residual，双层不重复对冲）→ cooldown 3s → reject backoff 2^streak（封顶 32×）→ in-flight 跟踪防重复。
- **价格 = mid 被动挂单**（`fireLeg` 无 override 时 `m_price` 取 `utd->getMid()`）——已经是被动模式；只有收盘 flatten（GetFlat）才穿价（`flatten_aggr_ticks=2`）。
- **只对冲 delta**。vega 超限没有任何对冲/处置路径。
- size = `ceil(|excess|/deltaPerLot)` 封顶 `max_order_size=10`。

### 2.4 风险限位层 `checkRiskLimitsEx` → Panic

- `max_delta/max_vega/max_loss` REJECT → 置 `_limitsBreached` → `on_batch_complete` 理论上 `setState(Panic)`（撤双边、Panic blackout 300s 后重评）。
- **实测：11-22 至 11-25 四天 `-> Panic` / `Auto-panic` 日志 0 次**，而 vega -17,661 持续 > limit 10,000（`limitsBreached=true` 在 UI 快照可见）。**该链路在生产从未生效，原因待专项排查**（panic 块嵌在 `enabled && marketFresh && shouldCompute` 三重门内；`checkRiskLimitsEx` 在门外每批置位——两者时间窗错配是首要嫌疑）。
- 无软限分级：只有"未超限=正常报价 / 超限=应全停"两态，缺少 80%/100% 渐进降险。

### 2.5 size 维度

`__computeQuoteSize` 只做 `max_pos_stk/max_pos_opt` 持仓 clamp；**没有风险接近度（normRiskDelta/normVega）驱动的 size 收缩**。

## 3. 有效性评估（运行证据）

| 证据 | 说明 |
|------|------|
| vega -17.6k 连续 5 天 > 10k 限 | vega 方向性偏移（死代码）+ vega 对冲（不存在）+ Panic（失效）三重缺口叠加 → **vega 超限既不自我修复也不停机，长期裸奔** |
| DeltaHedger fired 336 次（11-24）/ 140 次（11-25）× 10 手 | 期货硬对冲是当前唯一生效的降险手段；SimNow 资金不足下全部被拒（error 31），delta 裸露无替代路径 |
| 00 时段 6,279 条期权信号双向成交 | 报价层无净 delta 归零倾向（ddAdj=0），双边成交持续制造新的 delta 失衡，全部推给期货腿 |
| 期货腿 adj 量级失控 | futures OQM 的挂单（max_pos_fut=2）与 DeltaHedger 并存，偏移失控时可能把期货腿报往极端价格 |
| 日盘期权报价恢复（11-25 09 时段 51 信号） | 基础做市正常，但降险通道缺失 |

## 4. 结论

**骨架存在、方向设计正确，但生产实际处于失效状态。** 具体分三档：

1. **完全失效（死代码/未配置）**：期权层方向性 delta 偏移（ddAdj 未配置）、逐期权 delta/vega risk shift（无写入点）、vega 降险（全链路缺失）、limits→Panic（0 触发）。
2. **方向对但不可用**：期货腿方向性偏移（risk_tol fallback 与 cu 量纲错配，偏移量级失控）。
3. **正常工作**：DeltaHedger 触发/冷却/退避/in-flight、收盘 flatten 穿价、alpha 信号、position clamp。

即：**当前系统对 delta 的降险手段只剩期货硬对冲，对 vega 的降险手段为零**——与"通过报价偏移被动降险"的目标语义相距最远的就是 vega，其次是期权的 delta 偏移。

## 5. 改善建议（按不动代码→动代码分层，均未实施）

### A. 配置级（不改代码，立即可做）
1. **启用 ddAdj**：pricer 段配置 `wgt_exp_ddelta`（建议从 0.5~1.0 个 spread 当量起试）——打开期权层方向性 delta 偏移。
2. **校准 `risk_prem_opt`**：让 `normRiskDelta = delta_eff / risk_tol` 落在 0~1 量级（建议 `risk_tol ≈ max_delta`，即 1,000；当前 fallback 10 使其放大 100 倍）。同时校准期货腿偏移幅度。
3. `hedge_ratio_delta/vega`、`lambda_vega_decay/wing` 目前全 0，`updateRiskShiftsVega` 的相关性/目标项实际不参与——校准或明确放弃该通道。

### B. 逻辑级（需授权后实施）
1. **vega 方向性偏移**：仿照 ddAdj 增加 `wgt_exp_vega` 通道（净 vega 多头 → 卖 vega 侧 ask 激进/买 vega 侧 bid 被动），或直接在 vol 曲线 atmVol 上做整体 shift——这是当前缺口最大的通道。
2. **清理死通道**：`delta_risk/vega_risk`（无 setter）要么接上 normRiskDelta 驱动、要么删除，避免"看似有三层偏移、实际只有零层"。
3. **修复 limits breach → Panic 链路**（0 触发专项排查），并增加**软限分级**：|风险| ≥ 80% 限位开始渐进偏移+缩 size；100% 停开仓方向双边；120% 全撤+Panic。
4. **DeltaHedger 失败回退**：连续 error-31/权限拒绝时，把对冲意图转成期权层偏移增强（ddAdj 权重临时加倍），而不是空转等待 backoff。
5. **size 风险收缩**：`__computeQuoteSize` 增加"开仓方向 size 随 normRisk 线性收缩至 0"。

### C. 验证
- replay/backtest 对比 ddAdj 与 vega 偏移开关下的 delta/vega 轨迹、被动成交占比（降险成交 vs 增险成交比）、期货腿手数。

---

### 附：本次审查涉及的关键代码锚点

| 机制 | 位置 |
|------|------|
| 期权 spread 合成（alpha/adj/ddAdj） | `CompositeOptionPricer.cpp:918-965` |
| `__computeQuoteSize`（持仓 clamp） | `CompositeOptionPricer.cpp:810-822` |
| risk_adjustment（delta_risk/vega_risk/交叉项） | `CompositeOptionPricer.cpp:1600-1650` |
| updateRiskShiftsDelta（normRiskDelta） | `CompositeOptionPricer.cpp:1366-1389` |
| updateRiskShiftsVega（vega_risk_norm） | `CompositeOptionPricer.cpp:1393-1543` |
| 期货腿方向性偏移 | `CompositeOptionPricer.cpp:computeOurMarketsFuture` + `risk_adjustment_future` |
| DeltaHedgeExecutor::decide（阈值/数量/冷却） | `DeltaHedgeExecutor.h:36-72` |
| fireLeg（mid 被动价 + reject backoff） | `OptionHedgeCoordinator.cpp:35-105` |
| 收盘 flatten 穿价 | `HftOptionStrategy.cpp:4777-4808` |
| limits→Panic 块 | `HftOptionStrategy.cpp:2085-2102`（0 触发待查） |
| 生产配置缺口 | `config.yaml` pricer 段（无 `wgt_exp_ddelta`/`risk_prem_opt`/`hedge_ratio_*`/`lambda_vega_*`） |
