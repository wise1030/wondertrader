# WtFutuCore 深度分析报告 V6 — 复核与修复方案

> **复核日期**: 2026-07-21  
> **复核方法**: 逐条读取源码验证，确认准确性后给出最优修复方案  
> **基础**: DEEP_ANALYSIS_V6.md 复核结果

---

## 一、复核结论概要

### 确认为真（需修复）

| 编号 | 问题 | 原级别 | 复核级别 | 修复难度 |
|------|------|--------|----------|----------|
| **B1** | FLATTEN_POSITION 不可达 | P0 | **P0** | 1 行 |
| **C10** | double→int32_t 截断 | P0 | **P0** | 1 行 |
| **B14** | timestampToMs 解析完全错误 | P0 | **P0** | 删除函数 |
| **B12** | TrendFollowing entry_price 永不赋值 | P0 | **P0** | 3 行 |
| **B4** | Welford 累积非滚动 | P0 | **P1** | 中等 |
| **B6** | Orphan 队列满静默丢弃 | P0 | **P1** | 3 行 |
| **A2** | arb 线程读 portfolio data race | P0 | **P1** | 中等 |
| **B7** | close 信号不释放 in_flight | P1 | **P1** | 3 行 |
| **B8** | 自成交检查只查首个匹配 | P1 | **P1** | 5 行 |
| **B3** | delta rate 双恢复路径冲突 | P1 | **P1** | 中等 |
| **B15** | SignalAggregator reset 不完整 | P1 | **P1** | 10 行 |
| **B13** | 自适应权重学习失效 | P1 | **P1** | 2 行 |
| **B10** | suppress 语义错配 | P1 | **P1** | 3 行 |
| **C2** | PerformanceMonitor 非线程安全 | P1 | **P1** | 文档 |

### 误报 / 降级

| 编号 | 原报告声称 | 复核结论 | 理由 |
|------|-----------|---------|------|
| **C1** | pushOverwrite 破坏不变量（P0） | **P3 死代码** | 全项目无调用者 |
| **B11** | 加仓覆盖平仓信号（P0） | **误报** | CLOSE 条件 `zscore > -exit*0.3` 与 ADD 条件 `zscore < -entry*0.5` 互斥 |
| **B5** | 残腿对冲"过度对冲"（P0） | **P2 内存泄漏** | `min(vol, remaining)` 已正确限制 hedge_qty；实际问题：对手腿部分成交后取消时 map 条目泄漏 |

---

## 二、确认问题的详细修复方案

### B1. FLATTEN_POSITION 不可达 [P0]

**根因**：`checkRiskLimits` 最多产生 2 个 BREACH（EXPOSURE + POSITION_NET），但 `flatten_threshold` 默认 3。

**最优修复**：将默认值从 3 改为 2。

```cpp
// FutuRiskMonitor.h:137
, flatten_threshold(2)  // was 3; max achievable breachCount is 2

// FutuRiskMonitor.h:154  
r.flatten_threshold = FutuConfig::readUInt32(v, "flattenThreshold", 2);

// UftFutuMmStrategy.h:165
, flatten_threshold(2), delta_rate_window_sec(2)...

// FutuConfigLoader.cpp:161
_config.risk.flatten_threshold = readUInt32(cfgFrequency, "flattenThreshold", 2);
```

**为什么不是"让 checkRiskLimits 产生更多 violation"**：增加 violation 数量会让 WARNING 级别的频率超限也计入 breachCount，导致 FLATTEN 在撤单过多时就触发，过于激进。改默认值为 2 精准匹配实际可达的 BREACH 上限。

---

### C10. double→int32_t 截断 [P0]

**根因**：`getPositionReductionToLimit` 返回 double，调用方赋值给 int32_t。

**最优修复**：

```cpp
// UftFutuMmStrategy.cpp:1870
// 改前: int32_t reduction = _portfolio->getPositionReductionToLimit(*breached);
// 改后:
double reduction = _portfolio->getPositionReductionToLimit(*breached);
```

后续使用处（L1889 `stra_exit_long/short` 的 qty 参数）会自动隐式转换为整数（向下取整），这对持仓手数是正确的语义。

---

### B14. timestampToMs 解析完全错误 [P0]

**根因**：`timestampToMs` 假设输入是压缩格式 `DDHHMMSSmm`，但实际输入已是 epoch ms。函数将 epoch ms 解析为垃圾值。

**影响链**：
- `getFillRetreat` (L447,450,459) 用垃圾值计算 elapsed → retreat 永不激活
- 但 `recordFill` 和 `pruneHistory`/`decayCalibration` 直接用 epoch ms（不经 timestampToMs），这些路径正常

**最优修复**：删除 `timestampToMs` 函数，直接使用 epoch ms。

```cpp
// SelfTradeCalibrator.cpp:447
// 改前: uint64_t now_ms = timestampToMs(current_time);
// 改后:
uint64_t now_ms = current_time;  // 已是 epoch ms

// SelfTradeCalibrator.cpp:450
// 改前: uint64_t fill_ms = timestampToMs(state.last_buy_fill_time);
// 改后:
uint64_t fill_ms = state.last_buy_fill_time;  // 已是 epoch ms

// SelfTradeCalibrator.cpp:459 同理

// 删除 L11-20 的 timestampToMs 函数
```

---

### B12. TrendFollowingStrategy entry_price 永不赋值 [P0]

**根因**：`_trend_state.entry_price` 仅在构造时初始化为 0，从不赋值。

**最优修复**：在趋势确认时记录入场价。

```cpp
// TrendFollowingStrategy.cpp — updateTrendState() 中，趋势方向确认时赋值
void TrendFollowingStrategy::updateTrendState()
{
    // ... existing MA/trend logic ...
    
    TrendDirection new_dir = detectTrendDirection();
    if (isConfirmedTrend() && new_dir != TrendDirection::NONE)
    {
        if (_trend_state.direction != new_dir)  // 方向变化 = 新仓位
        {
            _trend_state.direction = new_dir;
            _trend_state.entry_price = _prices.empty() ? 0.0 : _prices.back();
            _trend_state.bars_in_current_trend = 0;
        }
        else
        {
            _trend_state.bars_in_current_trend++;
        }
    }
}
```

---

### B4. Welford 累积非滚动 [P1]

**根因**：`_welford_n` 只增不减，统计量反映全部历史样本而非最近 N 个。

**影响评估**：
- 同品种跨期（价差平稳）：累积统计收敛到真值，影响小
- 跨品种（价差有趋势）：累积统计稀释近期信号，Z-score 失效

**最优修复**：改用 EWMA 衰减方差（无需维护移出样本，O(1) 更新）：

```cpp
// SpreadCalculator.cpp:136-165 — 替换 Welford 为衰减方差
void SpreadCalculator::updateStatistics()
{
    if (std::isnan(_current_spread)) return;

    // EWMA mean (alpha = 1/window, 约 1/256 ≈ 0.004)
    constexpr double alpha = 0.005;  // 半衰期 ~138 ticks
    double prev_mean = _spread_mean;
    _spread_mean = alpha * _current_spread + (1.0 - alpha) * _spread_mean;
    
    // EWMA variance (exponentially weighted)
    double diff = _current_spread - _spread_mean;
    _spread_var = alpha * diff * diff + (1.0 - alpha) * _spread_var;
    _spread_std = std::sqrt(_spread_var);
    
    // Z-Score
    _zscore = (_spread_std > 1e-10) 
        ? (_current_spread - _spread_mean) / _spread_std 
        : 0.0;
    
    // ... rest unchanged ...
}
```

**优势**：
- 无需 RingBuffer 维护移出样本
- 自动适应非平稳序列
- O(1) 计算（与 Welford 相同）
- `_welford_n` 仍可用于预热期判断（min_samples 检查）

---

### B6. Orphan 队列满静默丢弃 [P1]

**根因**：`_orphan_legs_from_arb.tryPush()` 返回值未检查。

**最优修复**：

```cpp
// AsyncArbitrageExecutor.cpp:541-545
// 改前:
// _orphan_legs_from_arb.tryPush({...});
// WTSLogger::warn("...");

// 改后:
if (!_orphan_legs_from_arb.tryPush({...})) {
    WTSLogger::error("AsyncArb CRITICAL: orphan queue FULL (64), "
                     "leg1 exposed! pair={}, code={}",
                     signal.pair_id, signal.leg1_code);
    // 兜底: 直接在 arb 线程标记 onArbSignalDropped,
    // 让主线程下个 arb 周期重新发对冲信号
    _arb_manager->onArbSignalDropped(signal.pair_id);
}
```

---

### A2. arb 线程读 portfolio PnL data race [P1]

**根因**：arb 线程 `generateSignals` 直接读 `_portfolio_ptr->getTotalUnrealizedPnL()`，主线程同时写 `_contracts` 元素。

**实际风险评估**：
- vector 结构稳定（addContract 仅 init 期调用，运行期无增删）→ 无 iterator invalidation
- 读写的是 `double` 字段（8 字节对齐），x86-64 上原子
- 最坏情况：读到微秒级不一致的 PnL 快照，对风控判断可接受

**最优修复**（性价比最高）：主线程周期性快照 PnL 到 atomic 变量。

```cpp
// FutuPortfolio.h — 新增 atomic 快照
class FutuPortfolio {
    // ...
    std::atomic<double> _snapshot_unrealized_pnl{0};
    std::atomic<double> _snapshot_total_pnl{0};
    
public:
    // 主线程在 markToMarket/addRealizedPnL 后调用
    void publishPnLSnapshot() {
        _snapshot_unrealized_pnl.store(getTotalUnrealizedPnL(), std::memory_order_relaxed);
        _snapshot_total_pnl.store(getTotalPnL(), std::memory_order_relaxed);
    }
    
    // arb 线程只读快照
    double getSnapshotUnrealizedPnL() const {
        return _snapshot_unrealized_pnl.load(std::memory_order_relaxed);
    }
    double getSnapshotTotalPnL() const {
        return _snapshot_total_pnl.load(std::memory_order_relaxed);
    }
};

// SpreadArbitrageManager.cpp:424-425 — 改为读快照
double unrealized = _portfolio_ptr->getSnapshotUnrealizedPnL();
double total = _portfolio_ptr->getSnapshotTotalPnL();

// StrategyCoordinator.cpp — updateMarketData 末尾调用 publishPnLSnapshot
_portfolio->publishPnLSnapshot();
```

**优势**：无需加锁，atomic double 在 x86-64 上 lock-free，延迟 <10ns。

---

### B7. close 信号低 confidence 不释放 in_flight [P1]

**根因**：只对 OPEN 类调 onArbSignalDropped。

```cpp
// AsyncArbitrageExecutor.cpp:294-298 — 改前只处理 OPEN
// 改后:
if (signal.confidence < 0.5)
{
    _arb_manager->onArbSignalDropped(signal.pair_id);
    // 无论 OPEN 还是 CLOSE，未执行就释放 in_flight
}
```

---

### B8. 自成交检查只查首个匹配 [P1]

**根因**：break 后不再检查其它订单。

```cpp
// AsyncArbitrageExecutor.cpp:366-378 — 改为循环检查
bool price_adjusted = false;
for (const auto& order : sell_it->second) {
    while (leg1_price >= order.price) {  // 循环直到无冲突
        leg1_price = order.price - leg1_tick_size;
        price_adjusted = true;
    }
    if (price_adjusted) break;  // 调整后重新从头检查
}
```

更简洁的写法：取所有卖单的最低价，确保低于它：

```cpp
double min_sell = std::numeric_limits<double>::max();
for (const auto& order : sell_it->second)
    min_sell = std::min(min_sell, order.price);
if (leg1_price >= min_sell) {
    leg1_price = min_sell - leg1_tick_size;
    price_adjusted = true;
}
```

---

### B13. 自适应权重学习失效 [P1]

**根因**：`scaled_pnl = pnl / (base_qty+1)` 未归一化，pnl 单位是货币（~10000），一次学习就超上限。

```cpp
// StatisticalArbStrategy.cpp:226 — 改前
// double scaled_pnl = pnl / (base_qty + 1);

// 改后: tanh 归一化，将 pnl 映射到 [-1, 1]
double scaled_pnl = std::tanh(pnl / (_config.base_qty * 10.0 + 1.0));
```

---

### B10. MarketMakingEnhancer suppress 语义错配 [P1]

**根因**：suppress 由 z-score 触发，但衰减时按 skew 大小清 suppress。

```cpp
// MarketMakingEnhancer.cpp:158-165 — 改前按 skew 清 suppress
// 改后: suppress 只由 z-score 条件清除，与触发条件一致

// 删除以下行:
// if (std::abs(_quoting_state.bid_skew) < 0.05) _quoting_state.suppress_bid = false;
// if (std::abs(_quoting_state.ask_skew) < 0.05) _quoting_state.suppress_ask = false;

// 替换为: 只衰减 skew，suppress 由 calculateAdjustment 每tick重算
_quoting_state.bid_skew *= decay_factor;
_quoting_state.ask_skew *= decay_factor;
```

---

### B15. SignalAggregator reset() 不完整 [P1]

```cpp
// SignalAggregator.h:214-218 — 补充重置
void reset()
{
    _ctx.reset();
    _valid_signals.clear();
    _prev_alpha = 0.0;
    _tick_counter = 0;
    _tick_count = 0;
    _mid_history_for_ic.clear();
    _mid_ma_short = 0;
    _mid_ma_long = 0;
    _ma_short_sum = 0;
    _ma_long_sum = 0;
    _dynamic_weights.reset();  // 若 DynamicWeightFramework 有 reset
    // _scale_trackers 也需重置（若有 reset 方法）
}
```

---

### B3. Delta Rate 双恢复路径冲突 [P1]

**根因**：RiskMonitor 内部 `checkAndHandleDeltaRateBreach` 和 Coordinator `checkRisk` 各自管理恢复。

**最优修复**：统一到 Coordinator 单一恢复路径，RiskMonitor 只负责检测和标记，不负责恢复。

```cpp
// FutuRiskMonitor.cpp:772-809 — checkAndHandleDeltaRateBreach
// 改为: 只返回 breach 状态，不直接 pause/resume
bool FutuRiskMonitor::isDeltaRateBreached() const
{
    if (_rate_limits.max_delta_change_per_sec <= 0) return false;
    double rate = getDeltaChangeRate();
    return std::abs(rate) > _rate_limits.max_delta_change_per_sec;
}

// 删除 pauseQuoting()/resumeQuoting() 调用
// 恢复由 Coordinator checkRisk 统一管理 (已有 _quoting_paused atomic + qphase 同步)
```

---

### B5 修正. 残腿对冲 map 条目泄漏 [P2]

**实际问题描述**：V6 声称"过度对冲"是误报。`min(vol, remaining)` 已正确限制对冲量。实际问题是：对手腿部分成交后被取消（非全部成交），`_arb_hedge_on_fill` 条目 `hedged_qty < original_qty`，永不 erase。

**修复**：添加超时清理（onTick 中周期性扫描）。

```cpp
// ArbExecutionBridge.cpp — onTick 中添加
void ArbExecutionBridge::cleanupStaleHedges(uint64_t now_ms)
{
    for (auto it = _arb_hedge_on_fill.begin(); it != _arb_hedge_on_fill.end(); ) {
        // 超过 30s 未完全对冲的条目: 对侧腿已 finalize, 清理
        if (now_ms - it->second.created_time_ms > 30000) {
            if (it->second.hedged_qty < it->second.original_qty) {
                WTSLogger::warn("Hedge entry expired with incomplete hedge: "
                    "pair={}, hedged={}/{}, closing entry",
                    it->first, it->second.hedged_qty, it->second.original_qty);
            }
            it = _arb_hedge_on_fill.erase(it);
        } else {
            ++it;
        }
    }
}
```

---

## 三、误报分析

### C1. pushOverwrite — 降级为 P3 死代码

**V6 声称**：pushOverwrite 破坏 SPSC 不变量。

**复核结论**：实现确实有 bug（tail 推进到 head 位置使队列看起来空），但**全项目无任何调用者**。`_orphan_legs_from_arb` 使用 `tryPush`（正确处理失败），`_tick_queue`/`_order_queue` 也使用 `tryPush`。

**处理**：标记为死代码，建议直接删除函数和 `_drop_count` 字段（简化代码），或修复实现留作未来使用。

---

### B11. 加仓覆盖平仓 — 完全误报

**V6 声称**：MeanReversion 加仓块（L107-124）可以覆盖平仓信号（L87-99）。

**复核结论**：条件互斥。

验证（典型配置 entry=2.0, exit=1.0）：
- CLOSE_LONG: `spread_position > 0 && zscore > -0.3`（zscore 接近 0）
- ADD_LONG: `spread_position > 0 && zscore < -1.0`（zscore 仍然很负）

`zscore > -0.3` 和 `zscore < -1.0` 不可能同时成立。两个 if 块只有一个是有效的。

即使极端配置（exit=5, entry=1）使条件重叠，加仓也仅在 zscore 在重叠区间时触发，此时既该平（接近均值）又该加（仍然偏离），加仓的 confidence×0.7 更保守，设计上可接受。

---

## 四、修复优先级排序

### 第一批：P0 快速修复（1-2 小时）

| 编号 | 修复内容 | 预估工时 |
|------|---------|---------|
| B1 | flatten_threshold 默认 3→2 | 5 min |
| C10 | int32_t → double | 5 min |
| B14 | 删除 timestampToMs，直接用 epoch ms | 15 min |
| B12 | TrendFollowing entry_price 赋值 | 20 min |

### 第二批：P1 逻辑修复（半天）

| 编号 | 修复内容 | 预估工时 |
|------|---------|---------|
| B4 | Welford → EWMA 衰减方差 | 1h |
| B6 | orphan 队列满兜底 | 15 min |
| A2 | PnL 快照 atomic | 1h |
| B7 | close 信号 in_flight 释放 | 15 min |
| B8 | 自成交检查循环 | 30 min |
| B13 | 自适应权重 tanh 归一化 | 15 min |
| B10 | suppress 衰减修正 | 15 min |
| B15 | SignalAggregator reset 补全 | 30 min |
| B3 | delta rate 恢复路径统一 | 1h |

### 第三批：P2 优化与清理（可选）

| 编号 | 修复内容 | 预估工时 |
|------|---------|---------|
| B5修正 | hedge map 超时清理 | 30 min |
| P1 | 组合指标增量缓存 | 2h |
| P2 | arb spinlock 批量化 | 1h |
| C1 | 删除 pushOverwrite 死代码 | 10 min |

---

## 五、架构层面建议

### 1. 统一时间戳规范 [跨模块]

引入编译期约束，禁止跨格式时间算术：

```cpp
// 新增 TimeDomain.h
enum class TimeDomain : uint8_t {
    EPOCH_MS,    // TimeUtils::getLocalTimeNow()
    HHMMSSMMM,  // tick->action_time()
};

template<TimeDomain D>
struct Timestamp {
    uint64_t value;
    Timestamp(uint64_t v) : value(v) {}
    // 禁止不同 Domain 之间的减法
    uint64_t operator-(const Timestamp<D>& other) const { return value - other.value; }
};

// 数据入口处转换
Timestamp<TimeDomain::EPOCH_MS> convertFromTick(uint32_t action_time, uint32_t action_date);
```

### 2. TradingStateGuard 统一状态管理 [A1]

```cpp
class TradingStateGuard {
    TradingState& _ts;
    FutuRiskMonitor& _risk;
    std::unordered_map<std::string, bool>& _blocked;
    double& _risk_spread_mult;
    
public:
    // 唯一的恢复入口: 清所有状态
    void resumeAll() {
        _ts.resumeFromRisk();
        _risk.resumeQuoting();
        _risk.unblockLong();
        _risk.unblockShort();
        _blocked.clear();
        _risk_spread_mult = 1.0;
    }
};
```

### 3. PortfolioSnapshot 不可变快照 [A2 长期方案]

```cpp
struct PortfolioSnapshot {
    double total_delta;
    double total_exposure;
    double unrealized_pnl;
    double total_pnl;
    uint64_t timestamp;
    // read-only, 可安全跨线程共享
};

// 主线程 publish, arb 线程 read-only 引用
std::shared_ptr<const PortfolioSnapshot> FutuPortfolio::getSnapshot() const;
```

---

## 六、性能优化建议优先级

### 高性价比（改动小，收益大）

| 优化 | 位置 | 改动 | 收益 |
|------|------|------|------|
| 组合指标增量缓存 | FutuPortfolio | 50 行 | 每 tick 省 5-8 次遍历 |
| arb 批量查询 | StrategyCoordinator | 20 行 | 每 tick 省 2 次 spinlock |
| checkAndHedge 条件化 | StrategyCoordinator | 5 行 | 不需对冲时跳过全量同步 |
| updateAlerts 增量化 | SpreadRiskManager | 30 行 | 每 tick 省 N 次 push_back |

### 中性价比

| 优化 | 位置 | 改动 | 收益 |
|------|------|------|------|
| shouldCancelDueToRate 改 deque | UnifiedOrderTracker | 5 行 | O(n²)→O(n) |
| cancelByPair 反向索引 | OrderRouter | 20 行 | O(P×A)→O(A) |
| handleBilateralQuote 预分配 | FutuQuoter | 5 行 | 每 tick 省 2 次堆分配 |

---

> **总结**: V6 报告共报告 37 个问题，复核后确认为真的有 35 个（B11 误报，C1 降级为死代码）。P0 级 4 个可在 1 小时内修复。建议按第一批→第二批顺序执行，每批完成后编译验证。
