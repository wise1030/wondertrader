# Panic 机制优化方案 —— 对标原版 quantbox（v4，2026-11-25）

> **实施状态（2026-11-25 15:08 部署上线）**：
> - ✅ P0：`panic_severity_ratio` 静态比值档已从 config/代码移除（循环瘫痪解除）
> - ✅ P1a：触发源收敛为 pricerPanic（毒性/PnL）+ checkerPanic（成交价）+ failStreak（降险失效）——持仓风险静态比值退出 panic
> - ✅ P1b：降险失效检测上线（`panic_fail_streak_sec: 120` / `panic_fail_rate: 0.8`）：超限 + 120s 无降险成交 + 降险单拒绝率 ≥80% → 才全停。SimNow 无资金环境下夜盘买入降险单会被拒 → 开盘 ~2 分钟后会按设计进入该 Panic（逼人工处置）；资金恢复后自动退出
> - ✅ P1c：pricer blackout 指数退避（300→600→…→1800s 封顶，重Arm在 blackout 内即升级）
> - ✅ P1d：panic 进入/退出联动 scanners 静默/恢复 + UI Alert（原因码）
> - 注意事故记录：13:5x 的 config patch 曾误吞 `pricer:` 段头行，导致 risk-shift 参数全部落在 delta_hedge 段成为死键（15:05:40 前的进程实际跑在 pricer 全默认状态）——15:08 修复并重启，0 未知键警告。另 14:14-14:21 的 PriceLimit REJECT 风暴实为「配置错位 → 量纲 fallback 生效」的组合产物，非 L3 参数生效（当时参数还是死键）。


> 前序：`panic-trigger-evaluation.md`（触发源评估）、`quote-shift-design-proposal-v3.md`（risk shift 主方案）。本文只针对 **Panic 机制**对标原版做优化设计。**未改任何代码；P0 为纯配置处置。**
>
> 当前实况：11-25 14:29 起 `limits runaway`（vega severity 1.79 ≥ ratio 1.2）触发 Panic，blackout 300s 后立即再触发——**循环瘫痪中**。

---

## 1. 对标结论：原版的三层语义分离

| 层 | 原版实现 | 触发对象 | 动作 | 恢复 |
|----|----------|----------|------|------|
| **Panic**（瞬时保护） | lua 配置的单一市场信号 × 阈值（`panic_thresh1/2`）→ `pricer.onPanic` | **市场环境冲击**（毒性流/波动异动，运营 lua 自定义） | 期权双边报价擦除；**主月期货对冲腿保留**（cc:1607）；scanners 静默 | `panic_blackout_interval`（300s）**自动恢复** |
| **持仓风险**（持续状态） | `risk_tolerances` → normRisk → **报价偏移**；`max_risk_d/max_risk_v`（alpha 暴露上限）；DeltaHedger | delta/vega/gamma 暴露 vs 限位/目标 | 渐进：偏移、缩量、对冲 | 持续运作，永不"停机" |
| **对冲兜底** | DeltaHedger（期货） | 偏移不足以降险时 | 期货硬对冲 | — |

**核心对标结论**：原版 panic 判定里**没有任何持仓风险项**；持仓风险的终点是"偏移拉满 + 对冲兜底"，不是停机。我们 v3 引入的 `runawayBreach`（`severity ≥ 1.2` 静态比值 → Panic）是原版没有的发明，且对一个 severity 长期 1.79 的账户构成循环瘫痪——**这是需要撤掉的对齐项**。

### 已天然对齐的部分（核查过，不需改）

| 项 | 原版 | 我们 |
|----|------|------|
| panic 时期权双边擦除 | `eraseBids/eraseAsks`（cc:1430） | `isPanicked()` → 同（cpp:1090 区域） |
| panic 时**期货对冲腿保留** | `exp != primary_exp` 才清（cc:1607） | `panicClear keeps hedges live by design`（B12，CTG:339/656） |
| blackout 自动恢复 | `panic_blackout_interval` 300s | 同参数 |
| 市场信号类触发源 | lua PanicSignal × 阈值 | `pricerPanic`（Toxicity/Pnl 信号）——精神对齐，可配置化程度较低（P2） |

---

## 2. 优化方案

### P0 —— 配置处置：解除当前循环瘫痪（纯配置，立即可做）

```yaml
delta_hedge:
    panic_severity_ratio: 999    # 1.2 → 999：静态比值档停用
```

- 效果：`runawayBreach` 实际不触发，循环瘫痪解除。
- **风控不裸奔**：以下全部保留并在工作——
  - `|pgn| ≥ 0.8` 期货兜底武装（bail_threshold_delta 250）
  - `|pgn| ≥ 1.0`（=RiskLimitsEx breach，同一归一）报价层**方向性抑制**：增险边 size→0、降险边保留、L3 偏移拉满
  - RiskLimitsEx 告警（每进程边沿一次）+ UI `limitsBreached` 显示
  - 毒性流/成交价异常/行情 stale 的 panic 三源不受影响
- 前提说明：severity 1.79 的根源是遗留 vega 库存（运营处置中，平仓清单已出）；在根源处置前，"1.2× 全停"惩罚的是一个已知且正被管理的状态。

### P1a —— 触发源重构（对标：持仓风险退出 panic）

移除 `runawayBreach`（静态比值 → Panic），panic 触发源收敛为：

| 源 | 保留/新增 | 理由 |
|----|-----------|------|
| `pricerPanic`（毒性流/PnL 信号） | 保留 | 原版 PanicSignal 的移植，市场环境冲击 |
| `checkerPanic`（成交价偏离） | 保留 | 原版没有但业务合理：成交价异常意味着定价/行情不可信 |
| `feedPanic`（新增，可选） | 行情数据异常升级 | 现 `marketFresh` 只停刷新不 panic；连续 stale 超阈（如 >5min）可升级——二期评估 |
| `runawayBreach`（静态比值） | **移除** | 原版无此物；静态比值惩罚持续状态；替代品见 P1b |

### P1b —— "降险失效检测"替代静态比值（业务增强，原版没有）

持仓风险的终极硬保护改为**动态失能检测**——只有当"降险通道被证明无法工作"时才全停：

```
触发条件（全部满足，持续 T=120s）：
  1. severity ≥ panic_severity_ratio（暴露仍超限）
  2. 降险方向腿的成交数为 0（fills == 0）
  3. 降险方向订单的失败率 ≥ 80%（reject / (orders+rejects)）——期货兜底拒单 + 期权降险挂单未成交
→ Panic（全撤 + 长歇，见 P1c）
```

- 语义："我们已经尽力降险（偏移拉满、兜底开火）但什么也成交不了，且暴露未回落"——这才是需要人介入的真失控。
- 遗留库存场景下：若资金恢复、降险买单能成交 → 条件 2 不满足 → 不 Panic，偏移继续工作 ✓；若资金枯竭且 severity 攀升 → Panic + UI 强告警 → 正确地逼人工处置 ✓。

### P1c —— 循环抑制（blackout 指数退避 + 强告警）

针对"300s 恢复 → 立即再触发"的循环：

- Panic 进入时记录触发签名（原因 + severity 快照）；
- blackout 到期重评时，若**触发条件未解除**：blackout 时长 ×2（300→600→1200→2400，封顶 1800s），每次升级打 LL_ERROR + UI Alert（分级告警已有 pushUiAlert 通道）；
- 条件解除后回到 300s。
- 效果：把"每 300s 循环瘫痪"变成"指数拉长的休眠 + 持续强告警"——策略不可用性被诚实地暴露给运营，而不是刷屏循环。

### P1d —— panic 动作补齐对标（小项）

- **scanners 静默**：原版 panic 时 `scanner->onPanic`；现版 `_scanCoord` 与 panic 无联动。补：panic 进入时 `stopAll()`、恢复时 `startAll()`（扫描器在 blackout 期发出的组合信号无人消费，静默是正确语义）。
- **UI Alert**：panic 进入/退出走分级告警通道（含原因码与 severity 快照）。

### P2 —— panic 信号可配置化（长期，可选）

原版的 panic 源是 **lua 信号**（运营自定义市场异常定义）。现版三源硬编码。长期方向：把"市场异常"定义做成可配置信号链（如：波动率跳变 / 成交强度 / 盘口消失的任意组合），运营按品种/时期调整——这是原版设计最值得借鉴的扩展点，但优先级低于 P0/P1。

---

## 3. 参数表（P0 配置 + P1 新增）

| 参数 | 值 | 语义 | 层 |
|------|-----|------|-----|
| `panic_severity_ratio` | **999**（P0 过渡值） | 静态比值档停用（P1a 移除后此键废弃） | config |
| `panic_fail_streak` | 120s / 80% / fills==0 | 降险失效检测（P1b） | 代码（新） |
| `panic_backoff_base` / `panic_backoff_max` | 300s / 1800s | 指数退避（P1c） | config→热参 |
| `panic_blackout_interval` | 300s（不变） | 基础 blackout | 已有 |
| `bail_pgn` / `bail_threshold_delta` | 0.8 / 250 | 期货兜底（已上线） | 已有 |
| `risk_shrink_start` | 0.8 | 方向性抑制（已上线） | 已有 |

## 4. 验证

1. 单测：降险失效判定纯函数（fills/rejects/severity 组合）；退避序列。
2. replay（11-24/25 数据）：确认遗留超限库存下不再进入 Panic 循环；方向性抑制/期货兜底行为不变；注入模拟毒性流 → pricerPanic 正常触发与恢复。
3. SimNow：解除瘫痪后观察夜盘/日盘——偏移报价位置、兜底触发、方向性抑制的成交统计。

## 5. 明确不做

1. **不把持仓风险以任何静态比值形式接回 panic**（对标核心）。
2. 不在 panic blackout 内保留期权单边报价（原版也是全擦）——方向性抑制由 size 分级层在 Panic 之前的 0.8~1.0 区间完成。
3. 不做 lua 信号引擎移植（P2 的可配置信号链走 ParamRegistry/配置文件，不引入 lua）。
4. `max_risk_d/max_risk_v`（alpha 硬限）维持不移植——RiskLimitsEx 已覆盖同语义。

## 6. 与已上线机制的关系

| 机制 | 状态 | 在新架构中的位置 |
|------|------|------------------|
| L3 二次型 shift（delta/vega） | 已上线（量纲已修） | 主力降险通道（常开） |
| 方向性 size 抑制（0.8~1.0→1.0） | 已上线 | 持仓风险分级的核心 |
| 期货兜底（bail_pgn 0.8 / 250） | 已上线 | delta 兜底 |
| REJECT-ONLY 释放、Passivity cap、sticky 降权、teenie | 已上线 | 做市基础 |
| runawayBreach（静态比值→Panic） | **本方案移除**（P0 先配置停用） | 被 P1b 降险失效检测替代 |
| panic blackout 循环 | **本方案改造**（P1c 指数退避） | — |
