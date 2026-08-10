# FUTU_CALLBACK_LOCK=0 切换评估报告 (C12 产出)

## 1. 背景

`FUTU_CALLBACK_LOCK` 编译开关 (默认 1=大锁, UftFutuMmStrategy.cpp:69-74):
- `1` = 全部回调 `_cb_mtx` 串行化 (保守, 生产基线)
- `0` = 细粒度模式, `_cb_mtx` 不再加, 依赖原子/结构锁

本报告评估切换到 0 的可行性。

## 2. 跨线程裸状态审计 (StrategyCoordinator)

| 状态 | 类型 | MdSpi (tick) | TdSpi (fill) | 保护方式 | 状态 |
|------|------|-------------|-------------|---------|------|
| `_last_mid` | `map<string, unique_ptr<MidSlot{atomic<double>}>>` | write (updateMarketData) | read (requoteAfterFill) | atomic store/load | ✅ C12 已修 |
| `_last_quote_params` | `map<string, CachedQuote>` | write (processQuoting) | read (requoteAfterFill) | `_last_quote_lock` (RecursiveSpinLock) | ✅ 已有锁 |
| `_last_requote_ms` | `map<string, uint64_t>` | - | read/write (requoteAfterFill) | TdSpi 单线程 | ✅ 无竞争 |
| `_last_taker_reduce` | `map<string, uint64_t>` | read/write (checkTakerReduce) | - | MdSpi 单线程 | ✅ 无竞争 |
| `_quote_chain` | `QuotePolicyChain` | read/write (processTick) | - | MdSpi 单线程 | ✅ 无竞争 |

## 3. Strategy 层原子状态 (UftFutuMmStrategy, v7.6 已修)

| 状态 | 保护方式 |
|------|---------|
| `_last_mid` | atomic MidSlot (v7.6, init 后 map 不可变) |
| `_channel_ready` | `atomic<bool>` |
| `_price_stale` | `atomic<bool>` |
| `_exchange_time_ms` | `atomic<uint64_t>` |
| `_order_error_count` | `atomic<uint32_t>` |
| `_quoting_paused_since` | `atomic<uint64_t>` |
| `_portfolio_ctx_dirty` | `atomic<bool>` |
| `TradingState` | 内部 atomic CAS (setQuotingPhase) |

## 4. 其他已有的细粒度锁

| 锁 | 保护对象 | 锁序 |
|----|---------|------|
| `FutuPortfolio::_lock` | ContractState / positions / prices | portfolio -> arb spin |
| `OrderApiGuard::orderApiMutex` | stra_buy/sell/cancel (框架容器) | 结构锁 -> orderApiMutex |
| `FutuQuoter::_lock` | per-quoter 报价状态 | quoter -> tracker |
| `UnifiedOrderTracker::_lock` | 订单跟踪 | 被 quoter/router 调用 |
| `_last_quote_lock` | _last_quote_params | 独立, 无嵌套 |

## 5. 结论

**_last_mid 是 StrategyCoordinator 中唯一的无保护跨线程裸状态。C12 已修复。**

切换 FUTU_CALLBACK_LOCK=0 的前置条件已满足:
- 所有跨线程共享状态已有原子或结构锁保护
- 锁序单向无环 (OrderApiGuard.h 已验证)

**剩余风险 (需实盘灰度验证):**
1. **复合操作原子性**: processTick 读 portfolio -> 决策 -> 下单, 若 TdSpi 并发改持仓, 决策基于旧数据. 可接受 (下一 tick 收敛), 是 HFT 标准做法.
2. **TradingState 多写者**: 6 个类 15 处写点 (C10 审计), 虽用 atomic CAS, 但复合转换 (reset/exitToQuoting) 是逐字段 store, 存在 ns 级窗口. C10 EventDispatcher 基础设施已就位, 未来收敛为单写者可消除.
3. **回测无影响**: 回测单线程, FUTU_CALLBACK_LOCK=0 时 `_cb_mtx` 本就无竞争 (~20ns).

**建议: 可开始实盘灰度验证 (先模拟盘, 后小仓位实盘).**
