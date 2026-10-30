# WtOptionCore 阶段 6：完整 PositionBook 与 FIFO 成交桥接

- **日期**: 2026-10-30
- **范围**: 正式 long/short PositionBook、broker 快照受控成交桥接、乱序/重启回归
- **验证**: 主构建 `src/build_all` 编译通过；WtOptionCore 7/7 CTest 通过

---

## 已实现变更

### 1. 新增正式 PositionBook

文件：

- `PositionBook.h`（新增）
- `CMakeLists.txt`

数据模型：

```cpp
SidePosition {
    prevVol, prevAvail,   // 昨仓 / 昨可平
    newVol, newAvail      // 今仓 / 今可平
}

InstrumentPosition {
    longPos,
    shortPos,
    net(),
    netAvail(),
    gross()
}
```

支持：

- broker long/short 双边快照；
- 昨仓 + 今仓；
- 可平量 today/prev 拆分；
- fill 增量桥接；
- `snapshot()` 只读快照；
- `version()` 单调版本；
- `clearAll()` 用于 channel recovery / session recovery 清旧状态。

### 2. AEP trader 事件改为 FIFO

文件：

- `OptionAsyncEventProcessor.cpp`

原先固定：

```text
position → trade → order
```

会在同一 batch 中把 broker 快照强制排到成交前，破坏 PositionBook 的快照/成交桥接。

现在：

- session/channel/position/trade/order 全部保持 enqueue/FIFO 顺序；
- timer 仍排在 trader 事件之后；
- tick 仍在 trader/timer 后统一 batch 更新。

这是 PositionBook 受控桥接的基础。

### 3. HftOptionStrategy 接入 PositionBook

文件：

- `HftOptionStrategy.h/.cpp`

变更：

- 新增 `_positionBook`；
- `_positions` 保留为 legacy net projection，供 session accounting / PnL pending init 使用；
- `on_position`：
  - PositionBook 应用完整 long/short prev/new snapshot；
  - net 投影更新 `_positions`；
  - OptionRiskData 同步；
  - hedge position 同步；
  - PositionGuard 接收完整快照；
  - PositionOffsetMgr 接收完整快照。
- `on_trade`：
  - PositionBook 增量 addFill；
  - net 投影从 PositionBook 重新计算。
- channel ready 进入 Recovering 时：
  - 清空 PositionBook；
  - 清空 legacy `_positions`；
  - 等待 broker snapshot 重新建立权威状态。

### 4. PositionOffsetMgr 支持完整快照

文件：

- `PositionOffsetMgr.h/.cpp`

新增：

```cpp
onPositionUpdate(const PositionBook::InstrumentPosition&)
```

同时设置：

- long/short 总仓；
- long/short 总可平；
- today/prev 可平拆分。

### 5. PositionGuard 支持完整快照入口

文件：

- `PositionGuard.h/.cpp`

新增：

```cpp
onBrokerPosition(const PositionBook::InstrumentPosition&)
```

当前 Guard 内部差异模型仍是 signed-net，因此新 overload 将完整快照归一为 `pos.net()` 后进入现有逻辑；接口已统一，后续如需双边 Guard 可在不改变 Hft 接线的情况下扩展。

---

## 新增测试

### `test_position_book_long_short_bridge`

覆盖：

- prev+new 昨仓/今仓合并；
- long/short 双边快照；
- net/gross/netAvail；
- fill 增量桥接；
- version 单调；
- restart clear 后 broker snapshot 取代旧状态；
- PositionOffsetMgr 获取 long today/prev 可平拆分。

### `test_trader_events_preserve_fifo_order`

覆盖：

- trade 先入队时，先处理 trade，再处理 position；
- 防止 AEP 重新按 position-before-trade 排序。

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

## 当前边界

- PositionBook 已保存完整 long/short 双边事实；
- PositionGuard 内部仍是 net discrepancy 模型；
- OQM/RiskData 当前使用 net position，这是现有 OQM 状态模型接口决定的；
- 若未来需要按 long/short 独立 Guard/OQM 风控，可在 PositionBook 快照基础上继续扩展。

---

## 尚未完成

1. typed ConfigLoader / effective-value dump。
2. worker 减负与延迟基准。
3. Scanner/Gamma/Power 语义完整实现。
