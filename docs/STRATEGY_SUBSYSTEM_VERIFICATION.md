# 策略子系统验证报告 — skew / toxicity / alpha

> 2026-06-21 | 对 EC_BACKTEST_ANALYSIS.md 三个 P0 结论的复核

---

## A. Skew 方向 — ❌ 之前诊断错误，skew 方向 100% 正确

### 复核方法

之前误读了 QUOTE 日志格式。日志 `skew={:.2f}(delta={:.2f})` 里的 `delta` 字段是
**delta_skew（skew 计算结果）**，不是持仓 delta。

重新用 position updated 日志重建真实 net_position（long-short），与 QUOTE 的
skew 关联分析。

### 结果

```
ec2607 有持仓样本: 73 (另外 124193 flat)
  skew 方向正确: 73 (100.0%)
  skew 方向错误: 0 (0.0%)
```

具体例子:
```
net_pos=-3  → skew=+0.03  ✓ (空头时正，抬高bid鼓励买回平空)
net_pos=+5  → skew=-0.06  ✓ (多头时负，压低ask鼓励卖出平多)
net_pos=-5  → skew=+0.06  ✓
```

computeContractDeltaSkew 代码逻辑 L238:
```
direction = (utilization > 0) ? -1.0 : 1.0  ← 正确
```

### 真正的问题（不是方向，是幅度+频率）

  - skew 非零率仅 21.9%（ec2607）/ 3.7%（ec2609）→ 大部分时间无持仓，skew 不工作
  - 持仓量极小（1-5 手），skew 幅度只有 0.02-0.06
  - 根因：成交后立即被 stra_buy/stra_sell 的 CLOSET 子单平掉，持仓时间极短

---

## B. Toxicity 参数 — 阈值严重过高

### VPIN 计算

PredictiveToxicity.cpp:
```
VPIN = sum(|buy_vol - sell_vol|) / (num_buckets × bucket_size)
```
- bucket_size=1000（每次累积 1000 手成交封一个桶）
- window=50（维护最近 50 个桶）

### toxic_score 计算

```
combined_score = 0.5 × VPIN + 0.5 × alpha_toxicity
alpha_toxicity = ofi_weight × |ofi| + trade_weight × |imbalance_ratio|
```

### EC 实际值 vs 阈值

```
                  EC实际max    阈值      倍数
VPIN              0.069       0.7       10x 过高
toxic_score       0.183       0.7       4x 过高
```

### 阈值问题

- ToxicFlowDetector.h:60 代码默认 `vpinThreshold=0.10`
- 但 _ec_5d.yaml 配了 `vpinThreshold: 0.7` → 覆盖了默认值
- `is_toxic = combined_score > alpha_threshold(=adverseThreshold=0.7) || vpin > vpin_threshold(=0.7)`
- 两个 0.7 阈值都远超 EC 的 max 值（0.183/0.069）

### 根因

配置文件的阈值（0.7）是从 ao 场景拷贝过来的，在 EC 上完全不适用。
EC 的 VPIN 天然比 ao 低（成交更均衡），0.7 阈值让毒性保护永远不触发。

### 建议

- VPIN 阈值降到 0.05-0.08（基于 EC max=0.069）
- adverseThreshold 降到 0.12-0.15（基于 EC max=0.183）
- 或改成动态阈值（95th percentile）

---

## C. Alpha 准确率 — 统计门槛过高，不是信号无效

### 计算逻辑

PerformanceAnalyzer.cpp L68:
```cpp
if (std::abs(trade.alpha_at_trade) >= _config.strong_alpha_threshold) {
    _alpha_signals++;
    bool alpha_correct = (alpha > 0 && is_buy) || (alpha < 0 && !is_buy);
    ...
}
```

### 阈值问题

- `strong_alpha_threshold` 默认 = **0.7**（PerformanceAnalyzer.h:211）
- `alpha_at_trade = sc.alpha.alpha`，实际范围 0~0.63
- `abs(alpha) >= 0.7` **永远不满足**（max 0.63 < 0.7）
- → `_alpha_signals` 始终 = 0 → `alpha_accuracy = 0/0 → 默认 0`

### 根因

alpha_acc=0% **不是 alpha 信号无效**。是 PerformanceAnalyzer 的 strong_alpha_threshold=0.7
门槛过高，没有任何成交的 alpha 达到统计门槛，导致 `_alpha_signals=0`，分母为 0。

### alpha 信号本身

- alpha avg=0.32, max=0.63, 47% 看多 / 46% 看空 → 分布合理
- conf avg=0.26, 36% 高信心 → 有一定置信度
- 但**预测力未验证**（因为统计门槛过高无法计算准确率）

### 建议

- strong_alpha_threshold 降到 0.2-0.3（基于 alpha 分布）
- 或改成百分位动态阈值
- 降后才能判断 alpha 信号是否真有预测力

---

## 总结修正

| 原诊断 | 复核结论 | 真问题 |
|--------|---------|--------|
| skew 方向反了 (P0-1) | **❌ 错误** skew 方向 100% 正确 | 幅度太小 + 持仓时间极短 |
| alpha 完全无效 (P0-2) | **部分错误** 不是信号无效 | 统计门槛 0.7 过高 (max alpha=0.63) |
| toxicity 不触发 (P0-3) | **✓ 正确** 阈值 0.7 远超 EC 实际值 | yaml 阈值从 ao 拷贝，EC 需 0.05-0.15 |

### 统一规律

三个问题里有两个是**阈值配置问题**（0.7 硬编码/default）：
- toxicity vpinThreshold=0.7（yaml 配置）
- toxicity adverseThreshold=0.7（yaml 配置）
- alpha strong_alpha_threshold=0.7（代码默认）

0.7 这个值在 EC 上统一过高。EC 的 VPIN/score/alpha 天然比 0.7 低一个数量级。
