# Alpha + Toxicity 中长期改造方案 v3 — 最终版

> 2026-06-21 | 基于 v2 + 用户两点纠正
>
> v3 变更:
>   - 国内期货只有行情切片(tick snap), 无逐笔成交. Trade-through 基于切片推断
>   - Trade-through 作为 ISignalSource 实现, 对齐信号架构, 不单独搞 Detector
>   - 数据来源: MarketDataContext(tick快照), 与其他 5 个信号源一致

---

## 一、关键事实修正

### 1.1 国内期货数据现状

CTP 期货接口:
  - 推送: OnRtnDepthMarketData → 行情快照 (bid/ask/volume/totalvol)
  - **不推送**: 逐笔成交 (WTSTransData)
  - 代码中的 on_transaction 回调在实盘 CTP 不会被调用
  - 回测中 WTSTransData 是回测引擎模拟的, 不代表实盘

### 1.2 现有信号源的数据基础

所有 ISignalSource 实现都基于 MarketDataContext(tick 快照):
  - OFI: 盘口 bid/ask 量变化
  - TradeFlow: tick volume 增量 × 价格方向推断买卖 (非逐笔)
  - BookImbalance: 盘口深度
  - Momentum: mid 价格 EMA
  - LeadLag: 跨合约 mid 相关性
  - VPIN(在 ToxicFlowDetector 里): tick volume → bucket → 买卖不平衡

→ 所有"成交方向"都是从 tick 快照推断的, 非真实逐笔.
→ Trade-through 也必须基于 tick 快照, 不能假设有逐笔.

---

## 二、Trade-through 作为 ISignalSource

### 2.1 架构对齐

```
当前信号源架构:
  ISignalSource::update(MarketDataContext& book) → 统一数据接口
  SignalAggregator: _sources[SignalType::XXX] = make_unique<XxxSource>()
  computeAlpha(): Σ(weight_i × signal_i.getAlphaValue())
  
  已有 SignalType::TOXICITY (ISignalSource.h:48)
  已有 ToxicitySignalResult (ISignalSource.h:189-199)
  已有 SignalContext.toxicity (ISignalSource.h:234)
  但: 当前没有 ISignalSource 实现填充 toxicity 字段
      (toxicity 由 ToxicFlowDetector 独立计算, 不走 SignalAggregator)
```

### 2.2 Trade-through 信号源设计

Trade-through 在无逐笔数据下的等价定义:
  **从 tick 快照的 volume 增量 + 价格方向, 检测连续同方向成交脉冲**

```cpp
class TradeThroughSignalSource : public ISignalSource {
    // 基于 tick 快照的"伪逐笔"检测
    // 每次 tick:
    //   1. volume 增量 = tick.totalvolume - last_totalvolume
    //   2. 方向 = 价格 vs mid (或 vs 上一 tick)
    //   3. 如果 volume 增量 > large_trade_threshold → "大单"
    //   4. 连续 N 个 tick 同方向大单 → trade-through
    
    struct TickFlow {
        double volume;     // 本 tick 的 volume 增量
        int direction;     // +1 买, -1 卖, 0 中性
        uint64_t timestamp;
    };
    
    std::deque<TickFlow> _recent_flows;  // 滑动窗口
    uint32_t _window_ticks = 10;          // 10 个 tick 窗口 (~3-5秒)
    double _large_flow_threshold = 20.0;  // 大单阈值 (volume 增量, 手)
    int _consecutive_threshold = 3;       // 连续同方向大单
    
    // 输出: ToxicitySignalResult
    // toxicity_score = imbalance × min(1.0, consecutive / threshold)
    // toxic_detected = consecutive >= threshold && imbalance > 0.6
};
```

### 2.3 与 VPIN 的融合

```
SignalAggregator 改造:
  _sources[SignalType::TOXICITY] = make_unique<TradeThroughSignalSource>()

  extractSignalResults() 新增:
    auto tox_it = _sources.find(SignalType::TOXICITY);
    if (tox_it != _sources.end()) {
        _ctx.toxicity = static_cast<const ToxicitySignalResult&>(tox_it->second->result());
    }

ToxicFlowDetector 改造:
  VPIN 作为持续性分量保留 (bucket_size 1000→100, 去 alpha_toxicity)
  从 SignalAggregator 拿 TradeThroughSignalSource 的脉冲分量
  
  最终 toxic_score = max(VPIN_score, TT_score)
  → 与 v2 相同的 max 融合, 但 TT 走信号源架构而非独立 Detector
```

### 2.4 统一数据流

```
tick snapshot (CTP 唯一数据源)
  ↓
MarketDataContext (盘口 + volume 增量)
  ↓
SignalAggregator::update(book)
  ├── OFISignalSource::update(book)          → ofi
  ├── TradeFlowSignalSource::update(book)    → trade_flow  
  ├── BookImbalanceSignalSource::update(book)→ book_imbalance
  ├── MomentumSignalSource::update(book)     → momentum
  ├── LeadLagSignalSource::update(book)      → lead_lag
  ├── VolatilitySignalSource::update(book)   → volatility
  └── TradeThroughSignalSource::update(book) → toxicity  ← 新增
  
  computeAlpha(): Σ(IC_weight_i × signal_i)   ← IC 动态权重
  computeToxicity(): TT_score (脉冲)
  
ToxicFlowDetector (外部独立):
  VPIN (持续性, 小桶) + SignalContext.toxicity (脉冲, 来自 TT)
  final = max(VPIN, TT)
```

---

## 三、波动率自适应 Spread — 融入 spread_mult (v2 不变)

spread_mult 链新增 vol_mult 分量, 用 VolatilitySignalSource 已有的 vol_percentile:

```cpp
// SpreadOptimizer L35 区域, spread_mult *= vol_mult
double vp = ctx.volatility.vol_percentile / 100.0;  // [0,1]
if (vp > 0.7) {
    spread_mult *= 1.0 + (vp - 0.7) * _params.vol_spread_factor;  // factor~1.5
} else if (vp < 0.3) {
    spread_mult *= 0.9;
}
```

不新增类, 2 行代码, 融入现有 EMA 平滑链.

---

## 四、Alpha IC 动态权重 (v1 不变)

ICWeightTracker 基于各信号源的 getAlphaValue() vs 未来 N tick 回报,
动态调整 computeAlpha() 中的权重. IC 负的信号权重趋零.

---

## 五、修订实施计划

### 第一阶段: 离线 IC 验证 (1-2天)

| 步骤 | 内容 |
|------|------|
| 1.1 | SignalAggregator 加 [SIGNAL_DECOMP] 日志 (各信号源独立值) |
| 1.2 | 跑 EC 5 天 debug 回测 |
| 1.3 | Python IC/IR 分析各信号 → 有效/反向/无效判定 |

### 第二阶段: 代码改造 (3-4天)

| 步骤 | 内容 | 改动 |
|------|------|------|
| 2.1 | ICWeightTracker + SignalAggregator 集成 | 新增 1 类 + 改 SignalAggregator |
| 2.2 | TradeThroughSignalSource : ISignalSource | 新增 1 类, 基于 tick 快照 |
| 2.3 | SignalAggregator 注册 TT + extractToxicity | 改 SignalAggregator |
| 2.4 | ToxicFlowDetector VPIN 小桶 + max(TT) 融合 | 改 ToxicFlowDetector |
| 2.5 | spread_mult 加 vol_mult | 改 SpreadOptimizer, 2 行 |

### 第三阶段: 回测验证 (2天)

### 总改动量: 新增 2 类 + 改 3 模块

---

## 六、验证指标 (不变)

| 指标 | 改造前 | 目标 |
|------|--------|------|
| alpha N=1 acc | 12-24% (反向) | >50% |
| toxicity vs 波动率 corr | -0.12 (负) | >0 |
| fill_rate | 13% | 20-30% |
| adverse | 0.57-0.93 | <0.50 |

---

## 七、关键设计决策

1. **Trade-through 基于 tick 快照** — 不假设有逐笔, 用 volume 增量 + 价格方向推断
2. **TT 对齐 ISignalSource 架构** — 填充已有 ToxicitySignalResult, 走 SignalAggregator
3. **VPIN + TT = max 融合** — 持续性 + 脉冲性互补, 不互相稀释
4. **波动率自适应融入 spread_mult** — 不新增 Tracker, 2 行代码
5. **Alpha IC 动态权重** — 自适应市场状态, IC 负的自动降权
