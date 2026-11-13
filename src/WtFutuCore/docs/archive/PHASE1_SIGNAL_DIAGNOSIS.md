# 阶段 1 排查结果 — LeadLag + TradeFlow 根因

> 2026-06-21

---

## 一、LeadLag 全零 — 参数不适配（非数据连接问题）

### 1.1 排查结论

handleLeadLagPush 被正确调用（LEADLAG_DBG 确认），数据链路通。
LeadLag 的 result.valid 有时 true 有时 false（取决于 anchor tick 是否到过）。
alpha 有值但极小（0.0008-0.002），4 位小数显示为 0.0000。

### 1.2 根因

LeadLagSignalSource::calculateSignal() L191:
```cpp
total_signal += info.mid_change * weight * 100.0;  // Scale for range
```

EC mid~3000-3700，一个 tick 0.5：
  mid_change = 0.5 / 3700 = 0.0135%
  × 100 = 0.0135
  tanh(0.0135) = 0.0135

经过 RingBuffer 50 窗口加权平均后更小 → 0.001-0.002

### 1.3 修复方向

不是信号逻辑错，是缩放因子不适配 EC 的价格水平。
  方案 a: 改缩放因子从 100 到 10000-50000（适配 EC 的 mid 量级）
  方案 b: 改成 bps（基点）缩放: mid_change_bps = (mid-change)/mid × 10000
  方案 c: 用绝对价格变化的 tick 数代替百分比变化

### 1.4 valid 闪烁

calculateSignal 只在 updateLeadContract(anchor tick) 时调用。
非 anchor tick 时 result 保持上次值（正确行为）。
但每个合约的 aggregator 独立，anchor 合约自己的 LeadLag 没有 lead → valid=false。
非 anchor 合约在 anchor tick 到达后 valid=true。

---

## 二、TradeFlow IC=-0.83 — Lee-Ready bid-ask bounce 反向

### 2.1 排查结论

TradeFlow 信号从 TickTransactionInferer::inferFromTick 获取方向。
方向判定逻辑（detectMethod L200-208）：

```cpp
if (last_price >= ask_px - tick_size*0.1) → SPREAD_CROSS_UP (买方发起)
if (last_price <= bid_px + tick_size*0.1) → SPREAD_CROSS_DOWN (卖方发起)
```

### 2.2 根因：Lee-Ready bid-ask bounce bias

EC spread 窄（1-2 tick），last_price 几乎总等于 bid1 或 ask1。
价格在 bid/ask 间跳动（bid-ask bounce）：
  tick N: last=ask → 判定"买方发起" → net_flow +
  tick N+1: last=bid → 判定"卖方发起" → net_flow -

但 bid-ask bounce 是均值回复的：
  在 ask 成交后（判"买方"），下一步大概率回 bid → 价格下跌
  → "买方发起"预测了下跌 → 反向指标

这是经典 Lee-Ready tick rule 在高频窄 spread 下的已知缺陷。

### 2.3 修复方向（3 种可选）

**方案 a: 加延迟 (Lee-Ready 修正)**
  不用当前 tick 的 last_price 判方向，用 N tick 延迟后的价格验证。
  如果 N tick 后价格上涨 → 确实是买方发起。
  这正是我们 IC 分析验证的：trade 信号 vs N tick 后价格变动。

**方案 b: 量加权方向判定**
  不用单个 tick 的价格位置，用 tick volume 增量加权。
  大单在 ask 成交 → 更确信是买方发起。
  小单在 ask 成交 → 可能只是 bounce，低置信度。

**方案 c: 用盘口变化替代价格位置**
  不看 last_price vs bid/ask，看 bid_vol/ask_vol 的变化。
  ask_vol 减少 → 卖方被吃 → 买方发起（更直接）。
  这其实就是 OFI 的逻辑 → 与 OFI 高度相关。

### 2.4 推荐

方案 b（量加权）最符合做市逻辑：
  - 大单在 ask 成交 = 真实买方意图（不是 bounce）
  - 小单跳动 = 噪声，低置信度
  - 保留 TradeFlow 的独特价值（区分大单 vs 小单），不退化为 OFI

---

## 三、对权重框架的启示

### 3.1 TradeFlow 不是"信号无效"，是"方向推断方法不适配"

  - Lee-Ready tick rule 在窄 spread + bid-ask bounce 下反向
  - 这是方法问题，修复后 TradeFlow 的 IC 可能从 -0.83 变正
  - 权重框架的 floor 保护确保修复前不会完全归零

### 3.2 LeadLag 不是"信号无效"，是"缩放因子不适配"

  - 信号链路通，alpha 有值但极小
  - 调整缩放后 LeadLag 可能成为 EC 最有效的信号（跨期领先）
  - EC 是跨期品种，LeadLag 理论上最有预测力

### 3.3 验证了"IC 低 ≠ 信号无效"的框架原则

  - 两个信号 IC 低都有明确的参数/方法原因
  - 修复后需要重新评估 IC
  - 权重框架应在参数修复后再评估各信号的可信度

---

## 四、下一步

1. 修 LeadLag 缩放因子（简单参数调整）
2. 修 TradeFlow 方向推断（量加权 or 延迟验证）
3. 重跑 IC 验证，确认修复后各信号的真实预测力
4. 基于修复后的 IC 设计权重框架的初始权重
