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
| P1.4 日志开销 | ⏳ 待动 |
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
| Step 2a | `RiskCoordinator::checkTakerReduce` | 80 | 低 | ⏳ |
| Step 2b | `RiskCoordinator::checkRisk` | 250 | 中(风控中枢) | ⏳ |
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

### Step 2a - 抽 checkTakerReduce（⏳ 待做）
移入 `RiskCoordinator`（复核§3.2：错位归属）。依赖最少，最自洽。

### Step 2b - 抽 checkRisk（⏳ 待做，最高风险）
`_arb_executor`×10 全 null-guard -> RiskCoordinator 注入可空指针 + 前向声明/.cpp 包含。
**风控路径改动必须附回测证据；差异即回滚。**

## P1.4 日志开销
- 靶子：`WTSLogger` 调用 ~293 处（**非** `fmt::format` 18 处--复核已纠偏，fmt 不是优化对象）。
- 动作：热路径 debug 级日志加 level guard；先在 UFT 环境采 tick-to-trade 延迟基线，改完对比分布，无回归才合入。

## P2（季度级，需完整回归）
- **持仓同步收敛**：① `FutuPortfolio` 唯一写入口，收编所有 `onPositionUpdate`/`resyncPosition` 写入；② 读侧加 `PositionBook` 只读门面，新代码强制走门面。
- **头文件瘦身**：只移 4 个大头文件的非平凡实现到 `.cpp`。**禁用 Pimpl 于 `UnifiedOrderTracker`/`FutuPortfolio`**（tick 热路径，间接跳转+堆分配是负优化）；Pimpl 仅适合 Assembler/配置类冷路径。
- **锁优化**：`RecursiveSpinGuard` 原始提及 130 处。先在实盘采锁竞争/持有时长，确认热点再考虑分片/无锁化。无数据不动。

## P3（顺手）
- **include 清理**：跑 IWYU 拿真实清单（"~190"是估算），清理后挂 `clang-tidy misc-include-cleaner` 进 CI 防回潮。
- **基线度量**：改造前记录编译时间/`.so`体积/TestUnits通过率/回测tick延迟，每项改造后对比--无 before/after 的"优化"不算完成。

## 合规（与 AGENTS.md 对齐）
- 改动限 `src/WtFutuCore/` 内；涉及 `WtUftCore` 框架接口只记为外部限制，不改框架。
- PR 附 TestUnits 通过 + `dist/WtBtFutu` 回测 `outputs_bt/{trades,funds,positions,closes}.csv` 对比无回归。
- Conventional Commits：`refactor(StrategyCoordinator): ...`、`chore(arb): ...`、`test(clampReduce): ...`。
