# WtFutuCore 深度分析 V7

> **分析时点**: 2026-07-21，V6 修复（15 项）全部完成并编译通过后
> **分析维度**: 架构 / 业务逻辑 / 代码质量 / 性能
> **方法论**: 4 个并行探索 agent 全量扫描 90 文件/3 万行 + 人工核实 6 个关键严重项
> **编号约定**: A=架构 B=业务逻辑 C=代码质量 P=性能；标注 `[新]`/`[V6已修]`/`[V6未修]`/`[V6记录]`

## 现状总评

V6 修复后系统整体成熟度**较高**：SPSC + spinlock + atomic 三层并发模型清晰；关键 data race（A2 PnL 快照）已修；套利/信号/策略三处的插件化扩展性（注册表 + 接口）是亮点；WTSLogger 惰性求值、RingBuffer、_violations_buf 复用等延迟优化已到位。

本次扫描发现 **44 个新问题**（严重 14 / 中等 21 / 轻微 9），其中：
- **资金安全类 5 个**（B 段，须立即修）：STP 误拒、closeout 卡死、VaR 符号错、隔夜 PnL 重计、channel_lost 不停 arb
- **数学错误 3 个**（C 段）：VaR、Sharpe 年化、net_exposure 符号
- **性能可压缩 ~1.5–2μs/tick**（P 段，主路径从 ~8μs 逼近 6μs）
- **死代码模块 1 个**（C 段，MarketMakingEnhancer 全模块未接线）

---

## 复核与修复状态（2026-07-21）

已对 P0 全部 11 项逐一人工复核 + 修复 + 编译验证（`make WtUftRunner` 通过）：

| ID | 复核结论 | 状态 |
|----|----------|------|
| B1 STP 不过滤 pending_cancel | 真实（FutuQuoter 大量 markPendingCancel，checkSelfTrade/getConflictingMMOrders 未过滤） | **已修复** |
| B2 getActiveCountBySource 不过滤 pending_cancel | 真实（与 totalActiveOrders/getActiveOrders 口径不一致） | **已修复** |
| B3 撤单无超时 | 真实（checkAutoCancel 跳过 pending_cancel，无清扫） | **已修复**（加 5s 超时强制 untrack） |
| B4 closeout 期间不暂停 ARB | 真实（onTick 仅查 use_spread_arbitrage） | **已修复**（isCloseoutTriggered 门控） |
| B5 channel_lost 不停 arb | 真实（无 setEnabled(false)） | **已修复**（新增 setEnabled + lost/ready 联动） |
| B6 arb 线程无 try/catch | 真实（arbThreadFunc 顶层无兜底） | **已修复**（异常禁用套利保线程存活） |
| B7 HALT 期间误增 error_count | **误报**（2085 行已有 RISK_HALTED 早退守卫） | 无需修复 |
| B8 net_exposure 符号错 | 真实（全对冲价差算成 2 倍敞口） | **已修复**（`-` → `+`） |
| B9 resetDailyPnl 不重置 avg_cost | 真实（昨日成本混入今日 daily_pnl） | **已修复**（重置 avg_cost=0 触发 pre_close 重设） |
| C3 Sharpe 年化错误 | 真实（per-trade 用 sqrt(250)） | **已修复**（sqrt(250×日均笔数)） |
| C4 inventory_pnl 恒 0 | 真实（realized_pnl 从未写入；无消费方） | **已修复**（recordTrade 按合约累计） |

**净结果：10 项真实已修复，1 项（B7）误报。** 编译 `WtFutuCore` + `WtUftRunner` 均通过。

---

## P1 复核与修复状态（2026-07-21）

已对 P1 全部项逐一人工复核。复核发现 report 有 3 处**误判/高估**，避免了有害改动：

| ID | 复核结论 | 状态 |
|----|----------|------|
| C2 PerformanceAnalyzer pending_adverse 泄漏 | 真实（只减同合约 ticks_remaining + 只清连续 front 已评估项 → 低流动性合约泄漏） | **已修复**（30s 墙钟超时 + remove_if 全清理） |
| B12 cancelByPair 防御性补撤死代码 | 真实（`orders_it==end` 条件因桶预创建恒 false） | **已修复**（改 `if(!sent)`） |
| B13 cancelOrder 不打 pending_cancel | 真实（无调用方，但为一致性补标记） | **已修复** |
| C6 beta 截断 [0.7,1.5] 硬编码 | 真实（CorrelationManager 跨品种 expectedBeta<0.7 被错误截断） | **已修复**（config 加 beta_min/max + 按 expectedBeta 设带宽） |
| A1 on_trade/channel_ready 漏清 _risk_spread_mult | 真实（2 处恢复路径绕过 coordinator 自动恢复） | **已修复**（新增 `onExternalResumeFromRisk()` + 2 处调用） |
| B16 VaR 缺乘数 | **误判**：`spread_type` 默认且配置均为 WEIGHTED（含乘数 m1/m2），spread_std 已含乘数，公式当前正确；仅 SIMPLE_DIFF 才缺乘数。盲目补乘数会让 WEIGHTED 高估 300 倍 | **不修改**（配置相关潜在项，标注） |
| C1 MarketMakingEnhancer 整模块死代码 | **误判**：是**有意观测模式**（StrategyCoordinator.cpp:1076-1079 注释明确推迟到 C2 阶段，避免未验证行为变更），且已被 `\|agg_z\|>0.1` 门控。接线=未验证行为变更、删除=丢弃分阶段工作，均不对 | **不修改**（保持现状） |
| P1 TickContext.code 堆分配 | **高估**：合约码如 `SHFE.ag.ag2608` 仅 14 字符 ≤15 命中 SSO，无堆分配；且 43 处用作 map key，改 const char* 反而引入临时构造 | **跳过** |
| P3 checkAutoCancel vector 分配 | 真实（每 tick 3 次无容量 vector 堆分配） | **已修复**（成员缓冲 _actions_buf/_active_indices_buf/_stale_buf + 返回 const ref） |
| P4 getPairsForContract 返回 vector 按值 | 真实（6 处调用点按值拷贝） | **已修复**（返回 const ref + 调用点 const auto&） |
| P5 spinlock 未 cacheline 对齐 | 真实（5 处 std::atomic_flag 未 alignas(64)） | **已修复**（alignas(64) + 64B 填充） |

**净结果：8 项真实已修复，3 项（B16/C1/P1）误判或有意设计不修改，P2(ArbTickData.code) 同 P1 原理 SSO 覆盖顺带评估。**
编译 `WtFutuCore` + `WtUftRunner` 均通过。

**A1 范围说明**：本次修复了已确认的具体危害（2 处恢复路径漏清 `_risk_spread_mult`）。
完整的 4 状态 SSOT 统一（TradingStateGuard）是大型架构重构，涉多模块状态语义，留 P3 长期项单独规划。

---

## P2/P3 复核与处置（2026-07-21）

延续"先验证再改"原则。性能优化项落地，架构重构项给出负责任处置（避免对线上 HFT 系统盲改引入回归）。

### 已修复（性能，编译 `WtUftRunner` 通过）

| ID | 复核结论 | 修复 |
|----|----------|------|
| **P6** correlation+beta 重复 std::log | 真实：两方法各 2/1 次扫描 + 每元素 6 次 log，仅被 updateStatistics 调用（无外部调用方） | **已修复**：新增 `computeCorrelationAndBeta()` 单次扫描 log-return 入复用缓冲，同算两者，数学公式逐一核对一致 |
| **P8** generateSignal 多次 spinlock | 真实：**实际 4 次** `_pair_states_spin` 获取（report 说 3 次） | **已修复**：合并前 3 次只读锁为 1 次（cooldown 早退保留在锁内避免无谓 state 拷贝），写锁独立 → **4→2 锁**。已确认 canOpenPosition 不重复取锁防死锁 |
| **P7** executeSignal 自检 O(n) 扫描 | 真实：锁内 O(n) vector 拷贝 + O(n) 线性扫描。数学推导：自检约束边界即全局 min_sell/max_buy（与按 arb_price 过滤等价） | **已修复**：`_mm_buy/sell_orders`(vector) → `_mm_max_buy/_mm_min_sell`(标量)，updateMMOrders 预计算，自检 O(1)。已确认该 vector 仅用于自检 |

### 评估后推迟（性价比低 / 高风险 / 冗余）

| ID | 结论 |
|----|------|
| **P9** ContractState hot/cold | 系统仅 1-3 合约，聚合计算已在连续 vector 内（tick 级访问早已入缓存），ROI 极低；ContractState ~30 字段跨多文件，侵入性高。**推迟** |
| **P10** code_id intern 表 | 组件指针**已在 processTick 入口一次性解析**（注释明示"消除重复字符串哈希查找"），后续 21 处复用。额外收益仅 4 次查找/tick（~120ns），改 4 个 map 结构风险不成比例。**推迟** |
| **B14** orphan leg 落盘 journal | 启动时 `on_channel_ready` 已与券商对账持仓（崩溃裸腿体现为实际持仓被识别管理），journal 冗余；且热路径加磁盘 I/O 与低延迟目标冲突。**推迟**（如需更强保障，建议启动对账时显式比对 arb pair 净敞口而非加 journal） |

### 大型架构重构（A2/A4/A1-atomic/ARB-D1）— 给方案不盲改

这些是多模块、跨数周的重构，无测试设施可即时验证回归，报告本身标注"中期/长期"。对线上 HFT 系统一次性盲改不负责任。

| ID | 评估 | 处置 |
|----|------|------|
| **A1-atomic** TradingState 全 atomic | **不需要**：TradingState.h:14-28 已文档化单写者契约 + DEBUG 线程断言，实际仅主线程访问。改 atomic 增加开销解决不存在的问题 | 保持现状 |
| **A2** SpreadArbitrageManager 拆分 | 1937 行 God Object，拆 4 子模块涉及全部调用方 | 给出分阶段方案（见下），需配合回测验证，不盲改 |
| **A4** 风控 IRule 链 | 改核心风控求值路径 | 同上 |
| **ARB Phase D1** 完整 arb 状态机 | ARB_SELF_CLOSE_DESIGN.md 已规划的重大特性（非 bug） | 属产品路线图，需完整设计+实现+回测，不在本轮修复范畴 |

### A2 分阶段拆分建议（供后续按阶段执行）

1. **抽 `ArbFlightTracker`**：`_pair_arb_states` + `_active_close_intents` + `_overshoot_*`（in_flight/intent/overshoot 三合一，对接 Phase D1）。风险最低，接口最内聚。
2. **抽 `SpreadPairRegistry`**：pair 配置 + `_calculator_manager` 编排。纯数据访问，易测。
3. **抽 `SignalGenerator`**：策略调度 + generateSignal。依赖 1、2。
4. 每步跑 `make WtUftRunner` + 回测对比信号序列一致性。

---

## A. 架构分析

### A1【严重·V6未修】4 套交易状态并行，SSOT 严重违规

**位置**: `TradingState.h`、`FutuRiskMonitor.h:571-605`、`UftFutuMmStrategy.h:432`、`StrategyCoordinator.h:290`

同一逻辑状态（暂停/方向封锁/软倍数）分散在 4 处独立存储：
| 状态 | 主存储 | 冗余副本 1 | 冗余副本 2 |
|------|--------|-----------|-----------|
| 暂停 | `TradingState.qphase`（非原子） | `FutuRiskMonitor._quoting_paused`（atomic） | — |
| 方向封锁 | `TradingState.long/short_blocked` | `FutuRiskMonitor._long/short_blocked`（atomic） | `UftFutuMmStrategy._blocked_contracts`（unordered_map） |
| 软风控倍数 | `StrategyCoordinator._risk_spread_mult`（double） | — | — |

**已确认的遗漏**: `UftFutuMmStrategy::on_trade:1653-1659` 恢复时漏清 `_risk_spread_mult` → 风控暂停恢复后报价宽度被永久放大 ×1.5/×2.0。

**修复方案**:
```cpp
// 引入统一守卫，所有恢复点强制调用
class TradingStateGuard {
    TradingState& _ts; FutuRiskMonitor& _rm;
    double& _spread_mult;
    std::unordered_map<std::string,bool>& _blocked;
public:
    void resumeAll();    // 原子清所有标志（含 spread_mult=1.0、unblock、resumeQuoting）
    void enterRiskHalted(RiskCategory cat, double pnl);
};
```
替换 4 处手动同步点：`StrategyCoordinator.cpp:919-948`、`UftFutuMmStrategy.cpp:1653-1659,1840-1855`。

---

### A2【中等·新】SpreadArbitrageManager God Object（35 个 public 方法）

**位置**: `SpreadArbitrageManager.h:118-473`

一个类承担 7 个独立关注点：pair 配置、SpreadCalculator 编排、信号生成、B-3 门控、B1 intent 通道、B5 overshoot 冷却、多 pair 聚合查询。测试困难，认知负担高。

**修复**: 拆分为 `SpreadPairRegistry` + `SignalGenerator` + `ArbFlightTracker`（in_flight/intent/overshoot 三合一状态机，对接 ARB_SELF_CLOSE_DESIGN.md 的 Phase D1）。

---

### A3【中等·新】三层 SpreadState 副本链，arb 线程仓位滞后 1 tick

**数据流**: `Portfolio._contracts`（SSOT）→ `SpreadArbitrageManager._pair_states`（每 tick 复制）→ `SpreadRiskManager._pair_states`（再复制）。arb 周期 5ms，止损决策读到的是 1 tick 前的仓位。

**修复**: 改 `std::shared_ptr<const SpreadState>` 原子替换（RCU 风格），消除复制。

---

### A4【中等·新】风控规则硬编码，扩展性差

**位置**: `FutuRiskMonitor.cpp:108+`

`checkRiskLimits` 硬编码 4 种 `RiskLimitType`，加规则需改 enum + checkRiskLimits + determineActionWithCategory 三处。

**修复**: 改 `std::vector<std::unique_ptr<IRiskRule>>` 链式调用，每规则返回 `RiskViolation` 或 pass。预留 `addCustomRule()` 接口。

---

### A5【轻微·新】Coordinator 11+ 个 setter 注入易遗漏

**位置**: `StrategyCoordinator.h:185-210`

**修复**: 改 `CoordinatorDeps` 结构体（仿 `ArbExecutionBridge::Deps`），编译期完整校验。

---

## B. 业务逻辑分析（资金安全优先）

### B1【严重·新】STP 检查不排除 pending_cancel 状态的 MM 单 → 高频刷新窗口 ARB 信号被误拒

**位置**: `UnifiedOrderTracker.cpp:456-485`

`checkSelfTrade` 循环仅检查 `isActive()`，**不检查 `isPendingCancel()`**：
```cpp
for (uint32_t orderId : it->second) {
    const UnifiedOrderInfo* order = getOrderByOrderId(orderId);
    if (!order || !order->isActive()) continue;   // ← 缺 !isPendingCancel()
    ...
}
```
而 `OrderRouter.cpp:210-212` 注释明示 `cancelAllBySource` 设 `pending_cancel` 正是为了"防自成交检查在 cancel-ack 窗口内当活跃单"——**注释意图与实现矛盾**。

`FutuQuoter` 路径 B1/B2 先 `stra_cancel`+`markPendingCancel` 再下新单（`FutuQuoter.cpp:210-219,283-301`），ack 窗口 1–100ms 内旧单仍 `isActive()`。高频刷新时 ARB 买单被旧 MM 卖价误拒 → `ArbExecutionBridge.cpp:190-202` 还会撤对侧腿+标记残腿，副作用扩大。

**修复**:
```cpp
if (!order || !order->isActive() || order->isPendingCancel()) continue;
```
与 `getPendingBuyQty`/`getPendingSellQty`（`UnifiedOrderTracker.cpp:293-335`）过滤口径一致。

---

### B2【严重·新】`getActiveCountBySource` 不过滤 pending_cancel → closeout inflight guard 卡死

**位置**: `OrderRouter.h:200-205` vs `OrderRouter.cpp:320-324,336-349`

```cpp
uint32_t getActiveCountBySource(Source src) const {
    auto it = _active_orders.find(static_cast<int>(src));
    return it == _active_orders.end() ? 0
        : static_cast<uint32_t>(it->second.size());  // ← 未排除 pending_cancel
}
```
同文件 `totalActiveOrders`/`getActiveOrders` 都排除了 pending_cancel，**三处口径不一致**。

`CloseoutExecutor::handleExecuting`（`CloseoutExecutor.cpp:213-216`）用此值作 inflight guard：closeout 单撤单后 `pending_cancel=true` 但未 `onOrderDone` → count 仍 >0 → guard 永不归零 → **closeout 卡死，盘后未平仓**。

**修复**:
```cpp
uint32_t n = 0;
for (const auto& info : it->second) if (!info.pending_cancel) ++n;
return n;
```

---

### B3【严重·新】撤单无超时机制 → pending_cancel 单可能永久残留

**位置**: `UnifiedOrderTracker::checkAutoCancel`（`UnifiedOrderTracker.cpp:351-359`）显式跳过 `isPendingCancel()` 的单

所有撤单路径仅置 `pending_cancel=true` 后调 `stra_cancel`，无超时清理。若交易所 ack 丢失/网络断连，单永远卡 pending_cancel：占用 `max_orders` 配额、B1 持续误拒、`getActiveCountBySource` 虚高（B2）。

**修复**: tracker 加 `pending_cancel_timeout_ms`（建议 5000ms），`checkAutoCancel` 扫超时单强制 `untrackOrder` + 告警。

---

### B4【严重·新】closeout 期间不暂停 ARB → drain 永远完成不了

**位置**: `ArbExecutionBridge::onTick`（`ArbExecutionBridge.cpp:21`）

仅检查 `use_spread_arbitrage`，不检查 RiskMonitor 的 closeout 状态。closeout DRAINING 阶段 ARB 仍可下单 → `tracker.getOrderCount()>0` → drain 永不完成；ARB 单被 closeout 单反向成交制造意外盈亏。

**修复**:
```cpp
if (_deps.risk_monitor && _deps.risk_monitor->isCloseoutTriggered()) return;
```

---

### B5【严重·新】channel_lost 不停 arb 线程 → 队列满后无声丢 tick

**位置**: `UftFutuMmStrategy::on_channel_lost:1961-1993`

仅 `haltTrading + cancelAll + 撤单`，**无 `_async_arb->setConfig(enabled=false)`**。arb 线程继续生成信号推送 `_order_queue`，队列满后丢 tick，系统看似正常但 arb 完全失效。

**修复**: channel_lost 时调 `_async_arb->setConfig(enabled=false)`。

---

### B6【严重·新】arb 线程无 try/catch → 异常致线程静默退出

**位置**: `AsyncArbitrageExecutor.cpp:188`（arbThreadFunc 顶层）

`generateSignals`/`executeSignal` 若抛异常（如策略访问空指针），arb 线程退出，主线程继续 pushTick 到满队列后无声丢弃。

**修复**: arbThreadFunc 顶层 try/catch，异常时 disable arb + EventNotifier 告警，不让线程死。

---

### B7【误报·已复核】on_entrust 在 RISK_HALTED 期间仍递增 _order_error_count

**复核结论：误报。** `UftFutuMmStrategy.cpp:2085-2090` 已有早退守卫
`if (qphase==RISK_HALTED) return;`，在 `_order_error_count++`（2107 行）**之前**返回，
计数不会被污染。探索 agent 误读了控制流（把"set(ERROR) 被静默拒绝"与"count++ 污染"
误认为同一路径）。**无需修复。**

---

### B8【严重·数学】`net_exposure` 符号错误 → 风控净敞口虚高 3 倍 【已修复】

**位置**: `SpreadRiskManager.cpp:96-98`
```cpp
risk.leg1_exposure = state.leg1_position * state.leg1_price;   // 有符号
risk.leg2_exposure = state.leg2_position * state.leg2_price;   // 有符号
risk.net_exposure  = risk.leg1_exposure - state.beta * risk.leg2_exposure;
```
多 leg1(+10@100) + 空 leg2(-10@50)，beta=1：net = 1000 - 1×(-500) = **1500**，而真实对冲后净敞口 = 1000 - 500 = **500**。空头负号被当"加"。

**修复**:
```cpp
risk.net_exposure = risk.leg1_exposure + state.beta * risk.leg2_exposure;  // 符号同向叠加
// 或用绝对值: abs(leg1) - beta*abs(leg2)
```

---

### B9【严重·会计】resetDailyPnl 不重置 avg_cost → 隔夜浮盈重复计入

**位置**: `FutuPortfolio.cpp:137-145`

`resetDailyPnl` 清 realized/unrealized/daily_pnl 但不动 `avg_cost`。下一次 `markToMarket`（`:99-101`）用昨日 `avg_cost` 重算 `unrealized = (lastPrice - avg_cost)*position*multiplier`，把"建仓以来的浮盈"加回今日 `daily_pnl`。若昨日大亏，今日开盘 unrealized 为负叠加，今日 `max_loss` 阈值更易误触。

`setReferencePrice` 接口存在（`:121-126`）但无调用方。

**修复**: `on_session_begin` 对隔夜持仓调 `setReferencePrice(code, pre_close)` 重置 avg_cost 为昨收。

---

### B10【中等·V6记录】SO-2 首 tick 门可被绕过

**位置**: `SpreadOptimizer.cpp:85-87`

`if (_last_output_spread_mult < 0.5) _smoothed_spread_mult = new_smoothed` —— 若极端毒性参数使 smoothed 跌破 0.5，每次调用都重置为首 tick 路径，绕过 ±10%/15% 变化率限制。

**修复**: 用独立 `bool _first_tick` 标志替代 0.5 阈值判断。

---

### B11【中等·新】`getPositionBreachedContract` 只返回首个违规合约

**位置**: `FutuPortfolio.h:409-417`

`checkRiskLimits`（`FutuRiskMonitor.cpp:251`）只调用一次。多合约同时超限时只处理第一个，其余被忽略。V6 B1 修复了 FLATTEN 阈值可达性，但单合约轮询的根因仍在。

**修复**: 改返回 `vector<const ContractState*>` 或在 Portfolio 维护"违规合约集合"迭代器接口。

---

### B12【中等·新】cancelByPair 防御性补撤条件错误（死代码）

**位置**: `OrderRouter.cpp:264-269`

```cpp
if (!sent && orders_it == _active_orders.end()) { ctx->stra_cancel(oid); }
```
ARBITRAGE 桶构造时预分配（`:22`），条件 `orders_it == end` 仅在桶从未创建时成立 → 防御性 cancel 永不触发。`_oid_to_pair` 出现脏数据时漏撤残留腿。

**修复**: 条件改为 `if (!sent)`（去掉 `orders_it == end` 限制）。

---

### B13【中等·新】cancelOrder（单订单版）不打 pending_cancel 标记

**位置**: `OrderRouter.cpp:199-202`

与 `cancelAllBySource`（`:215-219` 置 `pending_cancel=true`）不一致。外部调 `cancelOrder` 后，`getActiveOrders` 仍返回该单，STP 仍视其活跃。

**修复**: `cancelOrder` 内遍历找对应 `ActiveOrderInfo` 置 `pending_cancel=true`。

---

### B14【中等·新】orphan leg 仅进程内跟踪，崩溃重启裸腿无人对冲

**位置**: `AsyncArbitrageExecutor.cpp:575-681`（`_orphan_legs_from_arb`/`orphan_legs_deferred`）

进程崩溃 → 在途 orphan 信息丢失。

**修复**: orphan leg 入队时落盘 lightweight journal，启动时重放。

---

### B15【中等·新】`submitExitLong/Short` 缺 price≤0 防御

**位置**: `OrderRouter.cpp:127-197`

`submitBuy/submitSell` 有 `price<=0` 拒单（`:45,93`），`submitExitLong/Short` 没有。上游 `computePrice` 在 bid/ask=0 时返回 0 → 传给交易所被拒 → 未跟踪 localid。

**修复**: 入口加同样检查并置 `result.rejected=true`；调用方（`ArbExecutionBridge`/`CloseoutExecutor`）补 `rejected` 字段消费（当前完全未读，见 C3）。

---

### B16【中等·新】VaR 缺合约乘数（视 spread_type 而定）

**位置**: `SpreadRiskManager.cpp:103-105`
```cpp
double position_value = std::abs(state.spread_position) * state.spread_std;
risk.var_99 = 2.33 * position_value;
```
`SIMPLE_DIFF` 模式下 `spread = p1 - p2`（不含乘数），对 IF（乘数 300）VaR 低估 300 倍。`WEIGHTED` 模式 spread 含 `m1/m2` 乘数（`SpreadCalculator.cpp:130`）则正确。

**修复**: 显式乘合约乘数，或按 `spread_type` 分支处理。

---

### B17【中等·新】portfolio VaR 假设 pair 独立

**位置**: `SpreadRiskManager.cpp:231-244`

`total_var_sq = sum(var_i^2)` 假设 pair 间独立，但跨期 pair 高度相关（共享腿）。正相关组合**低估风险**。

**修复**: 引入 pair 间 correlation matrix，或用 historical simulation。

---

### B18【中等·新】`calculateVaR` 忽略 confidence 参数

**位置**: `SpreadRiskManager.cpp:221-229`

接受 `confidence` 参数但永远返回预存 var_99。调用方传 0.95 得到 99% VaR（数值更大）。

**修复**: 真正使用 confidence（z 因子 1.65/2.33），或删除参数避免误导。

---

## C. 代码质量分析

### C1【严重·死代码】MarketMakingEnhancer 整模块未接线

**位置**: `MarketMakingEnhancer.cpp` 全文件 + `SpreadArbitrageManager.cpp:514+` 调用点

`getQuotingAdjustment` 每 tick 计算 skew_mult/suppress_flag（含 RingBuffer push），但**结果不注入 SpreadOptimizer**（`UftFutuMmStrategy.cpp:1067-1069` 注释明示 observe-only）。纯 CPU 浪费。

**修复**: 要么接线到 SpreadOptimizer（作为额外 skew 来源），要么删除调用。`_adjustment_history`（`h:162`）字段也从未读取。

---

### C2【严重·新】PerformanceAnalyzer pending_adverse 内存泄漏 + 清理逻辑错

**位置**: `PerformanceAnalyzer.cpp:121-149`

`for (auto& pa : _pending_adverse)` 只对 `pa.code==code` 递减 ticks_remaining，**低流动性合约的 pending 永不评估**（无 tick 不减）。`_pending_adverse` 是 `std::deque` 无上限 → 长期运行内存膨胀。

清理逻辑 `while (!empty() && front().evaluated) pop_front()` 只 pop 连续已评估的 front，中段已评估元素永远残留。

**修复**: 加全局 wall-clock 超时；用 `std::remove_if` 全清理。

---

### C3【严重·新】Sharpe 年化因子错误，系统性低估 ~100 倍

**位置**: `PerformanceAnalyzer.cpp:232`
```cpp
metrics.sharpe_ratio = mean / std_dev * std::sqrt(250);
```
`_pnl_history` 是 **per-trade** PnL（每成交 push 一次，`:94`），不是 per-day。HFT 一天可上千笔，正确年化因子 = `sqrt(250 * trades_per_day)`。当前用 `sqrt(250)` 等价假设每天 1 笔。

**修复**: 记录每笔时间戳，按时间加权算 per-second/per-minute returns 再年化。

---

### C4【严重·新】inventory_pnl 字段恒为 0（写路径缺失）

**位置**: `PerformanceAnalyzer.cpp:283` 读 `_positions[code].realized_pnl`，但 `updatePosition`（`:160-166`）从不写该字段。PnL Attribution 中 inventory_pnl 永远 0。

**修复**: `updatePosition` 补 `realized_pnl` 累加，或删除该 attribution 项。

---

### C5【中等·新】beta 双标准不一致

**位置**: `SpreadCalculator.h:110` vs `SpreadCalculator.cpp:385`

`getBeta()` 返回 `_smoothed_beta`，`getState().beta` 返回 raw `_beta`。下游 `SpreadRiskManager` 用 state.beta（raw），Portfolio 用 getBeta()（smoothed）。

**修复**: 统一导出 raw + smoothed 两字段，调用方明确选择。

---

### C6【中等·新】beta 截断 [0.7, 1.5] 硬编码，跨品种配置失效

**位置**: `SpreadCalculator.cpp:188-191`

同品种跨期（IF2503 vs IF2506）合理，但跨品种（rb vs I 真实 beta ~0.3；Au vs Ag ~0.05）被错误截断到 0.7。

**修复**: 从 `SpreadPairConfig` 读 pair 级 `[beta_min, beta_max]`，默认放宽 `[0.1, 5.0]`。

---

### C7【中等·新】TimeUtils 多种时间格式混用

全项目存在 3 种时间戳格式：epoch ms、HHMMSSmmm（打包整数）、压缩日期。V6 B14 修了 `SelfTradeCalibrator` 的 `timestampToMs`，但 `TickTransactionInferer.cpp:405` 的 `window_start = current_time - imbalance_window_ms` 仍可能误用 HHMMSSmmm（跨秒/分边界产生非法时间戳）。

**修复**: 引入强类型 `Timestamp<TimeDomain>`，数据入口统一转换 epoch ms。

---

### C8【中等·新】computeBidPrice/computeAskPrice 死代码

**位置**: `FutuQuoter.h:249-267`

实际报价由 `computeQuotePrices`（cpp）直接用 `l0_bid/l0_ask_price`，这两个方法用完全不同公式且无调用者。维护者易混淆。

**修复**: 删除。

---

### C9【中等·新】liquidity_score 永远为 0

**位置**: `SpreadRiskManager.cpp:154,202`

`liquidity_sum` 从未累加。`PortfolioRiskSummary.liquidity_score` 默认 1（`h:150`），计算后变 0。下游若依赖判断流动性，永远认为"无流动性"。

**修复**: 补 liquidity_sum 累加逻辑，或删除字段。

---

### C10【中等·新】PF-3 ContractState.last_update 永远为 0

**位置**: `FutuPortfolio.cpp:90`

`cs->last_update = 0` 注释 "Could use tick timestamp"，从未写入。无法用于 stale data 检测。

**修复**: `markToMarket` 内赋值 `cs->last_update = TimeUtils::getLocalTimeNow()`。

---

### C11【中等·新】SC-2 `_alpha` 回归截距计算后从不读取

**位置**: `SpreadCalculator.cpp:316`

`_alpha = (sum_y - beta*sum_x)/n` 纯浪费 CPU。getState 不导出。

**修复**: 删除，或在 SpreadState 导出用于 fair value 计算。

---

### C12【中等·新】闰年算法双重计算

**位置**: `SpreadRiskManager.cpp:26-48`

L36 `(year-2000)/4` 已含闰年，L41 又在 `month>2 && 闰年` 时 `+=1`，差 1 天。

**修复**: 用 `std::chrono::year_month_day` 或完整 Gregorian 实现。

---

### C13【中等·新】predictive vs realized toxicity 融合用 max 削弱 realized

**位置**: `ToxicFlowDetector.cpp:160-162`

`toxic_score = max(weighted_score, extreme_signal * weight)`。realized 0.9 被 predictive 0.3 稀释到 0.54。

**修复**: 改加权融合而非 max 覆盖。

---

### C14【轻微·V6已修残留】LockFreeQueue `_drop_count` 成 dead code

**位置**: `LockFreeQueue.hpp:121-138`

V6 C1 删了 `pushOverwrite`，但 `_drop_count` 字段 + tryPop 的 drop 处理块保留（恒 0，每次 pop 多一次 atomic load）。

**修复**: 删除字段与处理块，tryPop fast-path 省 ~5ns。

---

### C15【轻微·新】BASIS spread 模式与 SIMPLE_DIFF 完全重复

**位置**: `SpreadCalculator.cpp:106-133`

**修复**: 删除 BASIS 或赋予不同语义。

---

### C16【轻微·新】RateCounter window_ms==0 变无限速

**位置**: `OrderRouter.h:60-69`

误配时 `now - window_start >= 0` 永真 → 流控失效。

**修复**: 启动时校验拒绝 window_ms==0。

---

## D. 性能分析（高频对速度的极致追求）

### 延迟基线估算
- **主线程 on_tick → refreshQuotes**: ~8–20μs（行情/持仓状态相关）
- **套利线程 pushTick → executeSignal**: ~3–8μs
- **目标 <10μs**，主路径有压力

### P1【严重·新】`TickContext.code` 每 tick 堆分配（-30~80ns/tick）

**位置**: `StrategyCoordinator.h:49` + `.cpp:222`

`struct TickContext { std::string code; }`，合约码如 `CFFEX.IF.2503` 长 15 字节超 SSO 阈值，大概率 heap。每 tick 1 次 malloc/free。

**修复**: 改 `const char* code`（生命周期由调用方 `stdCode` 保证），或改 `char[16]` 固定缓冲。

---

### P2【严重·新】`ArbTickData.code` 每 tick 2 次堆分配（-60~150ns/tick）

**位置**: `AsyncArbitrageExecutor.h:46-53` + `.cpp:84`

`ArbTickData tick(code, ...)` string 拷贝构造 + `tryPush` 内 placement-new `T(item)` 再拷贝 = 2 次 malloc/tick。

**修复**: `code` 改 `char[16]`（仿 `UnifiedOrderInfo::code[MAX_CODE_LEN]`），placement-new 变 trivial 拷贝。

---

### P3【严重·新】`checkAutoCancel` 每 tick 2 次 vector 堆分配（-100~200ns/tick）

**位置**: `UnifiedOrderTracker.cpp:341-359`

```cpp
std::vector<CancelAction> actions;        // 分配 1
std::vector<size_t> active_indices;       // 分配 2
active_indices.reserve(_orders.size());   // 可能 realloc
```
1000 tick/s × 2 = 2000 malloc/s。

**修复**: 改成员级复用缓冲（与 `_violations_buf` 同模式），`checkAutoCancel` 改 out-param。

---

### P4【严重·新】`getPairsForContract` 返回 vector 按值（-3 malloc/tick）

**位置**: `SpreadCalculator.cpp:532` 被 `StrategyCoordinator.cpp:1063,1081,1084` 一 tick 调 3 次，每次返回 `std::vector<std::string>`。

**修复**: 改返回 `const std::vector<std::string>&`（启动时缓存），或合并为单次 `getArbContextForLeg(code)` 返回结构体（顺带省 2× spinlock）。

---

### P5【严重·新】spinlock 未 cacheline 对齐，false sharing（-100~500ns/acquire）

**位置**: `AsyncArbitrageExecutor.h:338,343`、`SpreadArbitrageManager.h:332,384,451`

`std::atomic_flag` 单字节未 `alignas(64)`，与 map 数据同 cacheline。arb 线程与主线程高频争用时 cacheline bouncing 严重。

**修复**: `alignas(64) std::atomic_flag _xxx_spin;` —— 一行改动，高竞争时收益显著。

---

### P6【中等·新】correlation + beta 重复扫描 + 重复 std::log（-700ns~1.5μs/10tick）

**位置**: `SpreadCalculator.cpp:201-324`

两函数各 2 次 O(n) 扫描 + 都算 `std::log(curr/prev)`，同一 log_return 算 4 次。n=256 时 256×4×15ns ≈ 15μs/次，每 10 tick 触发 → ~1.5μs/tick 摊销。

**修复**: 
1. 合并两函数为单次扫描，共享 log_return 缓存 → 省 50% 扫描 + log；
2. tick 到达时增量算 1 个 log 存 RingBuffer → O(1) 增量 + O(n) 求和。

---

### P7【中等·新】executeSignal 自检持 spinlock + 线性扫描（-200ns~1μs/signal）

**位置**: `AsyncArbitrageExecutor.cpp:357-435`

持 `_mm_orders_spin` 期间 2× hash find + 2× 线性扫订单。主线程 `updateMMOrders` 同 spin 写 → cacheline bouncing。

**修复**: MM orders 改排序数组 + 二分；或预计算 `min_sell_price`/`max_buy_price` 标量在 `updateMMOrders` 时维护 → 自检 O(1)。

---

### P8【中等·新】generateSignal 每 pair 3× spinlock acquire（-150~400ns/pair）

**位置**: `SpreadArbitrageManager.cpp:448-563`

`_pair_states_spin` 每 pair 加锁 3 次（读 last_time、读 state、写 last_signal）。

**修复**: 锁内一次完成读 state + 读 last_time + 写 last_signal。

---

### P9【中等·新】ContractState hot/cold 未分离（cache miss -30~50%）

**位置**: `FutuPortfolio.h:59-176`

~160-200 字节，`code`（cold）与 `position`/`last_price`（hot）同结构。`getTotalDelta/Exposure` 每 tick 遍历，cacheline 被 cold 字段污染。

**修复**: 拆 `ContractStateHot`（≤64B，含 position/hedge_ratio/multiplier/last_price/bid1/ask1/max_position）+ cold 部分；聚合计算仅遍历 hot 数组。

---

### P10【中等·新】入口 4× unordered_map 字符串哈希（-100~200ns/tick）

**位置**: `StrategyCoordinator.cpp:242-257`

每 tick 对同一 code 做 4 次 `wt_hashmap<std::string>::find`。

**修复**: 启动时建 `code → uint16_t code_id` intern 表，4 组件表改 `std::vector<unique_ptr<T>>` 按 code_id 索引。

---

### P11【中等·新】SpreadOptimizer.cpp:194 每 tick debug 日志（启用时 -1~2μs/tick）

**位置**: `SpreadCalculator.cpp` 上游报价日志，11× float 格式化。生产 INFO 级零开销，但排障开 debug 时翻倍延迟。

**修复**: 采样 `if ((tick_count & 0xFF) == 0)` 或双重门控。

---

### P12【轻微·新】tick→quote 尾部 2× chrono::now（-80~150ns/tick）

**位置**: `StrategyCoordinator.cpp:302-317`

`high_resolution_clock::now` ~40-70ns/次。

**修复**: 用 TSC（rdtsc）替代，或合并为单次时间戳。

---

### P13【轻微】SPSC tryPop drop 处理恒 0 仍 acquire load（-5ns/pop）

**位置**: `LockFreeQueue.hpp:121`

fast-path：`if (_drop_count.load(relaxed)==0) skip block`。

---

## E. 优先级排序的总建议表

### P0 — 立即修复（资金安全/数据正确性，1-3 天）

| ID | 问题 | 文件:行 | 收益/风险 |
|----|------|---------|----------|
| B1 | STP 不过滤 pending_cancel | UnifiedOrderTracker.cpp:459 | 消除高频 ARB 误拒 |
| B2 | getActiveCountBySource 不过滤 pending_cancel | OrderRouter.h:200-205 | 防 closeout 卡死 |
| B3 | 撤单无超时 | UnifiedOrderTracker.cpp:351-359 | 防状态永久残留 |
| B4 | closeout 期间不暂停 ARB | ArbExecutionBridge.cpp:21 | 防 drain 卡死 |
| B5 | channel_lost 不停 arb | UftFutuMmStrategy.cpp:1961 | 防无声失效 |
| B6 | arb 线程无 try/catch | AsyncArbitrageExecutor.cpp:188 | 防线程静默退出 |
| B7 | HALT 期间误增 error_count | UftFutuMmStrategy.cpp:2075 | 防误触发硬暂停 |
| B8 | net_exposure 符号错 | SpreadRiskManager.cpp:98 | 风控指标正确 |
| B9 | resetDailyPnl 不重置 avg_cost | FutuPortfolio.cpp:137-145 | 隔夜 PnL 正确 |
| C3 | Sharpe 年化错误 | PerformanceAnalyzer.cpp:232 | 调参反馈正确 |
| C4 | inventory_pnl 恒 0 | PerformanceAnalyzer.cpp:283 | PnL 归因正确 |

### P1 — 短期修复（1-2 周）

| ID | 问题 | 类型 |
|----|------|------|
| A1 | 4 套状态 SSOT 违规 | 架构/已确认遗漏 |
| C1 | MarketMakingEnhancer 整模块死代码 | 代码/每 tick 浪费 CPU |
| C2 | PerformanceAnalyzer 内存泄漏 | 代码/长期运行 |
| C6 | beta 截断硬编码 | 代码/跨品种失效 |
| B16 | VaR 缺乘数 | 业务/数学 |
| B12/B13 | cancelByPair 死代码 + cancelOrder 不一致 | 业务/可靠性 |
| P1-P5 | 5 项堆分配 + cacheline 优化 | 性能/-220~530ns+100~500ns/tick |

### P2 — 中期改进（1 月）

| ID | 问题 | 类型 |
|----|------|------|
| A2 | SpreadArbitrageManager 拆分 | 架构 |
| A4 | 风控规则 IRule 链 | 架构/扩展 |
| P6-P8 | 算法与锁优化 | 性能/-1~2μs/10tick |
| P9-P10 | cache locality + code_id | 性能 |
| B14 | orphan leg 落盘 journal | 业务/容错 |

### P3 — 长期演进

- A1 终局：TradingState 全 atomic，删 FutuRiskMonitor 重复 atomic
- ARB_SELF_CLOSE_DESIGN Phase D1：完整 arb 状态机替代零散 in_flight/intent/overshoot
- D2：或phan leg 持久化 + 进程恢复
- 强类型 Timestamp<TimeDomain>

---

## F. 设计亮点（值得保持）

1. **套利策略插件化**（`ISpreadStrategy` + `SpreadStrategyRegistry` + `BuiltinStrategyRegistrar`）—— 新增策略注册一行即可
2. **信号源插件化**（`ISignalSource` + `SignalAggregator`，6 种内置信号源）
3. **SPSC 队列 alignas(64) 隔离** `_head`/`_tail`，无 false sharing
4. **世代号门控** MM 快照同步，跳过未变化深拷贝
5. **WTSLogger 惰性求值**（level check 先于 format，thread_local buffer 无堆分配）
6. **recordOrderFill 用 original_qty 防部分成交提前 untrack**
7. **B-3 门控**（in_flight 跟踪 + intent 通道 + overshoot 冷却）三层 arb 风控设计完整
8. **computeHedge 50% 截断** 防对冲 overshoot
9. **TradingState 单线程写契约** + canTransitionQuoting 校验
10. **closeout 三层编排**（Coordinator 状态机 + Orchestrator 时序 + Executor 三阶段）+ inflight guard 防 over-fill

---

*文档结束。所有发现均带 file:line 引用，可按 ID 索引定位修复。建议按 P0→P1→P2→P3 顺序实施，每批改完跑 `make WtUftRunner` 编译验证。*
