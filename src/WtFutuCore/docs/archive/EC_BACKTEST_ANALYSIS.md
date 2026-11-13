# EC 5天回测策略分析 — skew/alpha/toxicity/spread 诊断

> 2026-06-21 | UftMocker 框架 bug 全部修复后的回测基线
>
> 标的: ec2607/ec2608/ec2609, 5 天 (20260608-20260612), info+debug 级
>
> 状态: **诊断报告,不动代码**

---

## 一、PERF 总表(累积值,PerformanceAnalyzer 不按 session 重置)

| session | pnl | vol | trades | spread_captured | capture_rate | fill_rate | max_dd | sharpe | win% | adverse |
|---------|-----|-----|--------|----------------|-------------|-----------|--------|--------|------|---------|
| 0608 | 11021 | 15360 | 10204 | 0.64 | 64% | 13.8% | 53.0 | 3.54 | 59.7% | 0.929 |
| 0609 | 20804 | 29356 | 19177 | 0.64 | 64% | 13.0% | 53.0 | 4.93 | 60.1% | 0.895 |
| 0610 | 32026 | 44992 | 29873 | 0.63 | 63% | 13.5% | 53.0 | 4.84 | 59.9% | 0.897 |
| 0611 | 41051 | 58134 | 38437 | 0.63 | 63% | 13.0% | 53.8 | 4.80 | 59.9% | 0.905 |
| 0612 | 47117 | 67566 | 44671 | 0.62 | 62% | 12.1% | 53.8 | 4.43 | 59.8% | 0.918 |

日增 PnL: ~10000→9800→12000→9000→6000（递减趋势）

---

## 二、关键问题（按严重度排序）

### 🔴 P0-1: skew 方向完全错误（0/26162 正确 = 0.0%）

**数据**: ec2607 有持仓时（delta≠0）的 skew：
```
多头(delta>0): skew avg = +0.0396  ← 应该是负!
空头(delta<0): skew avg = -0.0392  ← 应该是正!
skew 方向正确(与delta反向): 0/26162 = 0.0%
```

**问题**: skew 本应让报价**远离**已有持仓方向（多头时压低 ask 鼓励对手卖出），实际 skew 在多头时是正的（+0.04），即**进一步偏向多头方向**，加剧 inventory 风险。

**影响**:
- 成交后持仓没有被 skew 引导平回 0
- delta 78-96% 时间是 flat(0)，说明成交后被别的方式（CLOSET 子单）平掉了，不是 skew 工作的
- adverse=0.93 高企部分原因：skew 把报价放在容易被方向性流吃掉的位置

**可能根因**: SpreadOptimizer 的 skew 计算符号反了，或 delta 传入单位不一致

### 🔴 P0-2: alpha 完全无效（alpha_acc=0%, alpha_pnl=0.00）

**数据**:
```
alpha 分布: 47% 看多, 46% 看空, 7% 中性 → 分布均匀，信号无方向性偏差
conf<0.3(低信心): 63.4%
conf>=0.3: 36.3%
```

**问题**: alpha 信号有值（avg=0.32），conf 有值（avg=0.26），但 alpha_acc=0% 且 alpha_pnl=0。说明：
- alpha 信号**产生了**（非零），但**预测准确率为 0**
- 或者 PerformanceAnalyzer 的 alpha 统计逻辑有 bug

**影响**: 策略报价中的 alpha skew 分量完全无效，等于纯随机

### 🔴 P0-3: toxicity 检测器完全不触发（tox_events=0）

**数据**:
```
toxic_score: avg=0.106, max=0.183
vpin:        avg=0.050, max=0.069
阈值:        adverseThreshold=0.7, vpinThreshold=0.7
```

**问题**: max score=0.183 << 阈值 0.7，max vpin=0.069 << 阈值 0.7。阈值设得太高，或信号源在 EC 上不适用。

**影响**:
- 毒性暂停(TOXICITY 状态)从不触发 → 在毒性流面前不保护
- adverse=0.93 的根源之一：没有毒性保护，所有成交都吃
- toxicity_spread_factor 调参(P2-4)完全无效的死参数

### 🟡 P1-1: fill_rate=13%（报价成交率极低）

**数据**: spread avg=3.12~3.66 tick，baseSpread=2.5，mult avg=1.17~1.33

**问题**: 实际 spread = 2.5 × mult ≈ 3-4 tick。EC 市场实际 bid-ask 可能只有 1-2 tick。报价在 book 深处，成交率仅 13%。

**影响**:
- 资金效率低，大部分报价不成交
- 但 PnL 仍正说明成交的那 13% 赚的 spread 够覆盖

### 🟡 P1-2: 持仓时间极短（delta 78-96% flat）

**数据**: ec2607 delta 78% flat, ec2608 82%, ec2609 96%

**问题**: 成交后持仓几乎立即被平掉（stra_buy/stra_sell 的 CLOSET 子单），做市商没有真正"持有库存"。这解释了：
- avg_inv=0（统计也是 0）
- spread_captured=0.64 tick（接近半 spread 1.25 的一半）

**影响**: 
- 做市商的库存周期极短 = 纯 spread scalping
- skew 没有机会发挥作用（持仓一闪而过）
- 没有真正承担库存风险 = 没有赚取库存风险溢价

### 🟡 P1-3: ec2609 远月几乎不成交

**数据**: ec2607 成交 25200 笔, ec2608 18268, ec2609 仅 1203

**问题**: ec2609 spread avg=3.66 tick（最宽），bilateral ratio 仅 8-26%（vs 主力 80-88%）。远月流动性差 + spread 太宽 = 几乎不成交。

---

## 三、PnL 可持续性分析

```
PnL 日增量趋势: 11021 → 9783 → 11222 → 9025 → 6067
                                         ↑ 开始下降
adverse 趋势:   0.929 → 0.895 → 0.897 → 0.905 → 0.918
                                                ↑ 恶化
fill_rate:      13.8% → 13.0% → 13.5% → 13.0% → 12.1%
                                                   ↓ 下降
```

**趋势**: PnL 日增量递减，adverse 上升，fill_rate 下降。策略在衰减。
可能原因：
- 5 天数据不够长，趋势不可靠
- 或做市的"免费 spread"被慢慢吃掉

---

## 四、优化方向

### 方向 A: 修 skew 方向（P0-1，最高优先级）

SpreadOptimizer 的 inventory skew 符号反了。修后：
- 多头时 skew 负（压低 ask 鼓励卖出平多）
- 空头时 skew 正（抬高 bid 鼓励买入平空）
- 预期：持仓周期延长，adverse 降低，inventory 风险降低

### 方向 B: 修 alpha 信号链（P0-2）

alpha_acc=0% 要么是信号本身无效，要么是统计 bug。需要：
- 验证 alpha 信号源（OFI/Momentum/LeadLag）在 EC 上是否有预测力
- 修 PerformanceAnalyzer 的 alpha 统计

### 方向 C: 调 toxicity 阈值（P0-3）

当前 0.7 阈值在 EC 上太高。需要：
- 分析 toxic_score 的分布，设动态阈值（如 95th percentile）
- 或降到 0.15-0.2（基于 max=0.183）

### 方向 D: 收窄 spread（P1-1）

baseSpread=2.5 太宽。回测不同 baseSpread（1.5/2.0/2.5）对比 fill_rate 和 PnL。

### 方向 E: P1-8 跨期同步报价

解决 ec2609 不成交 + 跨期报价不协调的问题（和 U2 per-contract 一起设计）。

---

## 五、优先级建议

1. **A（skew 方向）** — 符号反了是 bug，修了立竿见影
2. **C（toxicity 阈值）** — 简单调参，恢复毒性保护
3. **B（alpha 信号）** — 需要验证信号源有效性，工作量大
4. **D（spread 收窄）** — 回测调参，和 A 一起做
5. **E（跨期同步）** — 架构变更，Phase 4 milestone

---

## 六、备注

- PerformanceAnalyzer 不按 session 重置的统计 bug 仍存在（影响 PERF 解读）
- avg_inv=0 和 turnover=0 的统计可能也是 PerformanceAnalyzer 的 bug
- 5 天数据样本短，趋势判断需要更长时间窗口
