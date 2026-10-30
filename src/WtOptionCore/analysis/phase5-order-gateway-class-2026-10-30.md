# WtOptionCore 阶段 5：正式 OrderGateway 与 OQM raw ctx 移除

- **日期**: 2026-10-30
- **范围**: 将阶段 1 的 sender seams 收敛为正式 OrderGateway，并移除 OQM 的 raw IHftStraCtx fallback
- **验证**: 主构建 `src/build_all` 编译通过；WtOptionCore 7/7 CTest 通过

---

## 已实现变更

### 1. 新增正式 OrderGateway

文件：

- `OrderGateway.h`（新增）
- `CMakeLists.txt`

接口只包含生产 OQM 所需的四类操作：

```cpp
submitQuote(code, bidP, bidQ, askP, askQ) -> pair<bidId, askId>
submitOrder(code, isBuy, price, qty) -> vector<localid>
cancelOrder(localid) -> bool
cancelAll(code)
```

`ready()` 用于检查四类 handler 是否齐全。

### 2. HftOptionStrategy 创建生产 Gateway

文件：

- `HftOptionStrategy.h/.cpp`

Hft 在装配 OTG 时创建 `_orderGateway`，并把以下策略出口包装进去：

- `executeQuoteIds`
- `executeOrderIds`
- `executeCancel(uint32_t)`
- `executeCancel(code)`

因此 OQM 订单仍经过统一：

- 价格限幅检查
- FillPriceChecker 登记
- OrderAnomalyGuard 登记
- localid 返回与跟踪

### 3. OTG 持有 OrderGatewayPtr

文件：

- `OptionTradingGrid.h/.cpp`

变更：

- 移除分散的 `setSubmitQuoteFn/setSubmitOrderFn/setCancelOrderFn/setCancelAllFn`。
- 改为单一 `setOrderGateway(OrderGatewayPtr)`。
- option/future OQM 创建时统一从 gateway 获取 sender。

### 4. OQM 彻底移除 raw ctx fallback

文件：

- `OptionQuoteManager.h/.cpp`

变更：

- 构造函数不再接收 `wtp::IHftStraCtx*`。
- 删除 `m_ctx`。
- 删除所有 `m_ctx->stra_quote/stra_buy/stra_sell/stra_cancel/stra_cancel_all` 路径。
- 未注入 sender 时 OQM 对发单/撤单完全 inert，而不是绕过策略层直接访问框架 API。

保留的 OQM 注入接口仅作为 adapter 边界：

- `SendQuoteFn`
- `SendSingleFn`
- `CancelOrderFn`
- `CancelAllFn`
- `OrderIssuedFn`

这些函数现在一律由 OTG 从正式 `OrderGateway` 包装而来。

---

## 测试更新

### `test_otg_gateway_senders`

扩展覆盖：

- buy/sell OQM 使用 `submitOrder`；
- paired quote OQM 使用 `submitQuote`；
- paired quote 返回的 bid/ask ID 均登记；
- cancel 走 gateway。

### `test_oqm_enhancements`

新增 inert 回归：

- 不注入 gateway sender 时，`updateOrders()` 不发送任何订单；
- OQM 不会尝试直接访问 `IHftStraCtx`。

### 既有测试适配

- 所有直接构造 `OptionQuoteManager` 的测试移除第三个 `nullptr ctx` 参数。
- OTG 动态接线测试改用 `OrderGateway::Handlers`。

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

静态检查：

- `OptionQuoteManager.*` 中已无 `IHftStraCtx` / `m_ctx->stra_*`。
- `git diff --check -- src/WtOptionCore` 通过。

---

## 尚未完成

1. 完整 long/short PositionBook。
2. typed ConfigLoader / effective-value dump。
3. worker 减负与延迟基准。
4. Scanner/Gamma/Power 语义完整实现。
