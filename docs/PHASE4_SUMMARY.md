# Phase 4 交付物总结

> 2026-06-18 ~ 2026-06-21 | Phase 4: 信号系统改造 + 框架 bug 修复 + 评估指标修正
>
> 12 个 commit, 122 文件变更, 15405 行新增 / 9437 行删除

---

## 一、UftMocker 框架级 bug 修复 (3 个)

### BUG-M1: on_trade 平今/平昨穿仓
- 位置: UftMocker.cpp on_trade offset==1/2
- 问题: `_newvol -= qty` 无 max(0) 约束, 多笔 CLOSET 叠加时持仓穿成负数
- 影响: POSITION_BREACH 231,751 次
- 修复: qty 截断到实际持仓量 + overflow 日志

### BUG-M3: procOrder 值拷贝 _left 不写回
- 位置: UftMocker.cpp procOrder L927
- 问题: `OrderInfo ordInfo = it->second` 值拷贝, `_left -= qty` 只改副本
- 影响: 同一订单被重复撮合 N 次, overflow 110,330 次
- 修复: 改引用 `OrderInfo& ordInfo`

### BUG-M2 误诊: _avail 双重扣减
- 我引入的错误修复: on_trade 扣 _avail (原框架设计是下单冻结/撤单解冻)
- 影响: _newavail 虚低 → stra_buy 跳过 exit_short → 多空双向累积 → BREACH 296,224
- 修复: 撤销错误修复, 恢复原设计

**验证**: POSITION_BREACH 231,751 → 0, overflow 110,330 → 0, 穿仓 → 0

---

## 二、信号源修复 (4 个)

### LeadLag 缩放修复
- 位置: LeadLagSignalSource.h calculateSignal
- 问题: `mid_change × 100` 对 EC(mid~3700) 太小, 信号几乎全零
- 修复: scale_factor 100→3000 (bps 缩放, 可配置)
- 效果: IC 0.000 → +0.04~+0.09 (ec2609 最高)

### TradeFlow 滑窗衰减修复
- 位置: MarketDataContext.cpp TradeFlowTracker::onTickInference
- 问题: `_net_trade_flow += signed_flow` 只增不减, 整个 session 单调累积
- 修复: 加 5秒/100tick 滑窗 (deque 衰减)
- 效果: IC -0.83 → -0.04 (从强反向到接近中性)
- 效果: 买卖分布从 0.4%买/32.5%卖 → 40.8%买/44.1%卖

### TradeFlow 统计显著性归一化
- 位置: MarketDataContext.cpp getAnalysis + TradeFlowSignalSource.h
- 问题: buy_pressure/sell_pressure 比值在方向一致时饱和 ±1 (77.2%)
- 修复: 改用 t-statistic: net_flow / (avg_size × sqrt(n))
- 效果: 饱和率 77.2% → 4.4%

### OFI 去饱和
- 位置: OFISignalSource.h updateNormalization
- 问题: 单 tick std_dev 与 cumulative_ofi 量级不匹配 → tanh 饱和
- 修复: 改用 cumulative 幅度归一化 (avg_abs × n × 0.5)
- 效果: p75 从 1.0(饱和) → 0.84(有梯度)

---

## 三、信号幅度归一化

### RollingScaleTracker
- 位置: ICWeightTracker.h (新增类)
- 功能: 滚动 p95(|signal|) 归一化, 统一各信号幅度到可比范围
- 问题: OFI/Trade 幅度 ~1.0, Mom ~0.019, LL ~0.034 → 53x 差异
- 效果:
  - Mom 贡献从 0.5% → 14.7% (权重真正有效)
  - Book 贡献从 12.3% → 19.1%
  - LL 不归一化 (重复值多, 归一化会爆炸)

---

## 四、三层权重框架

### ICWeightTracker.h + AdaptiveWeightFramework (新增)

**设计原则**: IC 低 ≠ 信号无效, 可能是参数不适配. 权重有 floor(0.05)/cap(0.50).

**三层模型**:
1. 基础逻辑权重 (静态, 按交易逻辑): OFI=0.25, Trade=0.20, Book=0.20, Mom=0.15, LL=0.20
2. 市场状态调节 (regime 驱动):
   - OFI: 薄流动性 ×0.5, 深 ×1.5
   - TradeFlow: 高波动 ×1.3, 低 ×0.7
   - Book: 深 ×1.3, 薄 ×0.7
   - Momentum: 趋势 ×1.5, 震荡 ×0.5
   - LeadLag: 跨期 ×1.5
3. 在线可信度调节 (滚动 IC + 一致性): IR→factor 映射有下界 0.3

**集成**: SignalAggregator::computeAlpha 用动态权重替代固定权重

---

## 五、评估指标修正

### 真实 adverse selection 追踪
- 位置: PerformanceAnalyzer.h/.cpp
- 问题: 旧 adverse = is_crossing成本 + 负spread, 被 PnL 分母放大
- 修复: 成交后 10 tick 价格逆向移动 / 成交量 (独立指标)
- PERF 日志增加 real_adv/vol 输出

### 关键发现: 旧 adverse 和真实 adverse 完全反向
- sens0.5: 旧adv=0.55(看似好) 真实adv=3.76(最高)
- sens2.0: 旧adv=0.96(看似差) 真实adv=2.75(最低)
- 但 sens0.5 的 spread_captured=0.99 补偿了高 adverse

### 正确的评估指标组合
1. PnL/vol (做市效率)
2. sharpe (风险调整)
3. spread_captured (收入质量)
4. real_adv/vol (真实逆向选择, 辅助参考)
5. win_rate, fill_rate

---

## 六、最优配置 (基于正确指标重新评估)

### 6 组配置 sweep 结果

```
排名  配置        PnL/vol  sharpe  spread  win%   fill%
──────────────────────────────────────────────────────────
1     sens0.5    1.099    5.40    0.99    65.3%  29.3%  ★
2     sens1.0    0.875    4.80    0.74    60.5%  11.8%
3     w_balanced 0.782    4.72    0.69    60.8%  10.8%
4     sens2.0    0.709    4.54    0.61    59.7%  11.8%
```

### 最终最优配置

```yaml
# coordinator.yaml
pipeline:
  alphaSensitivity: 0.5  # 让 spread 接近对称, 捕获满 spread
modules:
  toxicityDetector:
    adverseThreshold: 0.16
    vpinThreshold: 0.09
```

权重: OFI=0.25, Trade=0.20, Book=0.20, Mom=0.15, LL=0.20 (平衡)

---

## 七、Phase 4 前后对比

```
指标              Phase 2 结束        Phase 4 最终
──────────────────────────────────────────────────────
POSITION_BREACH   231,751            0
overflow          110,330            0
穿仓              多次               0
LeadLag IC        0.000              +0.09 (ec2609)
TradeFlow IC      -0.83              -0.04
OFI 饱和          100%               84% (有梯度)
Mom 贡献          0.5%               14.7%
PnL/vol           N/A                1.099 (sens0.5)
sharpe            N/A                5.40
spread_captured   N/A                0.99
alpha_acc         0%                 37-45%
```

---

## 八、新增文件

| 文件 | 说明 |
|------|------|
| ICWeightTracker.h | 三层权重框架 + RollingScaleTracker + RollingIC |
| docs/PHASE4_PLAN.md | Phase 4 计划 |
| docs/EC_BACKTEST_ANALYSIS.md | EC 回测分析 |
| docs/STRATEGY_SUBSYSTEM_VERIFICATION.md | 信号子系统验证 |
| docs/ALPHA_TOXICITY_DEEP_ANALYSIS.md | Alpha/Toxicity 深度分析 |
| docs/ALPHA_TOXICITY_REDESIGN_PLAN.md | 改造方案 v3 |
| docs/ALPHA_WEIGHT_FRAMEWORK_DESIGN.md | 权重框架设计 |
| docs/PHASE1_SIGNAL_DIAGNOSIS.md | 信号诊断 |
| docs/POSITION_BREACH_FIX.md | POSITION_BREACH 修复 |
| docs/POSITION_BREACH_DIAGNOSIS.md | BREACH 诊断 (含误诊教训) |

---

## 九、方法论教训

1. **日志现象必须源码确认** — 误读 skew delta 字段 → 假"反向"报警
2. **审计穷尽 API 全集** — 只 grep 一个 API 会漏等价 API
3. **IC 低 ≠ 信号无效** — LeadLag 全零是缩放问题, TradeFlow 反向是滑窗缺失
4. **信号幅度必须归一化** — 否则权重框架形同虚设
5. **评估指标必须独立** — adverse/PnL 被 PnL 分母放大, 完全误导
6. **不能因修复引入新 bug** — BUG-M2 误诊浪费了 3 轮迭代
