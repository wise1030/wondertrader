# WtOptionCore 阶段 2：TimeService 执行记录

- **日期**: 2026-10-30
- **前置提交**: `035ec65b fix(option): Phase0阻断修复——AEP exactly-once/STOP/订单ID/动态接线/持仓合约事实`
- **范围**: 阶段 2 第一批“跨午夜连续事件时间”
- **验证**: 主构建 `src/build_all` 编译通过；WtOptionCore 7/7 CTest 通过

---

## 目标

WonderTrader 暴露的 `stra_get_time()/stra_get_secs()` 是日内时间。夜盘跨过 00:00 后，直接使用 seconds-of-day 会让以下 duration 变成负值或冻结：

- EMA decay
- FAST/SLOW 计算防抖
- CTG TPS 窗口
- OQM TTL / cancel ack watchdog / reject retry
- Delta hedge cooldown
- Panic blackout
- Fitter 定时
- Gamma hedge cooldown
- 队列告警限频

本轮先引入**每策略实例的连续事件时间源**，保持现有接口和小步可回滚。

---

## 已实现变更

### 1. 新增 OptionTimeService

文件：

- `OptionTimeService.h`
- `CMakeLists.txt`

特性：

- 输入仍为 `stra_get_time()/stra_get_secs()`。
- 仅在“上一时间位于 23:00 后、新时间位于 01:00 前”时累加 24h 偏移。
- 普通 session 内回退不会误加一天。
- session reset 不改变单调时间轴，避免 EMA 等跨 session 状态收到回退时间。
- 单策略实例持有，不使用函数级 static。

### 2. HftOptionStrategy 使用连续时间

文件：

- `HftOptionStrategy.h/.cpp`

变更：

- 新增 `_timeService`。
- 以下路径改用 `_timeService.nowSeconds(_ctx)`：
  - AnomalyGuard
  - PredictionEvaluator
  - CTG getTime/TPS
  - PositionGuard clock
  - pricer/signal time
  - batch compute debounce
  - stress timer
  - fill signal time
  - expiry lifecycle timer
  - scanner combo timeout
  - delta hedge cooldown
  - fill/expiry simulator timestamp
- session begin 调用 `_timeService.reset()`；该 reset 保留连续时间轴。
- 修正 `_tickTimestampUs`：从错误的 `stra_get_secs()*1e6` 改为连续 event microseconds。

### 3. OQM TTL / retry clock 接入 TimeService

文件：

- `OptionTradingGrid.h/.cpp`

变更：

- 新增 `TimeFn` 注入。
- option/future OQM 的 `setGetTimeFn()` 优先使用 TimeService；未注入时保留 raw `ctxTimeSeconds` fallback。

### 4. Gamma hedge cooldown 接入 TimeService

文件：

- `GammaScalpOptionPricer.h/.cpp`
- `HftOptionStrategy.cpp`

变更：

- 新增 `setTimeProvider()`。
- Gamma 动态对冲 cooldown 从本地 ms-of-day 改为连续策略时间。
- 未注入时保留 standalone fallback。

### 5. AEP 队列告警限频修正

文件：

- `OptionAsyncEventProcessor.cpp`

变更：

- `TimeUtils::getLocalTimeNow()` 是毫秒，不是微秒；限频阈值从 `5,000,000` 修正为 `5000ms`。
- 处理本地毫秒跨午夜回绕，避免告警限频失效一整天。

---

## 测试

新增：

- `test_time_service_midnight_rollover`

覆盖：

- `23:59:59.900 → 00:00:00.100` 后时间仍单调递增。
- hedge cooldown 跨午夜仍为 cooldown。
- session reset 不回退连续时间轴。
- 普通 session 内 backwards sample 不会误加一天。

全量测试：

```text
7/7 tests passed
100% tests passed, 0 tests failed
```

---

## 当前边界

本轮解决的是**策略事件时间/duration** 的跨午夜问题。

以下独立问题仍未完成：

1. `ExpiryData::getIntradayFraction()` 仍直接读取本地 minute；需要注入 session/time source 后彻底统一。
2. `OptionAsyncEventProcessor` 中 tick 的 `updateTime` 与 quote latency 的展示/统计口径仍需端到端梳理。
3. Scanner `system_clock` 日志时间不是交易核心路径，但后续应统一。
4. `on_timer` 源头仍使用 raw seconds-of-day 做 100ms dedup；这是有意保留的生产路径，因为 producer 需要无状态判断跨日并立即放 timer。

---

## 建议下一步

优先执行 **queue overflow fail-safe + stale market freshness gate**：

- MD 只按 code 保留最新；
- trader/control 事件不可丢；
- 队列满或行情 stale 后进入 Disabled/Panic，撤单并完成 freshness gate 后恢复；
- 为 OptionGrid / OQM / Pricer / Hedge 加入统一 market freshness 检查。
