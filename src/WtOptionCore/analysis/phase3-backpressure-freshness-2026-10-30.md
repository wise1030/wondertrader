# WtOptionCore 阶段 3：背压保护与市场新鲜度执行记录

- **日期**: 2026-10-30
- **范围**: 队列背压保护 + stale market freshness gate
- **验证**: 主构建 `src/build_all` 编译通过；WtOptionCore 7/7 CTest 通过

---

## 已实现变更

### 1. AEP trader/control 队列不再静默丢事件

文件：

- `OptionAsyncEventProcessor.h/.cpp`

变更：

- trader 队列满时 producer 短暂等待 worker drain（20ms backpressure）。
- worker drain 后通知等待 producer。
- 等待后仍失败才计数、记录 ERROR 并返回失败。
- trade/order/position/session/channel 事件不再像普通行情一样直接丢弃。

### 2. MD overflow 保留最新合约行情

变更：

- tick overflow 按合约代码合并，始终保留同合约最新 tick。
- overflow 满时优先淘汰最旧 tick；没有 tick 时淘汰最旧事件，确保新行情仍可表示。
- worker 先 drain overflow 再 drain SPSC ring，避免旧 overflow tick 后写覆盖更新 ring tick。
- overflow 未消费完时保留剩余事件，不再 `clear()` 丢弃。

### 3. 行情时间戳进入 OptionGrid / UTD

文件：

- `OptionAsyncEventProcessor.cpp`
- `HftOptionStrategy.cpp`
- `OptionGrid.h/.cpp`
- `UnderlyingTradingData.h`

变更：

- `TickDataRef` 增加：
  - `actionTime`
  - `updateTime`（本地接收 epoch ms）
- `OptionMarket.updateTime` 在异步路径正确赋值。
- underlying 增加最近行情时间。
- UTD 记录 future/hedge 行情接收时间。
- AEP tick `updateTime` 改用 epoch milliseconds，避免日内毫秒跨午夜和延迟统计单位混乱。

### 4. stale market freshness gate

文件：

- `OptionGrid.h/.cpp`
- `UnderlyingTradingData.h`
- `CompositeOptionPricer.h/.cpp`
- `HftOptionStrategy.h/.cpp`
- `ConfigValidator.h`

变更：

- 新增 `marketFreshTimeoutMs` 配置，默认 3000ms；0 关闭检查。
- OptionGrid 提供：
  - `isUnderlyingFresh(nowMs)`
  - `hasFreshOptionMarket(nowMs)`
- Hft 每 batch 检查：
  - underlying 是否 fresh
  - 是否至少一个 option 行情 fresh
  - active UTD 是否 fresh
- 行情 stale 时：
  - 首次进入 stale 状态会刷新 CTG desired 并强制 drain 撤单；
  - 后续 batch 暂停 quote refresh / scanner / pricer drain；
  - 行情恢复后自动恢复 refresh。
- CompositeOptionPricer 生成单个期权报价前再次检查该合约 `OptionMarket.updateTime`，避免整体尚有新鲜合约时继续更新陈旧合约报价。
- ConfigValidator 加入 `marketFreshTimeoutMs`，避免合法配置误报。

---

## 新增测试

### `test_md_overflow_coalesces_ticks_by_code`

覆盖：

- overflow 满后仍保留最新 tick；
- 同合约 tick 合并；
- 非 tick 事件满时新 tick 可淘汰旧事件；
- drop 计数正确。

### `test_optiongrid_market_freshness`

覆盖：

- underlying fresh/stale；
- option market fresh/stale；
- `OptionMarket.updateTime` 正确传递；
- timeout=0 时保持兼容。

---

## 验证

构建：

```bash
cmake --build src/build_all \
  --target WtOptionCore \
  test_ctg_ranking test_enhancements test_scanners test_stress \
  test_hedge_coordinator test_command_handlers test_async_event_processor \
  -j4
```

测试：

```bash
ctest --test-dir src/build_all/WtOptionCore --output-on-failure
```

结果：

```text
7/7 tests passed
100% tests passed, 0 tests failed
```

`git diff --check -- src/WtOptionCore` 通过。

---

## 尚未完成

1. SynCombo top-up 多 localid 生命周期。
2. 重启工作单 Recovering 对账。
3. 正式 OrderGateway 类及 raw ctx fallback 移除。
4. PositionBook 完整 long/short 双快照模型。
5. ConfigLoader typed schema / effective-value dump。
6. worker 减负、dirty-set、FAST 增量化、延迟基准。
7. Scanner/Gamma/Power 功能语义完整实现。
