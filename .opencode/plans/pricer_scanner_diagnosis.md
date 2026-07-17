# WtOptionCore 深度诊断 - Pricer调度 / Tick回调 / Scanner执行

## 一、Pricer 调度对比

### 1.1 原始项目 (quantbox)

**触发机制**: 事件驱动，仅在前月合约 book 变化时触发

```
前月合约 IBook 变化
    │
    ├─ 模式A: compute_values_on_book=true
    │   -> IBookListener::onBookChanged -> publishPricingChanged()
    │
    └─ 模式B: compute_values_on_book=false (默认)
        -> TriggerListDriver 监听 MSG_TAIFEX_BOOK 消息
        -> 20ms 超时合并
        -> onFire() 触发 computeValues
        -> 同时调度 onComputeValuesTimeout 在 min_compute_values_interval 后重试
```

**关键特征**:
- **仅前月合约变化时触发** - 不是每个 tick 都计算
- **20ms 消息合并** - TriggerListDriver 合并多个消息后一次性触发
- **超时重试** - onComputeValuesTimeout 自调度，确保不遗漏
- **pricingChangedEvent** - 通过事件发布触发 pricer 的 computeValues

### 1.2 迁移项目 (WtOptionCore)

**触发机制**: 每个 tick batch 都调用 computeValues，通过时间防抖

```
每个 tick batch
    │
    ├─ on_tick_batch: _pricer->setTime()
    ├─ on_batch_complete:
    │   ├─ 检查 (now - _lastComputeTime) >= _minComputeInterval (20ms)
    │   │   ├─ YES: _grid->computeValues(_pricer)
    │   │   └─ NO: 跳过 (但 drainPendingQuotes 仍执行)
    │   └─ drainPendingQuotes
```

**关键差异**:

| 方面 | 原始 | 迁移 |
|------|------|------|
| 触发源 | 仅前月 book 变化 | 任何 tick batch |
| 合并机制 | TriggerListDriver 20ms 消息合并 | 时间防抖 20ms |
| 超时重试 | onComputeValuesTimeout 自调度 | ❌ 无 |
| FAST/SLOW | 原始也有 FAST/SLOW 区分 | ✅ 有 (slow_compute_interval) |

### 1.3 差异影响

- **性能**: 迁移版本在每个 tick batch 都检查时间防抖，比原始的"仅前月变化触发"更频繁地进入检查逻辑（但实际计算被防抖跳过）
- **正确性**: 迁移版本**缺少超时重试** - 如果前月合约在 20ms 内没有变化，但其他期权合约变化了，原始项目会通过 onComputeValuesTimeout 重新计算，迁移版本可能遗漏

**修复建议**: 在 timer callback 中添加 computeValues 触发（类似 onComputeValuesTimeout）

---

## 二、期权价格 Tick 回调对比

### 2.1 原始项目

```
交易所 Tick 到达
    │
    ├─ IBookListener::onBookChanged(option, book, bidLevelChanged, askLevelChanged)
    │   -> 更新 OptionData 的市场数据
    │   -> 触发 scanner 的 onBookChanged (单合约级低延迟扫描)
    │
    └─ 前月 book 变化 -> 触发 computeValues (如上所述)
```

**关键**: 每个 option 有独立的 IBookListener，scanner 直接监听每个 option 的 book 变化。

### 2.2 迁移项目

```
WTSTickData 到达
    │
    ├─ HftOptionStrategy::on_tick(code, tick)  [HFT回调线程]
    │   -> _async->enqueue_tick(code, tick)    [enqueue到异步队列]
    │
    └─ Worker线程:
        ├─ on_tick_batch: setTime + scanner.onUnderlyingUpdate
        ├─ on_tick(code, tick):
        │   ├─ 如果是 hedge: utd->setMarket(bid, ask)
        │   ├─ PnlTracker 初始化 (preClose)
        │   └─ grid->onTick(code, ref)  -> 更新 OptionData 市场
        └─ on_batch_complete:
            ├─ computeValues (防抖)
            └─ drainPendingQuotes
```

**关键差异**:

| 方面 | 原始 | 迁移 |
|------|------|------|
| Tick 到达到数据更新 | 同步 (IBookListener) | 异步 (enqueue + worker) |
| Scanner 触发 | per-option book change 即时触发 | batch 级 onOptionUpdate |
| 多档行情 | IBook 多档 | OptionMarket 10档 (已实现) |

### 2.3 差异影响

- **延迟**: 迁移版本引入异步队列，增加了一个队列延迟（通常 <1ms）
- **Scanner 延迟**: 原始 scanner 在每个 option book 变化时即时扫描；迁移版本在 batch 完成后统一扫描
- **正确性**: 异步模型更安全（无数据竞争），但牺牲了单合约级低延迟扫描

---

## 三、Scanner 触发与执行对比（最关键）

### 3.1 原始项目 - 完整的自主交易代理

**触发**:
- `IOptionGridListener::onComputeValuesCompleted` - pricer 完成后触发
- `IBookListener::onBookChanged` - 每个 option book 变化即时触发（低延迟单合约扫描）
- 内部 debounce timer - 批量扫描合并

**执行**:
- **MMScanner**: 直接调用 `otd->sendOrder(ord)`，使用 FOK TIF，有超时取消调度
- **StrikeSpreadScanner**: 使用 ComboOrder 子类（SpreadOptionOrders/GutsOptionOrders/SynpairOptionOrders），多腿顺序执行
- **SyntheticFutureScanner**: 使用 SynOptionOrders，3 腿（futures+call+put），含比例处理

**ComboOrder 多腿执行流程**:
```
sendOrders()
    │
    ├─ 检查重复 (IssuedOrderTracker->num_active > 0 -> FAIL_DUPLICATE)
    ├─ 选择最 mispriced 的腿作为 leg1
    ├─ 发送 leg1 (价格改进 +1 tick)
    │
    ├─ onFill(leg1):
    │   ├─ 按实际成交量调整 leg2 size
    │   ├─ 发送 leg2 (价格改进 +1 tick)
    │   └─ 调度 checkOrder1 定时器 (~130ms)
    │
    ├─ onFill(leg2):
    │   ├─ 检查 expectedFill 一致性
    │   └─ checkDone() -> 标记完成
    │
    ├─ checkOrder1 超时:
    │   ├─ 撤销未成交的 leg1
    │   └─ checkDone(timeout=true)
    │
    └─ checkDone():
        ├─ 所有腿 Filled 或 Cancelled -> 完成
        ├─ 更新 m_currentOpenSize (仓位计数器)
        └─ 释放 max_open_size 配额
```

**关键执行特性**:
1. **多腿顺序执行** - 先发 leg1，fill 后再发 leg2/3/4
2. **价格改进** - 对冲腿 +1 tick 提高成交概率
3. **超时取消** - 130ms 定时器，超时撤单
4. **部分成交处理** - leg2 size 按 leg1 实际成交量调整
5. **重复防阻** - IssuedOrderTracker 检查活跃订单数
6. **比例处理** - 期权/期货比例转换 (option_vs_future_ratio)
7. **廉价期权特殊处理** - 价格 < max_tick 的期权假设已成交
8. **腿排序优化** - 按 mispricing 严重程度排序，先发最 mispriced 的腿

### 3.2 迁移项目 - 空壳评分函数

**触发**:
- `HftOptionStrategy` worker 线程在 `on_batch_complete` 中调用:
  - `scanner->onTick(grid)` - 网格级
  - `scanner->onOptionUpdate(option)` - 单合约级

**执行**:
- **9个 scanner 中只有 SpreadScanner 实现了 onTick**，其他 8 个的 onTick/onOptionUpdate 是空函数
- SpreadScanner 的 onTick 计算简单的 `edge = |theo - mid|`，调用 `notifyHit()`
- `notifyHit()` -> `IScannerListener::onScannerHit()`
- `CTG::onScannerHit()` -> **仅 log + 写入 m_optUpdateSet（死代码，refresh() 从不读取）**
- **没有任何 scanner 下单**

**ComboOrder 迁移状态**:
- `ComboOrder` 基类存在（`OptionOrder.h`），有 `Leg{order, ratio, expectedFill}` 结构
- `sendOrders()` 是纯虚函数，**无任何子类实现**
- 原始的 5 个 ComboOrder 子类（SpreadOptionOrders/GutsOptionOrders/SynpairOptionOrders/SynOptionOrders 等）**全部缺失**
- `ComboOrder` 从未被实例化

### 3.3 缺失清单

| 功能 | 原始 | 迁移 | 影响 |
|------|------|------|------|
| Scanner 下单执行 | 直接 sendOrder | ❌ 无 | Scanner 无法交易 |
| ComboOrder 子类 | 5 个子类 | ❌ 0 个 | 多腿策略无法执行 |
| 多腿顺序执行 | leg1->fill->leg2 | ❌ 无 | 套利无法分腿执行 |
| 价格改进 (+1 tick) | ✅ | ❌ | 对冲腿成交率低 |
| 超时取消定时器 | ~130ms | ❌ | 挂单无法自动撤 |
| 部分成交处理 | leg2 size = leg1 fill | ❌ | 腿间数量不匹配 |
| 重复防阻 | IssuedOrderTracker | ❌ | 可能重复下单 |
| 比例处理 | option_vs_future_ratio | ❌ | 期权/期货比例转换缺失 |
| 廉价期权处理 | < max_tick 假设成交 | ❌ | 深度 OTM 处理缺失 |
| 腿排序优化 | 按 mispricing 排序 | ❌ | 最优腿优先缺失 |
| ScannerInfo 标记 | 每腿带 scanner 名+腿号 | ❌ | 成交路由缺失 |
| evalSpread/evalGuts/evalSynPair | 完整 mispricing 计算 | ❌ | 仅有简单 edge |
| max_open_size 配额 | 仓位计数器 | ❌ | 无套利仓位限制 |
| FOK TIF | FillOrKill | ❌ | 无 FOK 支持 |

### 3.4 这是迁移中最大的功能回归

定价/曲面/网格基础设施迁移完整，CTG refresh/rank/TPS 是好的移植。但**整个 scanner -> 订单执行路径 -- 实际赚钱的部分 -- 完全缺失**。

---

## 四、修复方案

### 4.1 Pricer 调度修复 (低优先级)

在 timer callback 中添加 computeValues 触发（类似 onComputeValuesTimeout）:
```cpp
// 在 on_timer 回调中:
if (_pricer && _traderCtx->enabled) {
    double now = ctxTimeSeconds(_ctx);
    if ((now - _lastComputeTime) >= _minComputeInterval) {
        _grid->computeValues(_pricer.get());
        _lastComputeTime = now;
    }
}
```

### 4.2 Scanner 执行修复 (高优先级 - 核心)

需要实现完整的 scanner -> 下单执行链路:

**阶段1: ComboOrder 子类实现**
- 实现 `SpreadComboOrder` (2腿价差)
- 实现 `SynComboOrder` (3腿合成期货)
- 实现 `sendOrders()` - 多腿顺序执行
- 实现 `onFill()` - 部分成交处理
- 实现 `checkDone()` - 完成判定 + 超时

**阶段2: Scanner 执行逻辑**
- 恢复 `evalSpread`/`evalGuts`/`evalSynPair` mispricing 计算
- 恢复 IssuedOrderTracker 重复防阻
- 恢复 max_open_size 仓位配额
- 恢复价格改进 +1 tick
- 恢复超时取消定时器

**阶段3: Scanner 触发恢复**
- 在 `onComputeValuesCompleted` 中触发 scanner (原始方式)
- 或在 `on_batch_complete` 的 computeValues 后触发

**预估工时**: 5-8 天（这是最复杂的部分）
