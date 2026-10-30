# WtOptionCore 阶段 4：SynCombo 生命周期与 Recovering 对账门控

- **日期**: 2026-10-30
- **范围**: SynCombo 多 localid/top-up/unwind 生命周期 + 重启/重连 Recovering gate
- **验证**: 主构建 `src/build_all` 编译通过；WtOptionCore 7/7 CTest 通过

---

## 已实现变更

### 1. SynCombo 每腿记录全部 localid

文件：

- `ComboOrders.h/.cpp`

`LegExec` 新增：

- `localIds`：该腿所有订单 ID，包括 top-up；
- `openIds`：尚未收到终态回调的订单 ID；
- 保留 `localId` 作为首个 ID 的兼容字段。

所有发单路径统一通过 `recordLegId()` 记录：

- 初始腿发单；
- 后续腿发单；
- hedge top-up。

`onLegFill()` / `onLegTerminal()` 不再只匹配单个 `localId`，而是匹配该腿全部 ID。

### 2. SynCombo 终态管理

实现：

- 每个订单 ID 都独立等待 terminal/cancel callback；
- 只有对应腿全部 ID 终态后才认为该腿订单侧结束；
- `checkDone()` 同时要求：
  - 全部腿成交量达到目标；
  - 全部 leg/top-up ID 终态；
  - residual unwind ID 终态。

### 3. SynCombo 残差 unwind

实现：

- `markFailed()` 先撤销所有仍 open 的 leg/top-up/unwind ID；
- 对已成交但未完全对冲的腿自动发送反向 residual unwind 单；
- unwind 单 ID 进入 `m_unwindIds/m_unwindOpen`，继续等待 terminal；
- unwind 发单失败会记录 ERROR，提示人工平仓。

说明：

- unwind 使用原腿价格的反向单，属于明确的最小自动恢复动作；后续如要引入盘口价/风险平价逻辑，应单独变更。

### 4. Strategy 不再提前删除失败/超时 Combo

文件：

- `HftOptionStrategy.cpp`

变更：

- Spread/Syn timeout 后不再立即从 `_activeCombos` 移除；
- 新增/使用 `hasOpenOrders()`，直到 leg/top-up/unwind 全部 terminal 才释放管理对象；
- 迟到成交/撤单仍可路由到 combo。

### 5. Recovering 状态与重启门控

文件：

- `TradingState.h`
- `HftOptionStrategy.cpp`
- `tests/test_enhancements.cpp`

变更：

- 新增 `TradingStateType::Recovering`。
- Recovering 下 `enabledFlag() == false`，不允许报价。
- channel ready / session begin 后先进入 Recovering：
  - 清空 underlyingReady；
  - 发起交易所全合约撤单 sweep；
  - 等待 broker position snapshot 与第一笔 fresh underlying tick。
- 第一笔 underlying tick 到达后，才允许 OTG enable 并切换到 Normal。
- channel lost / session begin 会清除 underlyingReady，避免使用旧状态立即恢复。
- TradingState 单测覆盖 Recovering flag 语义。

说明：

- 当前框架的 `IHftStraCtx` 没有提供“枚举交易所工作单并认领”的接口；本轮实现的是可落地的 Recovering gate + 全量撤单 sweep + broker 仓位快照同步。
- 完整“按订单认领/对账”需要 WonderTrader 增加工作单枚举或快照 API。

---

## 新增测试

### `test_syn_combo_unwind_lifecycle`

覆盖：

- leg0 partial fill 后发送 leg1；
- leg1 reject/cancel 触发失败；
- 自动取消仍 open 的 leg0；
- 对 leg0 已成交残差发送反向 unwind；
- unwind ID 纳入管理；
- 迟到 terminal 释放 combo。

### `test_trading_state_flags`

新增 Recovering：

- `enabledFlag() == false`
- `panickedFlag() == false`

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

1. 框架级工作单枚举/认领 API（当前用 Recovering + cancel sweep 兜底）。
2. 正式 `OrderGateway` 类与 raw ctx fallback 移除。
3. 完整 long/short PositionBook。
4. typed ConfigLoader / effective-value dump。
5. worker 减负与延迟基准。
6. Scanner/Gamma/Power 语义完整实现。
