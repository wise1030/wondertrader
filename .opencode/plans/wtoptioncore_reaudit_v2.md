# WtOptionCore 第二轮全面诊断报告

> 基于 5 个 Phase 优化后的重新审计，与原项目 quantbox/optiontrader 逐行对比

## 一、项目架构维度

### 已修复的问题 (Phase 1-5)
- ✅ HftOptionStrategy/UftOptionStrategy 代码重复 → UFT 已删除
- ✅ OptionGrid ↔ OptionRisk/OptionPricer 集成 TODO → 已清除
- ✅ Scanner 未接入 HFT → setupScanners() 已实现
- ✅ WtOptContext 职责膨胀 → 延后 (仅 legacy 路径使用)

### 新发现的架构问题

#### A1-CRITICAL: 数据竞争 - _positions 在多线程间无同步访问

**严重程度**: P0 (可能导致 crash 或数据损坏)

`_positions` (`std::unordered_map<std::string,double>`) 被两个线程无锁访问:
- **HFT 回调线程**: `on_position()` 写入 (`HftOptionStrategy.cpp:1074,1076`)
- **Worker 线程**: `on_trade()` 写入 (`:760`), `on_tick()` 读取 (`:580`), `on_batch_complete()` 读取 (`:637`)

`std::unordered_map` 的并发读写是**未定义行为**，可能导致 crash 或数据损坏。

**修复方案**: `on_position()` 改为 enqueue 到 async processor，或用 `std::mutex` 保护 `_positions`。

#### A2-CRITICAL: 数据竞争 - _pnlPendingInit 在多线程间无同步访问

**严重程度**: P0

`_pnlPendingInit` (`std::set<std::string>`) 被两个线程无锁访问:
- **HFT 回调线程**: `on_position()` 插入 (`:1082`)
- **Worker 线程**: `on_tick()` 读取+删除 (`:578-579`)

**修复方案**: 同 A1，改为 enqueue 或加锁。

#### A3-HIGH: _traderCtx->enabled/panicked 非原子 bool

**严重程度**: P1

`OptionTraderContext` 的 `enabled` 和 `panicked` 是普通 `bool`，被 3 个线程访问:
- HFT 回调线程: `on_channel_ready/lost` 设置 `enabled`
- Worker 线程: 读取 `enabled`/`panicked`
- 热更新线程: `on_params_updated` 设置两者

**修复方案**: 改为 `std::atomic<bool>`。

#### A4-MEDIUM: CompositeOptionPricer 是上帝对象

**严重程度**: P2

505 行 header / 1810 行 cpp，承担: FAST/SLOW 计算、alpha/risk 调整、trade-shock、EMA 过滤器、信号管理、panic 状态、到期配置等。

**修复方案**: 提取 AlphaAdjuster、RiskAdjuster、TradeShockHandler 为独立组件。

#### A5-LOW: OptionTraderContext 过于简化

仅 2 个 bool + 1 个 function。缺少: 仓位限制、账户状态、风控限制、交易计数器。

---

## 二、业务逻辑维度

### 已恢复的业务逻辑 (Phase 1-5)
- ✅ 排序算法 (isBest、类型权重、穿越检测)
- ✅ Panic TPS 增强 + 保留 futures 对冲
- ✅ Panic Signal 自动检测
- ✅ Compute 防抖
- ✅ Mid-Day Session 调度
- ✅ Late Fill 检测
- ✅ Risk Free Rate Curve
- ✅ Multi-level Market (10 档深度)
- ✅ Multi-source Market 合并 (slot 0+2)
- ✅ Scanner 接入
- ✅ Front Month 换月
- ✅ Secondary Hedge
- ✅ AttributePublisher
- ✅ ManualOrderManager (hot-param)
- ✅ ExpirationSimulator
- ✅ OptionValueWriter
- ✅ Predictor 基础设施

### 新发现的业务逻辑差异

#### B1-MEDIUM: 禁用模式下未清空 future markets

**原始行为** (`ControllableTradingGrid.cc:425-430`):
```cpp
if(!m_spOptionTraderContext->enabled()) {
    BOOST_FOREACH( const OptionTradingDataPtr& otd, otd_list )
    {    otd->multiMarket().clear(); }
    BOOST_FOREACH( const UnderlyingTradingDataPtr& utd, utd_list )
    {    utd->multiMarket().clear(); }
}
```
原始在 `enabled=false` 时清空**所有** option 和 future markets。

**迁移行为** (`ControllableTradingGrid.cpp:157`):
```cpp
if (!m_grid || !m_ctx->enabled) return;
```
迁移在 `enabled=false` 时直接返回，**不清空任何 markets**。已有的 desired markets 残留。

**修复方案**: 在 `enabled=false` 时清空 option+future desired markets 后再返回。

#### B2-MEDIUM: onSetQMode 缺少 market 清除和 computeValues 触发

**原始行为** (`ControllableTradingGrid.cc:832-892`):
- 解析命令字符串获取 instrument + mode
- 清空 `values(0).ourMarket()` 和 `values(2).ourMarket()`
- 触发 `computeValues()`
- 发布属性变更

**迁移行为** (`ControllableTradingGrid.cpp:108-121`):
- 仅设置 quote mode
- **不清空 markets**
- **不触发 computeValues**
- 不发布属性

**修复方案**: 添加 market 清除和 computeValues 调用。

#### B3-LOW: combineMarkets 缺少 discard 优化

**原始行为**: `combineMarkets()` 有 discard 检查: 如果 desired 空 + last_desired 空 + current 空 → 跳过 (不加入 update list)。

**迁移行为**: 无 discard 检查，所有 options 都进入 pending quotes（即使 UT_NONE 会被跳过）。

**修复方案**: 添加 discard 快速路径（性能优化，非业务逻辑丢失）。

#### B4-LOW: 原始的 1 秒 wakeup call 重试

**原始行为**: dropped > 0 时通过 `ClockMonitor::scheduleWakeupCall` 在 1 秒后触发 `refresh()` 重试。

**迁移行为**: dropped quotes 移到 `m_droppedQuotes`，在下一个 refresh 周期重试。但没有主动的 1 秒定时重试——如果没有新 tick 到来，dropped quotes 可能长时间不被重试。

**修复方案**: 在 timer callback 中检查 `m_droppedQuotes` 非空时触发 refresh。

#### B5-LOW: onFillWithFees 的完整 late fill 逻辑

**原始行为**: `onFillWithFees` 检查合约是否在当前周期被更新 (`bUpdated`)，未更新则标记 `setLateFill(true)`，递增 `m_lateFills`/`m_totalFills`，通知 `services()->notifyFill()`。

**迁移行为**: OQM 的 `onFill` 检查时间差标记 late fill，但:
- **缺少** `m_totalFills` 计数器
- **缺少** `services()->notifyFill()` 通知
- late fill 标记在 OQM 层而非 OrderInfo 层

---

## 三、代码优化维度

### 已修复的问题 (Phase 1-5)
- ✅ 22 处 TODO 标记 → 已清除
- ✅ 时间解析不一致 → 统一为 ctxTimeSeconds()
- ✅ object_pool 非线程安全 → 已移除
- ✅ 合约信息未接入 → fees/tickSize 已接入
- ✅ 重复文件 → SignalFactory.h/IAlphaSignal.h/ForecastSignal.h 已删除

### 新发现的代码问题

#### C1-MEDIUM: 过时注释仍声称"not yet migrated"

| 文件 | 行 | 过时内容 |
|------|-----|---------|
| `OptionPricer.h` | 8 | "OptionRisk, OptionGrid (not yet migrated)" |
| `OptionPricer.h` | 19 | "this file will not compile until they land" |
| `OptionOrderInfo.cpp` | 42 | "OptionTradingGrid access omitted - grid not yet migrated" |
| `FutureOrderInfo.cpp` | 39 | "OptionTradingGrid not yet migrated; left as NaN" |

这些组件早已迁移完成，注释过时且具有误导性。

#### C2-MEDIUM: _computePending 是死代码

`HftOptionStrategy.cpp:682,699` 设置 `_computePending` 但从未读取。原始设计意图是触发延迟计算，但未实现。

#### C3-LOW: WtOptionStrategy 的空函数

`WtOptionStrategy.cpp` 有 3 个空函数:
- `checkPanic()` (L652)
- `updatePnL()` (L668)
- `processOrders()` (L691)

这些是 legacy 路径的 stub，不影响 HFT 路径。

#### C4-LOW: fprintf(stderr) 在 HFT 回调线程

`HftOptionStrategy.cpp:1032`: 首个 tick 时 `fprintf(stderr, ...)` 在 HFT 回调线程同步执行，可能阻塞。

---

## 四、性能提升维度

### 已实现的优化 (Phase 1-5)
- ✅ Compute 防抖 (20ms)
- ✅ 增量 Greeks 更新 (dirty flag)
- ✅ 优先级事件排序
- ✅ Drop tracking + retry

### 新发现的性能问题

#### P1-HIGH: OpenMP 并行化是死代码

`use_tbb_parallel_for` 默认 `false` (`OptionPricer2.h:113`)，**从未被设置为 true**。`#pragma omp parallel for` 存在于 `OptionPricer2.cpp:297,400` 但**永远不会执行**。`CompositeOptionPricer` 的主计算路径完全串行。

**修复方案**: 在 config 中添加 `use_parallel` 选项，或移除死代码。

#### P2-HIGH: 异步队列是 mutex-based，非 lock-free

`OptionAsyncEventProcessor` 使用 `std::mutex + std::deque`。讽刺的是，legacy 的 `WtOptContext` 使用了真正的 `boost::lockfree::spsc_queue`。HFT 回调线程在 enqueue 时会阻塞。

**修复方案**: 替换为 `boost::lockfree::spsc_queue` 或无锁环形缓冲区。

#### P3-MEDIUM: 每批次 std::stable_sort 浪费

`worker_loop` 对每批次做 `std::stable_sort`（O(N log N)）。典型批次只有几个 tick + 1 timer，排序开销不必要。

**修复方案**: 仅当有非 tick 事件时才排序，或用桶排序（5 个优先级 → 5 个桶，O(N)）。

#### P4-MEDIUM: tick dedup 每批次分配 std::map + std::string

`OptionAsyncEventProcessor.cpp:284`: 每批次创建 `std::map<std::string, const TickData*>`，每个 distinct code 分配一个 `std::string` key。`getCode()` 返回 `std::string` by value（`h:126`），每次调用都堆分配。

**修复方案**: 使用 `unordered_map<const char*, ...>` 或 flat hash map。

#### P5-MEDIUM: on_batch_complete 6 次线性遍历 getAllOptions()

每批次对 `getAllOptions()` 做 ~6 次独立遍历: positions、PnlTracker、portfolio PnL、scanner、attributes、publish。

**修复方案**: 融合为单次遍历。

#### P6-LOW: 每笔成交 make_shared<OrderStub>

`HftOptionStrategy.cpp:772`: 每笔成交 `std::make_shared<OrderStub>()` 堆分配。成交频率低于 tick，可接受但可用栈分配优化。

---

## 五、错误处理维度

#### E1-CRITICAL: Worker 线程无 try/catch，异常会崩溃进程

`BlackCalc.cpp` 在 9 处抛出 `std::runtime_error`。主计算路径 (`computeValues_FAST/SLOW`、`computeValue`、`worker_loop`) **无 try/catch**。异常传播将终止 worker 线程并崩溃进程。

**修复方案**: 在 `worker_loop` 和 `on_batch_complete` 中添加 try/catch，异常时跳过当前批次而非崩溃。

---

## 六、实施优先级

| 优先级 | 编号 | 问题 | 修复方案 | 预估工时 |
|--------|------|------|---------|---------|
| P0 | A1 | _positions 数据竞争 | on_position 改为 enqueue | 0.5天 |
| P0 | A2 | _pnlPendingInit 数据竞争 | 同上 | 0.5天 |
| P0 | E1 | Worker 无异常保护 | try/catch 包裹 worker_loop | 0.5天 |
| P1 | A3 | enabled/panicked 非原子 | 改为 atomic<bool> | 0.5天 |
| P1 | P1 | OpenMP 死代码 | 添加 config 开关或移除 | 0.5天 |
| P1 | P2 | 队列非 lock-free | 替换为 lockfree queue | 1天 |
| P2 | B1 | 禁用模式未清空 markets | enabled=false 时清空 | 0.5天 |
| P2 | B2 | onSetQMode 不完整 | 补充 market 清除 + compute | 0.5天 |
| P2 | P3 | 每批次 stable_sort | 桶排序或条件跳过 | 0.5天 |
| P2 | P4 | dedup 分配 std::string | flat hash map | 0.5天 |
| P2 | P5 | 6 次线性遍历 | 融合为单次 | 1天 |
| P3 | C1 | 过时注释 | 清理 | 0.5天 |
| P3 | C2 | _computePending 死代码 | 移除或实现 | 0.5天 |
| P3 | B4 | 1 秒 wakeup retry | timer 中检查 dropped | 0.5天 |
| P3 | C4 | fprintf 在回调线程 | 移除或改异步 | 0.1天 |
| P3 | A4 | God object 拆分 | 提取子组件 | 3天 |
