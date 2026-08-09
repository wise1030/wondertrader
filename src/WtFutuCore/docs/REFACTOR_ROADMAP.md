# WtFutuCore 重构路线图

> 基于 v4.0 深度优化报告（OPTIMIZATION_REPORT.md）+ 复核结论修正。
> 核心原则：刚出过生产事故的模块，先做"降风险"的小步重构，不做大爆炸式改造。
> 每步可独立回滚、独立验证。

## 状态总览

| 优先级 | 项 | 状态 |
|--------|-----|------|
| P0.1 删 `_trash/` | ✅ 已完成 (commit `f71e2082`) |
| P0.2 `write_file_atomic`/`fastPow` | ✅ 复核确认在用，不删（原报告"作废"有误） |
| P0.3 `clampReduceQty` 统一减仓截断 | ✅ 已完成 (`RiskLiquidator.h:36`，3 路径调用) |
| P0.4 减仓路径对照测试 | ✅ 已完成 (`TestUnits/test_clamp_reduce_qty.cpp`) |
| P1.1 套利文件归入 `arb/` 子目录 | ✅ 已完成 (commit `7fda800f`) |
| P1.2 套利双轨合并 | ⚠️ 已修正：不存在双轨，保持现状（见下） |
| P1.3 StrategyCoordinator 增量拆分 | ⏳ 方案制定中 |
| P1.4 日志开销 | ✅ 已调查·前提不成立·跳过 |
| P2.1 持仓同步收敛 / P2.2 头文件瘦身 / P2.3 锁优化 | ⏳ 季度级 |
| P3.1 include 清理 / P3.2 基线度量 | ⏳ 顺手 |

---

## P1.2 修正：不存在"双轨"，保持现状

### 前提修正
原路线图把 `ArbExecutionBridge`（同步）与 `AsyncArbitrageExecutor`（异步）当作"两条可合并的并行轨道"。
核查代码后该前提**不成立**：两者是**生产者-消费者两层**，非双轨。

| 组件 | 角色 | 线程 | 职责 |
|------|------|------|------|
| `AsyncArbitrageExecutor` (682行) | 生产者：信号引擎 | arb 线程 | 收tick/价差计算/生成信号/自成交检查/产出 `ArbOrderRequest` |
| `ArbExecutionBridge` (478行) | 消费者：主线程编排 | 主线程 | 喂tick/拉订单执行/残腿对冲/in_flight清理/同价去重 |

**证据**：Bridge 紧密调用 async 引擎 6 处（`ArbExecutionBridge.cpp`）：
`pushTick:46` / `updateMMOrders:84` / `processPendingOrders:92` /
`tagOrderPair:226` / `processOrphanLegs:277` / `consumePairTag:400`。

**"同步/异步"是 Async 内部开关**，不在两者之间：
```
use_async_arb_thread = true  // 实盘：启独立 arb 线程，pushTick 走 SPSC 队列(~50ns)
use_async_arb_thread = false // 回测：不启线程，pushTick 主线程同步执行
```
两种模式用**同一套** Async + Bridge，只是派发方式不同（AGENTS.md 要求回测 `useAsyncArbThread: false`）。
`in_flight` 状态跨 Async/Manager/Bridge 三类分治，是紧耦合横切状态，非可独立重复。

### 结论
- **不合并**。合并只会造 god-class、混主/arb 线程域、危及已硬化的 B-fix 链（B3/B5/B7/B13），零功能收益。
- 原路线图 P1.2"合并双轨"项**删除**，改为"维持现状 + 视产品路线决定是否投入新套利功能"。
- "套利是否在产品路线"与合并**脱钩**：只决定是否做新套利功能（新策略/调优/残腿状态机 Phase D1），不决定合并。

---

## P1.3 StrategyCoordinator 增量拆分

按依赖方向增量抽取，不一步到位。coordinator ~2000 行/god-object(~17 依赖)，
可抽 ~452 行(23%)，收敛为"调度 + 报价引擎"。

| 步骤 | 抽出 | 行 | 风险 | 状态 |
|------|------|----|------|------|
| Step 1 | `CloseoutTrigger` (processCloseout 触发+状态机) | 122 | 低 | ✅ 已完成 |
| Step 2a | `RiskCoordinator::checkTakerReduce` | 80 | 低 | ✅ 已完成 |
| Step 2b | `RiskCoordinator::checkRisk` | 250 | 中(风控中枢) | ✅ 已完成 |
| Step 3 | `processQuoting`/`requoteAfterFill` | 335 | - | 不动(热路径) |

**修正后的依赖清单**（复核纠偏）：
- Step 1 CloseoutTrigger: `risk_monitor`×11、`cfg`×8、**`_quoter`×8**(4 处 cancelAll, 改 `cancel_all_quotes` 回调保持单向依赖)、`portfolio`×3(仅 null 闸)、`trading_state`×1
- Step 2a checkTakerReduce: `order_router`×2、`risk_monitor`、`portfolio`、ctx（最自洽，先抽）
- Step 2b checkRisk: **`_arb_executor`×10**(null-guard 模式, 直接照抄)、`self_trade_calibrator`、`risk_monitor`、`trading_state`、`portfolio`、`quoters`、`cfg`。**注：原方案多列的 `_liquidator`/`_toxicity` 不在 checkRisk 内**
- `processAutoCancel`(1314-1344, 31 行)：留 coordinator

**决策门已解除**：套利保持编译 + `arb/` 子目录隔离 + `_arb_executor` 全 null-guard
(`if(_arb_executor)`, 配置启用才构造)。故 1->2a->2b 可连续推进，无需等待。
将来若补 `WT_BUILD_ARB`，只需改 CMake glob + 一处工厂，成本低（已决策、可回退）。

### Step 1 - CloseoutTrigger（✅ 已完成）
- 新增 `CloseoutTrigger.{h,cpp}`：`process()` 返回是否触发 closeout。
- `_quoters` 循环 cancelAll(4 处) -> `cancel_all_quotes` 回调（保持对 FutuQuoter 单向依赖）。
- `processTick` 调用点改 `_closeout_trigger.process()`；依赖在 `setTradingState`(最后 setter) 注入。
- 与 `CloseoutOrchestrator`(执行驱动, C3) 互补：本类只管触发/状态。
- 验证：libWtFutuCore.so 构建通过；TestUnits 20/22(2 失败为框架层环境依赖，无关)。

### Step 2a - 抽 checkTakerReduce（✅ 已完成）
移入 `RiskCoordinator`（复核§3.2：错位归属）。依赖最少，最自洽。
- 新增 `RiskCoordinator.{h,cpp}`：`checkTakerReduce(ctx, exchange_time_ms)`
- `_last_taker_reduce` 限频状态迁入；`_last_exchange_time_ms` 留 coordinator 作参数传入
- 2 调用点改 `_risk_coord.checkTakerReduce(ctx, _last_exchange_time_ms)`；依赖在 setTradingState 注入
- 验证：构建通过；TestUnits 20/22（2 失败为框架层环境依赖，无关）

### Step 2b - 抽 checkRisk（✅ 已完成）
- in_cooloff 参数化消 `_toxicity`；`_quote_chain*` 保留(3 riskWiden 写,共享)；`_quoters` cancelAll(2处)->回调
- `_liquidator`/`_last_halt_log_ms`/`_violations_buf` 状态迁入；2 调用点改 `_risk_coord.checkRisk(ctx,tc,in_cooloff)`
- `_arb_executor` 15 处 null-guard 照抄; setTradingState 扩展注入(9 字段)
- **验证(D+ 噪声地板标定)**: 改前同二进制 2 次回测 -> 29 风控事件全稳定(零 srand 噪声); 2b 后 2 次回测 29 事件全稳定, 且与改前基线**逐行 IDENTICAL**(零行为变化); TestUnits 20/22(2 既有环境失败)
- 局限: HALT/FORCE_FLAT/TAKER_REDUCE 分支本场景未触发, D+ 未覆盖(纯搬运纪律 + 编译器保证)
- **路 F（决策逻辑纯函数化）已决：defer/close** — 2b 搬运已 D+ 验证零行为变化，F 对搬运无加成；F 价值在未来风控逻辑改动时锁定决策表，届时随那次改动一起做。

## P1.4 日志开销（已调查·前提不成立·跳过）

**结论：不值得做。** 调查揭示前提（"debug 调用付 fmt::format 代价，需 level guard"）不成立。

- **WTSLogger 已内置 level 短路**：`debug/info/warn/error` 模板先 `if (m_logLevel > LL_xxx) return;` 再 `fmtutil::format_to`。prod（debug 关）下 debug 调用零 format 代价。复核"靶子是 WTSLogger 调用点"方向对，但假设它不短路——错了（同 `_liquidator`/`_toxicity`/双轨合并类前提错误）。
- **残留开销 = 参数求值**：按函数调用语义，实参在 level 检查**之前**求值，短路只省 format 不省求值。但热路径 debug 仅 4-5 处，参数都是结构体字段/局部变量（非昂贵函数调用），≈ 5 × (字段读 + 预测分支) ≈ **~10ns/tick**，对 μs 预算 UFT 可忽略。
- **无合规手段加 guard**：`m_logLevel` private、无 public level-check API；加 call-site guard 须改 `WTSLogger.h`（框架文件，AGENTS.md 禁越界）。
- 热路径 info 调用均事件驱动（config/closeout/section_break/risk_normalized/perf 周期），非每 tick。

**裁决**：框架已优化，热路径日志少且参数廉价，残留 ~10ns/tick 可忽略，guard 冗余或越界。跳过。

## P2（已调研·大部分不需动）

### P2.1 持仓同步收敛 — ✅ 已完成
`FutuPortfolio` 已是唯一写入口（SSOT 已达成）：外部写入仅 5 处且全部走其 API（`UftFutuMmStrategy.cpp:702` onPositionUpdate、`FutuRuntimeOps.cpp:80/92/383` onTradeFill/resyncPosition/updatePosition、`CloseoutOrchestrator.cpp:146` onPositionUpdate），position 字段直写 13 处全在 `FutuPortfolio.cpp` 内部。原"36 处"为虚高（多含读取/内部实现）。
**附注（已知第二写入口）**：`FutuRuntimeOps.cpp:97 _arb_bridge.onTradeFill` — 套利执行桥自维护第二本持仓账（设计使然，非违规），登记以免下次审计当新发现重查。读侧 `PositionBook` 门面为可选项，非紧迫。

### P2.2 头文件瘦身 — 可选·仅冷路径·低优先
4 个大头文件：`UnifiedOrderTracker`(767)/`SignalAggregator`(761)/`FutuPortfolio`(619)/`FutuRiskMonitor`(584)。
`UnifiedOrderTracker`/`FutuPortfolio` 在 tick 热路径，内联实现移 .cpp = out-of-line 调用 = UFT 负优化（与 Pimpl 同理），**只能移冷路径实现**（resync/validate/resetSession 类）。
若做：与下次涉及 checkRisk 行为的改动共用一次构建+回测验证窗口，不为它单独付验证成本。

### P2.3 锁优化 — ⏸ 受阻·解阻条件明确
`RecursiveSpinGuard` 用量：`UnifiedOrderTracker`(42)/`FutuPortfolio`(34)/`FutuQuoter`(22)/`OrderRouter`(17)。全模块无锁竞争/持有时长测量设施；竞争发生在实盘多线程（MdSpi/TdSpi/arb），回测单线程测不到。
**解阻第一步是测量（非优化）**：`RecursiveSpinGuard` 加编译开关持有时长统计（`#ifdef WT_LOCK_PROF`），或实盘 perf 采样一次。作为 backlog 登记，不排期。"无数据不动"。

## P3（顺手）
- **include 清理**：跑 IWYU 拿真实清单（"~190"是估算），清理后挂 `clang-tidy misc-include-cleaner` 进 CI 防回潮。
- **基线度量**：改造前记录编译时间/`.so`体积/TestUnits通过率/回测tick延迟，每项改造后对比--无 before/after 的"优化"不算完成。

## 合规（与 AGENTS.md 对齐）
- 改动限 `src/WtFutuCore/` 内；涉及 `WtUftCore` 框架接口只记为外部限制，不改框架。
- PR 附 TestUnits 通过 + `dist/WtBtFutu` 回测 `outputs_bt/{trades,funds,positions,closes}.csv` 对比无回归。
- Conventional Commits：`refactor(StrategyCoordinator): ...`、`chore(arb): ...`、`test(clampReduce): ...`。
