# Alpha 权重自适应框架设计 — 交易逻辑驱动

> 2026-06-21
>
> 核心原则: 信号有内在交易逻辑, IC 低可能是参数不适配, 不代表信号绝对无效.
> 框架基于交易逻辑设计, IC 只是可信度评估的一个输入, 不是唯一判据.

---

## 一、设计哲学

### 1.1 不做什么

  ✗ 不因单一样本 IC 低就剔除信号 (过拟合)
  ✗ 不用固定权重硬编码 (不自适应)
  ✗ 不用纯统计指标驱动 (缺交易逻辑)

### 1.2 做什么

  ✓ 每个信号有"基础逻辑权重" (反映其交易逻辑的理论价值)
  ✓ 基础权重 + 动态调节因子 = 实际权重
  ✓ 动态调节基于多维度可信度评估, 不只看 IC
  ✓ 信号之间互相验证 (一致性检验)
  ✓ 调节有下界 (不归零) 和上界 (不独占)
  ✓ 市场状态感知 (不同状态下信号有效性不同)

---

## 二、三层权重模型

### 2.1 架构总览

```
最终权重 = 基础逻辑权重 × 市场状态调节 × 在线可信度调节

  基础逻辑权重 (静态, 交易逻辑设定):
    反映信号的理论预测价值和数据可靠性
    不随样本变化, 只随品种类型调整

  市场状态调节 (中频, 按波动率/流动性状态):
    不同市场状态下信号的预测力不同
    如: 高波动时 Momentum 更有效, 低波动时 MeanReversion 更有效

  在线可信度调节 (低频, 滚动 IC/一致性):
    IC 是可信度的输入之一, 但有下界保护
    信号一致性提供额外验证
```

### 2.2 第一层: 基础逻辑权重

每个信号根据其交易逻辑的理论价值设定基础权重:

```yaml
# 按品种类型配置, 而非全局硬编码
signal_weights:
  # 基础权重反映信号的理论可靠性
  ofi:          0.25    # 盘口流: 直接观测挂单意图, 理论可靠但盘口薄时噪声大
  trade_flow:   0.20    # 成交流: 方向性意愿, 但方向推断有误差
  book_imb:     0.20    # 盘口不平衡: 即时供需, 短期有效但易被撤单干扰
  momentum:     0.15    # 动量: 趋势延续, 在有趋势时有效
  lead_lag:     0.20    # 跨期: 主力领先, 在跨期品种(如EC)理论最有效
  
  # 下界: 权重不低于此值 (即使 IC 持续低, 也保留最低权重)
  weight_floor: 0.05
  # 上界: 权重不超过此值 (防止单一信号独占)
  weight_cap:   0.50
```

**EC 的特殊性**: EC 是跨期品种(ec2607/08/09), LeadLag 理论上最有预测力
(主力合约价格领先次主力)。当前 LeadLag 全零是数据连接问题，不是信号无效。
修复后 LeadLag 应获得更高权重。

### 2.3 第二层: 市场状态调节

不同市场状态下信号的预测力不同:

```cpp
struct MarketRegime {
    enum class VolRegime { LOW, NORMAL, HIGH, EXTREME };
    enum class TrendRegime { TRENDING, RANGING, MIXED };
    enum class LiquidityRegime { DEEP, NORMAL, THIN };
    
    VolRegime vol;
    TrendRegime trend;
    LiquidityRegime liquidity;
};

// 每个信号在不同 regime 下的调节因子 [0.5, 2.0]
// 基于交易逻辑设定, 非数据拟合
struct SignalRegimeFactors {
    // OFI: 盘口薄时噪声大 → 降权; 盘口深时可靠 → 升权
    double ofi_thin = 0.5;      // 流动性差: OFI 噪声大
    double ofi_deep = 1.5;      // 流动性好: OFI 可靠
    
    // TradeFlow: 高波动时方向推断更准(价格突破mid) → 升权
    double tradeflow_high_vol = 1.3;
    double tradeflow_low_vol = 0.7;
    
    // BookImbalance: 始终有即时供需信息 → 稳定
    double book_normal = 1.0;
    
    // Momentum: 趋势市有效, 震荡市无效
    double mom_trending = 1.5;
    double mom_ranging = 0.5;
    
    // LeadLag: 跨期品种始终有效
    double leadlag_cross_term = 1.5;
    double leadlag_single = 0.3;  // 非跨期品种降权
};
```

### 2.4 第三层: 在线可信度调节

基于滚动 IC + 信号一致性, 有下界保护:

```cpp
class ConfidenceTracker {
    // 滚动 IC (参考指标, 非唯一判据)
    double rolling_ic;        // 滚动相关系数
    double rolling_ir;        // IC 的信息比 (mean/std)
    
    // 信号一致性: 当前信号方向与其他信号的一致程度
    // 多个信号同向 → 互相验证 → 可信度高
    // 信号分歧大 → 互相矛盾 → 可信度低
    double consistency_score;
    
    // 综合可信度 [0, 1]
    // 不因 IC 低就归零, 只是降低调节因子
    double confidence() const {
        // IC 贡献: IC 正 → 增益, IC 负 → 衰减, 但有下界
        double ic_factor = 0.5 + 0.5 * std::tanh(rolling_ir * 2.0);  // [0, 1]
        
        // 一致性贡献: 多信号同向 → 增益
        double consistency_factor = 0.5 + 0.5 * consistency_score;  // [0, 1]
        
        // 综合: IC 和一致性各占一半
        return 0.5 * ic_factor + 0.5 * consistency_factor;  // [0, 1]
    }
    
    // 权重调节因子 (不归零, 不独占)
    // confidence=0.5 (中性) → factor=1.0 (不调节)
    // confidence=0.0 (低) → factor=0.3 (降权但不归零)
    // confidence=1.0 (高) → factor=2.0 (升权但不独占)
    double adjustmentFactor() const {
        double c = confidence();
        // 映射: [0,1] → [0.3, 2.0]
        return 0.3 + 1.7 * c;
    }
};
```

### 2.5 最终权重计算

```cpp
// SignalAggregator::computeAlpha() 改造:
void computeAlpha() {
    // 1. 确定当前市场状态
    MarketRegime regime = detectRegime();
    
    // 2. 对每个信号计算三层权重
    for (auto signal_type : all_signal_types) {
        // 第一层: 基础逻辑权重 (配置)
        double w_base = _cfg.getBaseWeight(signal_type);
        
        // 第二层: 市场状态调节 (查表)
        double regime_factor = getRegimeFactor(signal_type, regime);
        
        // 第三层: 在线可信度调节 (滚动)
        double conf_factor = _confidence_trackers[signal_type].adjustmentFactor();
        
        // 最终权重 = 基础 × 状态 × 可信度, 受 floor/cap 约束
        double w = w_base * regime_factor * conf_factor;
        w = clamp(w, _cfg.weight_floor, _cfg.weight_cap);
        
        _dynamic_weights[signal_type] = w;
    }
    
    // 3. 归一化
    double sum = Σ(_dynamic_weights.values());
    for (auto& [type, w] : _dynamic_weights) w /= sum;
    
    // 4. 加权合成 alpha
    double alpha = Σ(_dynamic_weights[type] × signal_value[type]);
}
```

---

## 三、市场状态检测

```cpp
MarketRegime detectRegime() {
    MarketRegime r;
    
    // 波动率状态 (基于 VolatilitySignalSource)
    auto vp = _ctx.volatility.vol_percentile;
    if (vp < 20) r.vol = VolRegime::LOW;
    else if (vp < 60) r.vol = VolRegime::NORMAL;
    else if (vp < 80) r.vol = VolRegime::HIGH;
    else r.vol = VolRegime::EXTREME;
    
    // 趋势状态 (基于短期 vs 长期 EMA)
    double short_ma = EMA(mid, 10);
    double long_ma = EMA(mid, 50);
    double ma_ratio = short_ma / long_ma;
    if (abs(ma_ratio - 1.0) > 0.002) r.trend = TrendRegime::TRENDING;
    else r.trend = TrendRegime::RANGING;
    
    // 流动性状态 (基于盘口深度)
    double avg_depth = (bid_depth + ask_depth) / 2;
    if (avg_depth > 50) r.liquidity = LiquidityRegime::DEEP;
    else if (avg_depth > 10) r.liquidity = LiquidityRegime::NORMAL;
    else r.liquidity = LiquidityRegime::THIN;
    
    return r;
}
```

---

## 四、一致性检验

### 4.1 逻辑

多个信号同向 → 互相验证 → alpha 可信度高
信号分歧大 → 互相矛盾 → alpha 可信度低 → 应缩小 spread 而非大偏移

### 4.2 计算

```cpp
double computeConsistency(const std::vector<double>& signals, 
                          const std::vector<double>& weights) {
    if (signals.size() < 2) return 0.5;  // 单信号无法验证
    
    // 加权平均方向
    double weighted_sum = Σ(w_i × sign(s_i));
    double weight_total = Σ(w_i);
    double net_direction = weighted_sum / weight_total;  // [-1, 1]
    
    // 一致性 = |net_direction|, 接近 1 表示所有信号同向
    return std::abs(net_direction);
}
```

### 4.3 应用

一致性高 → confidence 高 → 权重调节因子 > 1 → alpha 更强
一致性低 → confidence 低 → 权重调节因子 < 1 → alpha 更弱
→ 信号矛盾时 alpha 自动衰减，不是强行合成一个噪声值

---

## 五、IC 滚动计算

### 5.1 不是"IC低就剔除"，而是"IC作为可信度的一个输入"

```cpp
class ICRollingTracker {
    uint32_t _window = 2000;      // IC 计算窗口
    uint32_t _horizon = 5;        // 预测期限
    uint32_t _update_interval = 50; // 更新频率 (非每 tick)
    
    std::deque<double> _signal_values;
    std::deque<double> _future_returns;  // 延迟 horizon 后对齐
    
    void update(double signal_val, double future_return) {
        _signal_values.push_back(signal_val);
        _future_returns.push_back(future_return);
        if (_signal_values.size() > _window) {
            _signal_values.pop_front();
            _future_returns.pop_front();
        }
    }
    
    double getIC() const {
        return corr(_signal_values, _future_returns);
    }
    
    double getIR() const {
        // IR = mean(分段IC) / std(分段IC)
        // 将窗口分成 N 段, 每段算一个 IC, 取 mean/std
        // 这比单一 IC 更稳定
    }
};
```

### 5.2 IR 到调节因子的映射 (有下界)

```
IR (信息比)     调节因子      含义
──────────────────────────────────────────
IR > 1.0        2.0          强预测力, 升权
IR = 0.5        1.3          有预测力, 略升权
IR = 0.0        1.0          中性, 不调节
IR = -0.5       0.6          弱反向, 降权但不归零
IR < -1.0       0.3          强反向, 大幅降权但保留最低权重

公式: factor = 0.3 + 1.7 × (0.5 + 0.5 × tanh(IR × 2))
```

---

## 六、信号参数适配 (独立于权重框架)

### 6.1 问题

IC 低可能是信号参数不适配, 需要独立于权重框架解决:

| 信号 | 当前参数 | EC 可能问题 | 适配方向 |
|------|---------|------------|---------|
| OFI | window=50 | EC 盘口薄, 50 tick 窗口噪声大 | 试 20/100 |
| TradeFlow | window=100 | 方向推断(bid-ask bounce)在 EC 失效 | 改进方向推断算法 |
| BookImb | threshold=0.2 | 阈值可能不适配 EC 盘口结构 | 试 0.1/0.3 |
| Momentum | ema_alpha=0.1 | EMA 周期不适配 EC 波动频率 | 试 0.05/0.2 |
| LeadLag | lag_ms=50 | 信号全零=数据未连接 | 修复跨期数据馈入 |

### 6.2 参数适配 ≠ 权重调节

  - 参数适配: 让信号的**计算**更适合该品种 (调 window/threshold/method)
  - 权重调节: 让信号的**贡献**反映其当前可信度 (调权重)
  
  两者独立: 先调参数让信号"正确计算", 再用权重框架"合理加权"

### 6.3 TradeFlow 反向的根因 (需独立排查)

TradeFlow IC=-0.83 (强反向) 的可能原因:
  1. tick 方向推断: price >= ask → 买, price <= bid → 卖
     EC 的 bid-ask bounce (价格在 bid/ask 间跳动) 会导致方向频繁翻转
     → 累积的"净流"方向与真实方向相反
  2. 这是计算方法问题, 需要改进方向推断算法 (如 Lee-Ready rule 变体)
  3. 修复后 TradeFlow 的 IC 可能从 -0.83 变正
  4. 在修复前, 权重框架的 IC 调节会降低 TradeFlow 权重 (但不归零)

---

## 七、实施计划 (修订)

### 阶段 1: 信号参数适配 + LeadLag 修复 (1-2天)

优先修复明确的问题:
  - LeadLag 全零 → 排查跨期数据连接
  - TradeFlow 反向 → 排查方向推断逻辑
  - 各信号参数按 EC 品种适配 (window/threshold)

### 阶段 2: 三层权重框架实现 (3-4天)

  - 基础逻辑权重配置体系
  - 市场状态检测 + regime 调节表
  - IC 滚动计算 + 可信度调节 (有下界)
  - 一致性检验
  - SignalAggregator 集成

### 阶段 3: Toxicity + Spread 改造 (2-3天)

  - TradeThroughSignalSource (ISignalSource 实现)
  - VPIN bucket_size 调小
  - spread_mult vol 分量

### 阶段 4: 回测验证 (2天)

---

## 八、关键设计决策

1. **权重有下界 (floor=0.05)** — 信号不会因 IC 低而完全归零
2. **权重有上界 (cap=0.50)** — 防止单一信号独占
3. **三层独立调节** — 基础/状态/可信度各管各的, 不互相覆盖
4. **一致性检验** — 信号矛盾时 alpha 自动衰减
5. **参数适配独立于权重** — 先修信号计算, 再调权重
6. **市场状态感知** — 不同 regime 下信号有效性不同
7. **IC 是参考非判据** — 避免过拟合单一样本
