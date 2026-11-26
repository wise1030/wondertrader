# Panic 触发机制评估 —— 对照原版 quantbox（2026-11-25）

> 背景：11-25 14:29 策略进入 Panic（`limits runaway`，vega severity 1.79 ≥ panic_severity_ratio 1.2），全撤+停报并随 300s blackout 循环。本文对照原版 quantbox 的 panic 设计，评估"持仓风险是否应触发 Panic"。**未改任何代码。**

---

## 1. 原版 quantbox 的 panic 设计（事实）

### 1.1 触发源：唯一的、市场信号型的

`OptionTraderStrategy.cc:419-448`：

```cpp
void OptionTraderStrategy::onSignalChanged( const signals::ISignal& signal ) {
    const std::vector<double>& sigs = signal.getSignalState();
    double sig1 = sigs[1];
    double sig2 = sigs[1] + sigs[2];
    bool bCond1 = abs(sig1) > m_prop_panic_thresh1;
    bool bCond2 = abs(sig2) > m_prop_panic_thresh2;
    if (bCond1 || bCond2) {
        if (!m_spOptionPricer->isPanicked())
            m_spOptionPricer->onPanic(signal);        // ← panic 唯一入口
        BOOST_FOREACH (const IScanModulePtr& scanner, m_scanners)
            scanner->onPanic(signal);
    }
}
```

- 信号由 `config().panic_signal`（**lua 配置的单一信号**，用近月合约构建，`OptionTraderStrategy.cc:260-271` 注册为 "PanicSignal"）——**运营自定义的市场环境/成交行为类信号**（波动跳变、成交冲击、盘口异动一类）。
- **触发源清单里没有持仓风险**：delta/vega/gamma 限位、持仓集中度、资金占用——在原版从不进入 panic 判定。

### 1.2 panic 的动作：pricer blackout，而非状态机停机

| 维度 | 原版行为 | 说明 |
|------|----------|------|
| 作用层 | **pricer 层**（`isPanicked()` 布尔） | 不动交易状态机、不撤已挂单 |
| 期权腿 | `eraseBids()+eraseAsks()`（cc:1430-1440） | 期权双边报价擦除，blackout 期间不重挂 |
| **期货腿** | `isPanicked() && exp != primary_exp` 才清（cc:1607）——**主月期货对冲腿保留活跃** | 注释原文："leave only primary hedge future on when panicking"——**panic 时保住对冲能力** |
| 持续期 | `panic_blackout_interval`（现配置 300s）后**自动恢复** | 瞬时保护语义，非持续状态管理 |
| scanners | 同步 `scanner->onPanic` | 扫描器一并静默 |

### 1.3 持仓风险在原版的归宿（与 panic 无关的三层）

1. `risk_tolerances`（risk_tol）→ `normRisk/vega_risk_norm` → **报价偏移**（risk shift，含被注释的二次型主通道）——"继续报价，但往降险方向倾斜"；
2. `max_risk_d / max_risk_v`（Params.xml："maximum delta/vega exposure allowed by **alphas**"）——alpha 层暴露上限；
3. DeltaHedger 期货对冲——delta 的硬对冲。

**原版哲学：持仓风险是"持续状态"，用报价行为与对冲渐进管理；panic 是"市场环境极端"的瞬时保护（blackout 后自动恢复）。两者语义正交。**

---

## 2. 我们系统当前的 panic（v3 实施后）

### 2.1 触发源三支

| 源 | 对应原版 | 触发条件 | 今日状态 |
|----|----------|----------|----------|
| `pricerPanic`（毒性流/PnL 信号 blackout） | ✅ 原版精神（PanicSignal 的移植） | RiskSignals 超阈 | 未触发 |
| `checkerPanic`（成交价偏离） | 原版没有（新增，合理） | FillPriceChecker | 未触发 |
| `runawayBreach` = `_limitsBreached && severity ≥ 1.2` | ❌ **原版没有**——持仓风险触发 panic 是我们的新发明 | vega -17.9k / 10k → severity 1.79 | **今日 14:29:10 触发，持续中** |

### 2.2 动作比原版重得多

| 维度 | 原版 | 我们 |
|------|------|------|
| 作用层 | pricer 布尔（报价擦除） | **交易状态机 Panic**：`enabled` 语义变化、CTG 全撤、双边停报 |
| 期货腿 | **保留主月对冲腿** | 全撤（方向性抑制也不区分——Panic 优先级高于一切） |
| 持续期 | 300s 自动恢复 | blackout 300s 后 severity 仍 1.79 → **立即再触发，循环瘫痪** |

### 2.3 今日事故复盘

```
14:29:10  RiskLimitsEx BREACH: max_vega -17893 / 10000     # severity = 1.79（长期常态，已挂 5 天）
14:29:10  Auto-panic (limits runaway) → Panic, 全撤
之后      blackout 300s → severity 不变 → 立即再触发 → 循环瘫痪
```

**为什么以前不发生**：旧 panic 块嵌在 `shouldCompute` 门内从未执行（四天 0 触发的失效链路）。今天 14:24 部署的 hoist 修复让评估每批执行——**修复把"失效的过严参数"激活成了"生效的瘫痪开关"**。1.2 的比值对一个长期 1.79× 的账户，等于永久瘫痪开关。

---

## 3. 评估：持仓风险是否应该触发 Panic？

### 3.1 理论辨析

Panic 的本质是"**环境异常到无法安全报价**"——它的对象是**外生冲击**（市场崩动、毒性订单流、成交价异常、数据源故障），特征是**瞬时、不可预测、blackout 后环境可能已恢复**。

持仓风险的特征完全不同：**内生、连续、由已成交/已持仓的历史决定**。它：
- 不会在 300s blackout 内自行消失（库存还在，severity 不变）；
- 对它正确的响应是**渐进管理**（偏移报价降险 + 期货兜底 + 停止增加），而不是"闭眼暂停"；
- 全停恰恰**锁死降险通道**——降险腿也被撤了，风险僵在原地；这正是 v3 分级设计用"方向性抑制（停增险边、保降险边）"替代全停的原因。

### 3.2 结论

**持仓风险不应该（直接、按静态比值）触发 Panic。** 理由：

1. **原版设计佐证**：quantbox 的 panic 判定里没有任何持仓风险项；持仓风险走 risk shift + 对冲 + alpha 硬限，语义正交。
2. **业务语义**：用户定义的降险路径就是"报价偏移 + 被动成交"，持仓风险越高偏移越强——这是**常态运作的一部分**；Panic 会终止这个机制本身。
3. **工程语义**：Panic = 环境不可知的瞬时保护（blackout 自恢复）；持仓风险 = 已知、连续、可管理的状态。用瞬时机制管持续状态必然产生"300s 循环瘫痪"（今日实况）。

**但持仓风险也不能没有硬保护。** 正确的硬保护形态是"**降险失效检测**"而非"静态比值"：
- 现有分层已经覆盖：`|pgn| ≥ 0.8` 期货兜底武装、`≥ 1.0` 增险边 size=0（报价层方向性抑制）、偏移拉满；
- 真正需要 Panic 的残余场景是：**降险通道失效 + 暴露仍持续扩大**（如期货兜底连续拒单 + 降险挂单连续无法成交 + severity 仍上行）——这是"动态失能检测"，不是"静态比值"。

### 3.3 今日 Panic 的定性

`panic_severity_ratio=1.2` 对一个 **severity 长期 1.79**（遗留 ITM put 空头库存 + `enable_auto_close: false`）的账户，是**必然误触发**：它惩罚的是一个已知、已被分级机制管理、且根源在运营处置（平仓清单/资金）的状态。属设计参数与业务现状错配，不是策略失控——riskmon 的涨停价检查、方向性抑制、期货兜底都在正常工作。

### 3.4 处置选项（均未实施）

| 选项 | 动作 | 代价/风险 |
|------|------|-----------|
| **A（推荐）** | config 上调 `panic_severity_ratio`（如 999，实际停用静态比值档），重启生效 | runaway 全撤停用；但 0.8 期货兜底、1.0+ 方向性抑制、RiskLimitsEx 告警、偏移拉满**全部保留**——风控不裸奔，只是不再用"全停"惩罚已知状态 |
| B | 维持现状 | 策略 300s 循环瘫痪，夜盘/日盘不可运行 |
| C（治本，独立） | 执行平仓清单压降 vega 库存，severity 回 <1.2 后可再收紧 ratio | 需资金/人工 |
| D（二期代码） | 把静态比值档改为"降险失效检测"（severity > ratio **且** 降险腿连续 N 次无法成交） | 需代码与状态跟踪 |

> 注：`panic_severity_ratio` 目前是 config 项（重启生效，非热参）；若选 A 后希望长期可调，二期把它挪进 ParamRegistry 热参面板。

---

## 4. 附：原版/现版 panic 机制对照速查

| 项 | 原版 quantbox | 现版（v3 后） |
|----|---------------|---------------|
| 触发源 | lua 配置的单一市场信号 × 2 阈值（OptionTraderStrategy.cc:419-437） | pricerPanic（毒性/PnL）+ checkerPanic（成交价）+ runawayBreach（持仓比值，**新增、原版无**） |
| 作用层 | pricer 布尔（报价擦除） | 交易状态机 Panic（全撤+停报） |
| 期货腿 | 保留主月对冲腿（cc:1607） | 全撤 |
| 持续期 | blackout 300s 自动恢复 | 300s 后 severity 不变 → 循环 |
| 持仓风险 | 不触发；走 risk shift + max_risk_d/v + DeltaHedger | **触发（新增）** ← 与原版哲学相悖 |
