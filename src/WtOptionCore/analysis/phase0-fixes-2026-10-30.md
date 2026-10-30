# WtOptionCore 阶段 0 修复执行记录

- **日期**: 2026-10-30
- **执行范围**: `code-review-and-optimization-plan-2026-10-30.md` 的阶段 0 第一批小步修复
- **代码基线**: 从 HEAD `d6825448` 的工作区开始
- **验证**: 主构建 `src/build_all` 编译通过；WtOptionCore 7/7 CTest 通过

---

## 已完成修复

### 1. 异步事件 exactly-once 与可靠控制唤醒

文件：

- `OptionAsyncEventProcessor.h`
- `OptionAsyncEventProcessor.cpp`

变更：

- trader 队列增加与 `_trader_mtx` 同步更新的原子 `_trader_count`。
- worker CV 谓词不再无锁读取 `_trader_queue.empty()`。
- 批量达到 1024 时只回插未消费 trader 后缀，不再重复已处理事件。
- trader 入队失败不再增加 totalEvents。
- 新增可靠 `enqueue_control()`，用于热参数 STOP/PANIC 在安静市场唤醒 worker。

回归测试：

- 新增 `tests/test_async_event_processor.cpp`
- `test_async_event_processor` 通过

---

### 2. STOP 真正撤销报价

文件：

- `HftOptionStrategy.h`
- `HftOptionStrategy.cpp`

变更：

- `on_params_updated()` 投递可靠 control event。
- `setState(Disabled, "hotcmd_stop")` 在 enabled 置 false 后执行 `CTG.refresh()` 和 `drainPendingQuotes()`，确保旧 desired market 转成撤单并发出。

说明：

- session_end/channel_lost 保持各自原有撤单路径，避免重复撤单。
- 统一“所有 Disabled 都自动撤单”的进一步状态机整理仍留在后续阶段。

---

### 3. 订单真实 localid 与在途对冲

文件：

- `OptionHedgeCoordinator.h/.cpp`
- `HftOptionStrategy.h/.cpp`
- `tests/test_hedge_coordinator.cpp`

变更：

- `OptionHedgeCoordinator::SendFn` 改为返回 `std::vector<uint32_t>`。
- `HftOptionStrategy::executeOrderIds()` 返回框架生成的全部 localid。
- `executeOrder()` 保持历史“成功笔数”语义，供 CTG/manual 使用。
- 对冲路径改用 `executeOrderIds()`，不再把成功标志 `1` 当 localid。
- 拆单产生的多个 localid 全部登记到 `OrderAnomalyGuard` / `FillPriceChecker`。

---

### 4. OQM 生产发单登记闭环

文件：

- `OptionQuoteManager.h/.cpp`
- `OptionTradingGrid.h/.cpp`
- `HftOptionStrategy.cpp`

变更：

- OQM 增加 `OrderIssuedFn`，每次 `trackOrder()` 都回调真实 localid。
- OTG 将 option/future OQM 的 localid 转发给策略。
- 策略统一调用 `registerIssuedOrderIds()`，使主 OQM 直连 `IHftStraCtx` 的订单也进入异常单和成交价偏离监控。

说明：

- 这解决“OQM 正常成交被判 Unknown 后跳过成交更新”的核心状态问题。
- OQM/scanner 的下单前统一价格限幅和完整 `OrderGateway` 仍未完成，属于下一阶段。

---

### 5. 动态 OQM wiring 与交易对象生命周期

文件：

- `OptionTradingGrid.h/.cpp`
- `UnderlyingTradingData.h/.cpp`
- `HftOptionStrategy.cpp`

变更：

- OTG 增加动态依赖注入：
  - option/future RiskFilterChain
  - PositionGuard lookup
  - PositionOffsetMgr lookup
  - OrderIssuedFn
- 动态创建 option/future OQM 时同步注入以上依赖。
- 为 per-expiry hedge code 预创建 Guard/OffsetMgr。
- 新增 `m_tblOptionTradingData` 强引用表，修复 OTD 在 `onAddOption()` 返回后因 OptionData 弱引用而立即析构的问题。
- UTD 对 OTG 的反向引用改为 weak_ptr，破除 OTG↔UTD shared_ptr 环。
- `onAddExpiry()` 直接使用回调传入的 `ExpiryData` 创建 ETD，不再依赖 grid 中已存在同名 expiry。

回归测试：

- `test_dynamic_otg_wiring` 验证动态 option/future OQM 均拿到 PositionGuard。
- 全量测试通过。

---

### 6. 昨仓、今仓与 RiskData 同步

文件：

- `HftOptionStrategy.cpp`
- `PositionOffsetMgr.cpp`
- `tests/test_enhancements.cpp`

变更：

- broker position 总仓改为 `prevol + newvol`。
- PositionOffsetMgr 保留 today/prev 可平量拆分，不再把全部可用量错误归入 today。
- broker position 回调同步：
  - `_positions`
  - OptionRiskData
  - hedge position
  - PositionGuard
  - PositionOffsetMgr
- future UTD OQM 也会从 `_positions` 同步仓位。

说明：

- long/short 双快照覆盖到单边净值的完整 PositionBook 仍在下一阶段实现。

---

### 7. 合约静态事实注入

文件：

- `OptionTradingGrid.cpp`
- `HftOptionStrategy.cpp`
- `PnlTracker.h`
- `OptionRiskData.h`（使用既有 setter）

变更：

- 从具体期权代码通过 `CodeHelper::extractStdCode()` 推导真实期权品种。
- 不再依赖 grid 层默认产品字符串获取 tick/multiplier/fee，覆盖 CZCE call/put 双品种。
- 有交易所 commInfo 时始终用真实 PriceTick/VolScale 覆盖 OptionData。
- multiplier 注入后同步：
  - OptionRiskData::setContractSize
  - PnlTracker::setMultiplier
- 按 per-lot fee 折算 PnLTracker feePct。
- `OptionGrid` 产品参数改为传入 `_optionProduct` 而不是 futures product。

---

### 8. primary hedge multiplier 与共享 hedge 去重

文件：

- `HftOptionStrategy.cpp`
- `OptionRisk.cpp`

变更：

- expiry 配置解析时用 hedgeCode 对应 commInfo 注册 primary hedge multiplier。
- `OptionRisk::update()` 组合层按实际 hedge instrument 去重，不再把同一期货在多个 expiry 下重复累计。

说明：

- per-expiry Greeks 中共享 hedge 的展示口径仍可进一步优化，但组合 delta 已不再重复累计。

---

### 9. future OQM 回报路由

文件：

- `HftOptionStrategy.cpp`

变更：

- `on_trade` / `on_order` 在找不到 option OTD 时按 code 路由到 UTD OQM。
- broker position batch sync 也同步 UTD OQM position。

---

### 10. 到期状态

文件：

- `ExpiryData.h/.cpp`
- `tests/test_enhancements.cpp`

变更：

- 到期日后 DTE 变为负数。
- settlement/premium fade fraction 在过期后为 0。
- 保留到期日当天 intraday fraction 语义。
- 增加负 DTE 回归断言。

说明：

- 完整的 `ACTIVE/EXPIRING/EXPIRED/SETTLED` 状态机、结算和移仓仍未完成。

---

### 11. Fitter 单点/区间外 UB

文件：

- `PeriodicCurveFitter.cpp`

变更：

- 移除不安全的 `find_range()` 双迭代器插值。
- 新增明确处理空、单点、精确命中、区间外和正常 bracket 的 `interpolate_value()`。
- 单点或 forward 落在全部 strike 外时不再解引用 `end()`。

说明：

- 当前全量测试通过；仍建议后续补 ASan/UBSan job 覆盖拟合边界。

---

### 12. PnL 跨 session 重置

文件：

- `PnlTracker.h/.cpp`
- `HftOptionStrategy.cpp`
- `tests/test_enhancements.cpp`

变更：

- 新增 `PnlTracker::resetSession()`，重置 realized、turnover、fill count、FIFO、weighted entry。
- session begin 时重置所有已存在 option/underlying tracker。
- 重新将 `_positions` 中的持仓加入 `_pnlPendingInit`，等待新 session preClose 成本基准。
- 增加 session reset 回归测试。

---

### 13. 多实例诊断计数和 Release 断言

文件：

- `HftOptionStrategy.h/.cpp`
- `tests/test_ctg_ranking.cpp`

变更：

- `batchCount` 从函数级 `static int` 改为实例成员，避免多策略实例数据竞争和长时间运行 signed overflow。
- `test_ctg_ranking` 的标准 `assert()` 全部替换为不受 `NDEBUG` 影响的 `ASSERT_TRUE`。

---

## 当前验证结果

构建命令：

```bash
cmake --build src/build_all \
  --target WtOptionCore \
  test_ctg_ranking test_enhancements test_scanners test_stress \
  test_hedge_coordinator test_command_handlers test_async_event_processor \
  -j4
```

测试命令：

```bash
ctest --test-dir src/build_all/WtOptionCore --output-on-failure
```

结果：

```text
7/7 Test #1..#7 Passed
100% tests passed, 0 tests failed out of 7
```

说明：

- 模块独立目录 `src/WtOptionCore/build` 缺少 WTSTools/Share 链接路径，不能作为完整验证依据。
- 本轮以仓库主构建目录 `src/build_all` 为准。

---

## 尚未完成、保留到下一阶段

以下事项不应被误解为已经完成：

1. 统一 `OrderGateway`：OQM/Scanner/Gamma/manual 的下单前出口仍分散。
2. OQM/scanner 直连 `IHftStraCtx` 的下单前价格限幅统一。
3. 跨午夜单调时钟系统重构。
4. 队列溢出后的自动 Disabled/Panic 与行情 freshness gate。
5. Combo top-up 多 localid 跟踪、残差 unwind、重启工作单认领。
6. 完整 PositionBook 与 long/short 双快照净值模型。
7. stale tick timestamp 全链路保留与 cancel-only。
8. ConfigLoader typed schema 与 effective-value dump。
9. worker 减负、dirty-set 调度、FAST 增量化和延迟基准。
10. Scanner/Gamma/Power 曲线功能验收。

---

## 建议下一步

按原方案进入两个独立小变更：

1. **OrderGateway 变更**：统一 OQM/Scanner/Gamma/manual 下单出口，补 wiring 测试。
2. **TimeService 变更**：统一单调时钟、跨午夜 reset、独立 timer，补 23:59:59.900→00:00:00.100 replay 测试。

不要与本轮阶段 0 修复混在同一个提交中。
