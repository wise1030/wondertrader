# WtOptionCore 阶段 1：OrderGateway 接缝执行记录

- **日期**: 2026-10-30
- **前置提交**: `035ec65b fix(option): Phase0阻断修复——AEP exactly-once/STOP/订单ID/动态接线/持仓合约事实`
- **范围**: 阶段 1 第一批“生产订单统一出口接缝”
- **验证**: 主构建 `src/build_all` 编译通过；WtOptionCore 7/7 CTest 通过

---

## 目标

在不重写 OQM 状态机的前提下，让生产订单不再绕开策略层的：

- 交易所价格限幅检查
- FillPriceChecker 发单登记
- OrderAnomalyGuard 发单登记
- 真实 localid 追踪

同时为后续完整 `OrderGateway` 类提取保留清晰边界。

---

## 已实现变更

### 1. OQM 支持生产 sender 注入

文件：

- `OptionQuoteManager.h/.cpp`

新增注入接口：

- `SendQuoteFn`
- `SendSingleFn`（已有）
- `CancelOrderFn`
- `CancelAllFn`
- `OrderIssuedFn`（上一阶段新增）

生产路径优先使用注入函数；仅测试或未接线场景回退到 `IHftStraCtx`。

---

### 2. OTG 持有统一订单出口

文件：

- `OptionTradingGrid.h/.cpp`

新增生产 gateway seams：

- `SubmitQuoteFn`
- `SubmitOrderFn`
- `CancelOrderFn`
- `CancelAllFn`

动态创建 option/future OQM 时：

- paired quote 调用 `SubmitQuoteFn`
- buy/sell 单调用 `SubmitOrderFn`
- cancel/cancelAll 调用对应 cancel seam
- submit 返回的全部 localid 都通过 `OrderIssuedFn` 登记

### 3. HftOptionStrategy 统一出口

文件：

- `HftOptionStrategy.h/.cpp`

变更：

- 新增 `executeQuoteIds()`，返回 paired quote 的真实 bid/ask localid。
- `executeQuote()` 保留历史成功计数语义，内部转调 `executeQuoteIds()`。
- 新增 `executeCancel(uint32_t localid)`。
- 为 OTG 注入：
  - `executeQuoteIds`
  - `executeOrderIds`
  - 单订单 cancel
  - 合约 cancel-all
- Scanner combo 发单从直连 `_ctx->stra_buy/sell` 改为 `executeOrderIds()`。
- Scanner/manual cancel 统一走策略 cancel 出口。
- Gamma/manual/CTG 原有路径继续走策略 executor。

---

## 当前语义边界

### 已统一

以下生产路径的发单前检查和登记已统一：

- OQM paired quote
- OQM buy/sell 单
- Scanner combo
- Delta hedge
- Gamma hedge
- Manual order
- CTG executor

### 仍保留的限制

OQM 当前订单状态模型仍按“单次 side update 跟踪一个 localid”工作。

如果框架 action policy 将一笔委托拆成多个 localid，当前行为是：

- 所有 localid 都会进入 FillPriceChecker / OrderAnomalyGuard 登记；
- OQM 内部订单状态只跟踪首个 localid；
- 每单数量分配无法可靠建模，因为当前 `OrderIDs` API 只返回 ID，不返回每个 ID 的数量。

这避免在信息不足时错误重建订单簿。完整拆单状态跟踪需要框架提供更丰富的提交结果，例如：

```cpp
struct SubmittedOrder {
    uint32_t localId;
    uint32_t qty;
    bool isCloseToday;
};
```

该改造单独进行，不与本轮 seams 混合。

---

## 测试

新增：

- `test_otg_gateway_senders`

覆盖：

- buy/sell OQM 不走 paired quote sender；
- paired/buy-sell sender 注入正确；
- submit 返回的拆单 ID 全部登记；
- OQM cancel 走 gateway cancel；
- 动态创建的 OQM 使用 OTG gateway seams。

全量结果：

```text
7/7 tests passed
100% tests passed, 0 tests failed
```

---

## 尚未完成

本轮没有完成以下独立事项：

1. 将 seams 提取为正式 `OrderGateway` 类并彻底移除 OQM raw ctx fallback。
2. OQM 拆单 per-id 数量状态跟踪。
3. Scanner/Gamma/manual 的订单事件统一状态机。
4. 撤单失败、拒单分类和 combo top-up 多 ID 生命周期。
5. 跨午夜 TimeService。
6. 队列溢出 fail-safe 和 stale 行情 gate。

---

## 建议下一步

优先执行 **TimeService** 独立变更：

- 实盘 duration 使用单调时钟；
- 回测使用带交易日的逻辑时间；
- session/day rebase 时统一 reset EMA/TPS/TTL/panic/hedge/fitter；
- 增加 `23:59:59.900 → 00:00:00.100` replay 测试。
