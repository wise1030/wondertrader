# WtFutuCore 套利自主平仓修复设计方案 v2.0

> 生成日期: 2026-07-19
> 方法: 3 组并行深度源码审计 + 2 组并行事实核查 + 成本模型论证
> 前置报告: [DEEP_ANALYSIS_V5.md](DEEP_ANALYSIS_V5.md) (v5.0, 48 项 Bug 修复)
> 状态: **Phase A (A1-A10) + Phase B (B1-B6) + C0 配置框架已实施并编译通过**；C1/C2 为配置开关，待回测验证后开启

## 版本历史

| 版本 | 日期 | 核心变更 |
|------|------|---------|
| v1.0 | 2026-07-19 | 初版：全解禁路径（CLOSE→TIMEOUT→STOP_LOSS 逐步解禁） |
| **v2.0** | 2026-07-19 | **重大修订：分级执行路径**。业务定位修正（MM 主导 + ARB 嵌入）；CLOSE 保持 B-3 抑制（特性保留）；STOP_LOSS 提前至 C1（最高优先级）；TIMEOUT 走 maker 单；新增成本模型（fee + bid/ask spread cost + 滑点）；Phase A 扩展 A9/A10；B5 改事前+事后双层；B6 处理一合约多 pair |

**v2.0 修订决策（用户已确认）**：
- Q1: 业务定位 = MM 主导，ARB 服从，手续费豁免是核心优势 ✅
- Q2: STOP_LOSS 提前至 C1（最高优先级）✅
- Q3: CLOSE 保持 B-3 抑制（MM maker 消耗，利用免手续费）✅
- Q4: Phase A 补充 A9（yaml 键错配）+ A10（minProfitThreshold 接线）✅
- Q5: 成本模型必须包含 **bid/ask spread cost**（taker 成本主体），不只是手续费 ✅
- Q6: 采纳 v2.0 方案大纲 ✅

---

## 一、背景与问题陈述

### 1.1 当前架构（B-3 门模式）

`SpreadArbitrageManager::applyB3Gate` (`SpreadArbitrageManager.cpp:761-773`) 在入口处**无条件抑制**所有平仓类信号：

```cpp
// Suppress CLOSE / STOP_LOSS / TIMEOUT signals — let MM consume.
// (Scheme B-3: arb never actively closes; MM's contract_skew handles it.)
if (raw.type == SpreadSignalType::CLOSE_LONG_SPREAD ||
    raw.type == SpreadSignalType::CLOSE_SHORT_SPREAD ||
    raw.type == SpreadSignalType::STOP_LOSS ||
    raw.type == SpreadSignalType::TIMEOUT_EXIT ||
    raw.type == SpreadSignalType::REBALANCE)
{
    result.type = SpreadSignalType::NONE;
    return result;
}
```

设计意图：套利只开仓、不平仓，全部由做市的 inventory skew 反向消耗。

### 1.2 业务痛点再分析：特性 vs 缺陷

v1.0 将 4 个现象全部视为"PnL 杀手"。v2.0 经成本模型论证后重新分类：

| 场景 | B-3 行为 | v1.0 判断 | **v2.0 判断** |
|------|---------|----------|--------------|
| z-score 回归到 exit_z | 平仓信号被抑制，MM skew maker 单消耗 | 平仓延迟 = PnL 杀手 | **特性**：maker 消耗赚 spread + 免手续费，经济最优（见 §4.3） |
| z-score 持续不收敛 | 无 timeout_exit 退出 | 仓位死扛 = PnL 杀手 | **半缺陷**：需退出，但应走 maker 单（挂 mid），非 taker |
| 止损条件触发 | STOP_LOSS 完全失效 | 风控漏洞 | **真缺陷（P0）**：趋势行情亏损持续扩大，必须 taker 立即退出 |
| 极端行情 | 套利仓变长期敞口 | 被动隔夜 | 部分由 STOP_LOSS 失效引起，随 C1 解决 |

**结论：真正的 bug 只有 1 个 —— STOP_LOSS 失效。其余 3 个是 B-3 的合理代价或可用更低成本方式解决。**

### 1.3 单账本设计带来的耦合

`FutuPortfolio` 是 MM + ARB 合一的单一 SSOT（`FutuPortfolio.h:57-174`），**无 source/tag 字段**（已核实）：
- 所有成交（MM/ARB/Hedge/Closeout）通过 `UftFutuMmStrategy::on_trade` → `_portfolio->updatePosition`
- `getTotalDelta()` (`FutuPortfolio.h:270-276`) 合并求和，不区分来源
- MM skew 用总 Delta 计算，**arb 持仓会驱动 MM 报价偏移**

### 1.4 简单解禁的致命风险

若直接删除抑制块，立即出现 3 类问题：

| 风险 | 触发机制 | 严重性 |
|------|---------|--------|
| **过冲** | arb close 单 + MM skew 单同时消耗同仓位 → 从 +N 跳到 -M（反向建仓） | P0 |
| **超量平仓** | 1-tick 滞后让 arb 读 stale `spread_position` | P0 |
| **平仓单双发** | CLOSE 信号绕过 in_flight 检查（line 835-840） | P1 |

**v2.0 注**：由于 CLOSE 保持抑制，上述风险主要暴露面缩小至 STOP_LOSS/TIMEOUT 路径，防御设计不变但触发概率大幅降低。

### 1.5 业务定位与成本优势（v2.0 新增）

**业务架构**：
```
主业务: 双边做市报价 (MM)
  ├─ 报价义务: 做市商考核, 不可为 ARB 中断
  ├─ 成本优势: maker 单豁免/部分豁免手续费
  └─ 收益来源: bid/ask spread capture

嵌入业务: 跨期套利 (ARB)
  ├─ 定位: 增强 MM 收益, 服从 MM 节奏
  ├─ 信号: 合约对趋势/回归特征 (z-score)
  └─ 成本: 开仓 taker (付 spread), 平仓可借道 MM maker (赚 spread + 免 fee)
```

**核心洞察**：MM 的 inventory skew 天然是 maker 单引擎。ARB 仓位经由 MM skew 消耗 = **平仓端零手续费 + 赚取 spread**。这是 B-3 设计的经济本质，v2.0 将其从"隐性巧合"提升为"显性策略"。

---

## 二、设计原则

1. **灰度可逆**：所有解禁通过配置开关控制，可一键回滚到纯 B-3 行为；B-3 代码不删除
2. **多层防御**：B-3 门 → intent 协同 → 事前预估 + 事后保险丝 → 残腿兜底
3. **不破坏单账本**：保持 Portfolio SSOT，不引入双账本复杂度
4. **时序安全**：尊重 1-tick 滞后，平仓前双层校验（arb 线程粗判 + 主线程精判）
5. **分级执行**：按信号类型匹配成本最优的退出方式（见 §4.3）
6. **激活死代码优先**：MarketMakingEnhancer / getQuotingAdjustment / trackArbOrder 已存在，优先接线
7. **MM 主导，ARB 服从**（v2.0 新增）：ARB 任何行为不得中断 MM 报价义务；协同方向是 ARB 利用 MM 通道，而非 MM 为 ARB 让路

---

## 三、关键决策（v2.0 已确认）

| 决策点 | v1.0 | **v2.0** | 依据 |
|--------|------|----------|------|
| 业务定位 | （未明确） | **MM 主导 + ARB 嵌入增强** | Q1 |
| CLOSE 信号 | C1 解禁走 taker | **保持 B-3 抑制**（MM maker 消耗） | Q3 + 成本模型 §4.3 |
| TIMEOUT 信号 | C2 解禁走 taker | **解禁，走 maker 单**（挂 mid，超时升级） | 成本模型 §4.3 |
| STOP_LOSS 信号 | C3 解禁（最低优先级） | **C1 立即解禁**（最高优先级，FAK+对手价） | Q2，止损失效是唯一真缺陷 |
| 解禁顺序 | C1(CLOSE)→C2(TIMEOUT)→C3(STOP_LOSS) | **C1(STOP_LOSS)→C2(TIMEOUT)**，CLOSE 永不解禁 | 风险优先 + 成本最优 |
| 成本模型 | 缺失 | **fee + bid/ask spread cost + 滑点 三层** | Q5 |
| Phase A 范围 | A1-A8 | **A1-A10**（+A9 yaml 键错配 +A10 minProfitThreshold） | Q4 |
| B5 保险丝 | 仅事后检测 | **事前预估 + 事后兜底双层** | 评审建议 |
| B6 协同 | 假设 1 leg = 1 pair | **处理 1 leg : N pair 映射** | 事实核查发现 |
| 回测验证 | 需要 | 需要，**用 WtBtRunner 加速灰度**（周期 6 周→2-3 周） | 评审建议 |

---

## 四、成本模型与正期望验证（v2.0 核心新增）

### 4.1 期货高频交易成本构成

taker 单的真实成本**远不止手续费**，主体是 bid/ask spread cost：

```
TakerCost_per_leg = SpreadCost + Fee + Slippage + Impact
                     │           │      │         │
                     │           │      │         └─ 大单穿多档 (本策略小单, ≈0)
                     │           │      └─ 价格跳动 (通常 0~1 tick)
                     │           └─ 手续费 (做市商部分豁免)
                     └─ 穿过买卖价差 (成本主体!)
```

**Maker 单成本结构完全不同**：
```
MakerCost_per_leg = -SpreadCapture + Fee_maker + AdverseSelection
                     │               │            │
                     │               │            └─ 被 informed flow 选中 (MM 已有 toxicity 防护)
                     │               └─ 做市商豁免 ≈ 0
                     └─ 赚取 spread (负成本!)
```

### 4.2 定量对比（以 SHFE 白银 ag 为例）

参数：`multiplier = 15 kg/手`，`tick = 1 元/kg`，`1 tick = 15 元/手`，典型双边 spread = 1 tick，价格 ≈ 8000 元/kg（单手金额 ≈ 12 万元）。

| 成本项 | ARB taker 平仓/腿 | MM maker 消耗/腿 |
|--------|------------------|------------------|
| Spread cost | **+1 tick = +15 元**（付） | **-1 tick = -15 元**（赚） |
| 手续费 | ≈ 6 元（0.5bp，若不豁免） | **0 元**（做市商豁免） |
| 滑点 | 0~1 tick | 0 |
| **单腿合计** | **≈ +21 元** | **≈ -15 元** |
| **两腿合计** | **≈ +42 元** | **≈ -30 元（净收益）** |

**单次平仓路径差异 = 42 - (-30) = 72 元/单位仓位**

年化估算（10 手均仓，日均 10 次平仓，250 交易日）：

| 平仓路径 | 年成本 |
|---------|--------|
| 全部 taker（v1.0 全解禁） | 42 × 10 × 10 × 250 = **-105 万元/年** |
| CLOSE 走 MM maker（v2.0） | -30 × 10 × 10 × 250 = **+75 万元/年** |
| **差异** | **180 万元/年** |

> 注：以上为量级估算，实际参数按品种/做市商协议调整。结论（maker 消耗远优于 taker）对参数鲁棒。

### 4.3 分级执行的经济学论证

| 信号 | 利润期望 | 成本敏感度 | **执行方式** | 论证 |
|------|---------|-----------|------------|------|
| **CLOSE_LONG/SHORT**（z 回归 exit_z） | 正（已实现价差回归利润） | **高**（利润已到手，成本直接侵蚀） | **保持 B-3 抑制，MM maker 消耗** | maker 消耗 = 赚 spread + 免 fee；taker = 付 spread + fee。差异 72 元/单位，CLOSE 是高频信号（每次回归都触发），年化差异百万级 |
| **TIMEOUT_EXIT**（持仓超时未回归） | 中性偏负（机会成本累积） | 中 | **解禁，ARB maker 挂 mid，30s 未成交升级 taker** | 低频信号；maker 优先控成本，超时升级保证退出 |
| **STOP_LOSS**（z 突破 stop_loss_z） | 负且扩大中 | **低**（止损成本 << 继续亏损） | **解禁，FAK + 对手价立即成交** | 趋势行情 z 从 3σ→5σ = 40 ticks ≈ 600 元/手/腿继续亏损 vs 42 元退出成本，必须立即 taker |
| **REBALANCE** | 中性 | 高 | 保持抑制 | 调仓需求低频，B-3 足够 |

### 4.4 正期望条件与阈值设计

**单次套利往返净收益公式**：

```
NetPnL = SpreadCapture - OpenCost - CloseCost

SpreadCapture = |z_entry - z_exit| × σ_spread × multiplier × legs_qty
OpenCost      = 开仓成本 (taker: 2 legs × (1 tick + fee_open))
CloseCost     = 平仓成本 (按 §4.3 分级)
```

**各模式正期望阈值**：

| 模式 | CloseCost | 开仓净成本 | 正期望条件 |
|------|-----------|-----------|-----------|
| CLOSE → MM maker（B-3） | **-2 ticks**（赚 spread） | 2 legs × fee_open（spread 一开一平互抵） | `\|Δz\| × σ_spread > 2 × fee_open` —— **阈值极低**，exit_z=0.5 即有正期望 |
| TIMEOUT → ARB maker | ≈ -2 ticks（同 maker） | 同上 | 同上 |
| STOP_LOSS → taker | +2 ticks + 2×fee | 2 legs × (2 ticks + fee) 全程 taker | 止损场景：`已浮亏 + CloseCost < 预期继续亏损`，**不看正期望，看减亏** |

**对 A10（minProfitThreshold 接线）的指导**：

```cpp
// min_profit_threshold 语义 = 开仓信号的最低预期利润 (ticks)
// 公式: threshold ≥ 2 legs × (taker_spread_cost + fee) - maker_rebate_close
// 对 ag: ≈ 2 × (1 tick + 0.4 tick_fee) - 2 × 1 tick ≈ 0.8 ticks
// 建议初值: 1.0 tick (含安全裕度), 按品种配置
```

---

## 五、分阶段实施清单

### Phase A：基础 Bug 修复（10 项，独立可上线）

> 与解禁无关的预存在 bug，必须先修，避免新逻辑与旧 bug 纠缠。

| ID | 文件:行 | 改动概要 | 工作量 |
|----|---------|---------|--------|
| **A1** | `SpreadArbitrageManager.cpp:895,146-147` | `in_flight_qty = order_qty * (leg1_ratio + leg2_ratio)`；yaml 加载 `leg2_ratio` 字段（当前硬编码 1.0） | 0.5h |
| **A2** | `UftFutuMmStrategy.cpp:2028-2038` | on_entrust 拒单分支 insert `_arb_hedge_on_fill`；新增 `ArbExecutionBridge::markLegRejected(pair_id)` 接口 | 1h |
| **A3** | `ArbExecutionBridge.cpp:281,75` | `_arb_hedge_on_fill` 从 `set<string>` 改为 `map<string, CloseHedgeState>`，`CloseHedgeState { original_qty, hedged_qty }`；hedge 量 = `original_qty - hedged_qty - current_vol` | 2h |
| **A4** | `UftFutuMmStrategy.cpp:1601-1651` | isCanceled 分支检测 `consumePairTag`，命中则调用 `ArbExecutionBridge::onLegCancelled(pair_id)` 触发 hedge | 1.5h |
| **A5** | `AsyncArbitrageExecutor.cpp:316-323` | STOP_LOSS/TIMEOUT_EXIT 进入方向推导前，校验 `state.spread_position != 0`（当前 ==0 时静默默认"卖 leg1"方向，已核实 bug），否则降级 NONE + 日志 | 0.5h |
| **A6** | `StrategyCoordinator.cpp:738-819` | 补 `case RiskAction::FLATTEN_POSITION:` 分支（枚举存在于 `FutuRiskMonitor.h:97` 且会被返回，但落 default 空分支）；顺带补 WIDEN_SPREAD/REDUCE_SIZE 分支 | 1h |
| **A7** | `OrderRouter.h/cpp:204-221` | 新增 `_oid_to_pair: unordered_map<localid, string>`；`cancelByPair(ctx, pair_id)` **撤该 pair 全部活跃订单（含历史加仓组）**，替换 4 处 `cancelAllBySource(ARBITRAGE)` 调用 | 3h |
| **A8** | `UnifiedOrderTracker.h:333`、`AsyncArbitrageExecutor.cpp` | 接线 `trackArbOrder`：tagOrderPair 时同时 tracker->trackArbOrder；onArbOrderFilled 时 recordOrderFill；onOrderFinalized 时 untrack | 1.5h |
| **A9** (v2.0 新增) | `SpreadArbitrageManager.cpp:118-131` | **statistical 全局段 yaml 键名错配修复**：代码读 `halfLife/entryThreshold/exitThreshold/correlationWindow/minCorrelation/spreadWindow/maPeriod/breakoutThreshold`，yaml 写 `entryZThreshold/stopLossZ/addSafetyRatio/lookbackWindow/stopLossPct/maxTrendBars` —— **全部静默落默认值**；对齐键名 + 接线 `stop_loss_z` 死配置 | 1h |
| **A10** (v2.0 新增) | `AsyncArbitrageExecutor.h:342` | **`setMinProfitThreshold` 接线**（当前无调用者，利润门槛失效）；阈值语义含 spread cost + fee（见 §4.4），从 spread_arbitrage.yaml 加载 `minProfitThresholdTicks: 1.0` | 0.5h |

**Phase A 合计**：约 12.5h（1.5 工作日），可独立 PR。

---

### Phase B：协同机制建设（解禁前提）

#### B1. ArbIntent 实时通道（3h）

**目标**：让 arb 线程能向主线程广播"我正在平仓某 pair"的意图。

**实现**：
- `SpreadArbitrageManager` 新增 `_active_close_intents: atomic<vector<CloseIntent>>`
- `CloseIntent { pair_id, leg1_code, leg2_code, direction, qty, set_time }`
- 接口：
  ```cpp
  void setActiveCloseIntent(const std::string& pair_id, const CloseIntent& intent);
  void clearActiveCloseIntent(const std::string& pair_id);
  bool hasActiveCloseIntent(const std::string& leg_code) const;  // 经 1:N 映射聚合 (见 B6)
  ```
- STOP_LOSS/TIMEOUT 信号通过 B-3 门时设置 → 成交/超时清理
- 写入 spin lock，读取 atomic snapshot

**v2.0 补充**：设置 intent 时同步通知 Coordinator **撤该 pair 两腿的 ARB 方向报价单边**（不是中断 MM 全部报价——MM 义务优先，仅抑制与 ARB 平仓同向的那一侧）。

#### B2. Coordinator 接入 intent（2.5h）

**实现**：
- `StrategyCoordinator` 新增 `setArbManager(SpreadArbitrageManager*)` 注入（生命周期由 UftFutuMmStrategy 保证）
- `processQuoting` (`StrategyCoordinator.cpp:870-942`) 中查询：
  ```cpp
  if (_arb_manager && _arb_manager->hasActiveCloseIntent(tc.code)) {
      // ARB 正在平该 leg: 抑制 MM 同向报价侧 (避免与 ARB 平仓单争抢流动性)
      // 注: 反向报价侧保留, 维持做市义务
  }
  ```

**v2.0 语义澄清**：此协同是"避免自相成交/争抢"，**不是** MM 为 ARB 让出消耗通道——CLOSE 保持 B-3 时 MM skew 照常消耗；仅 STOP_LOSS/TIMEOUT 主动退出期间抑制同侧。

#### B3. 平仓前双层校验（1.5h）

**v2.0 明确双层架构**：

```cpp
// 第一层 (arb 线程, 粗判): executeSignal 入口
if (is_close_signal(signal.type)) {
    // 读 _pair_states (spin lock 保护, 1-tick 滞后可接受)
    double spread_pos = state.spread_position;
    if (std::abs(spread_pos) < 0.5) {  // 已被 MM 消耗完
        signal.type = SpreadSignalType::NONE;
        return;
    }
    signal.suggested_size = std::min(signal.suggested_size,
        std::abs(spread_pos) * _close_cfg.max_close_size_pct);
}

// 第二层 (主线程, 精判): ArbExecutionBridge::processPendingOrders 下单前
void ArbExecutionBridge::processPendingOrders(IUftStraCtx* ctx) {
    for (auto& order : _pending_orders) {
        if (order.is_close) {
            // 实时读 Portfolio SSOT (主线程无 data race)
            double live_pos = _portfolio->getPosition(order.code);
            // 事前过冲预估 (B5 前置)
            double predicted = live_pos + order.signed_qty;
            if (live_pos * predicted < 0) {
                order.qty = std::abs(live_pos);  // clamp 到恰好平仓
                WTSLogger::warn("[ARB_CLOSE] {} clamped {}->{} to avoid overshoot",
                                order.code, ...);
            }
            if (order.qty < min_qty) continue;  // 已被消耗, 丢弃
        }
        // ... 提交
    }
}
```

**决策（开放问题 11.1 已解决）**：采用方案 b（主线程下单前精判），无跨线程 data race，配合 arb 线程粗判形成双层。

#### B4. 平仓专用 in_flight（3h）

- `PairArbState`（`SpreadArbitrageTypes.h:421-441`）新增：
  ```cpp
  double close_in_flight_qty = 0;
  uint64_t close_in_flight_set_time = 0;
  ```
- STOP_LOSS/TIMEOUT 通过 B-3 门时置位（`suggested_size * (r1+r2)`，A1 修复后 ratio 正确）
- onArbOrderFilled 优先扣 close_in_flight，再扣 open in_flight
- 新配置 `close_in_flight_timeout_ms = 5000`（STOP_LOSS 场景收紧至 1000，见 §6 execution_policy）
- 超时清理逻辑复用 open 路径

#### B5. 过冲保险丝：事前预估 + 事后兜底（4h）

**事前**（见 B3 第二层）：下单前预测 sign-flip 并 clamp。

**事后**（兜底）：
- `FutuPortfolio::onPositionUpdate` (`FutuPortfolio.cpp:146`) 检测 sign-flip：
  ```cpp
  if (cs->prev_position * new_pos < 0 && _arb_manager
      && _arb_manager->isLegInActivePair(code)) {
      _arb_manager->onOvershootDetected(code);
  }
  ```
- `onOvershootDetected(leg_code)` 触发：
  - 撤该 pair 所有 ARB 单（A7 的 cancelByPair）
  - 该 pair 进入 COOLDOWN（默认 1h）
  - **告警外发**（v2.0 新增）：`_event_notifier->notify("ARB_OVERSHOOT", ...)`（复用 `FutuRiskMonitor.cpp:89-97` 的 EventNotifier 通道，非仅日志）
- 正常不应触发；触发 = 系统异常需排查

#### B6. 接线 MarketMakingEnhancer + 一合约多 pair 处理（3h）

**事实核查修正**：`_contract_to_pairs` 是 **1:N 映射**（`SpreadCalculator.h:285`，`SpreadCalculator.cpp:456-457`），同一合约可属多个 pair（如 rb2505 同时是 rb-hc 与 rb-i 的腿）。v1.0 的 B6 假设 1:1，v2.0 修正：

```cpp
// Manager 侧聚合接口 (新增)
double SpreadArbitrageManager::getAggregateZscore(const std::string& leg_code) const {
    auto pairs = _calculator_manager->getPairsForContract(leg_code);
    double max_abs_z = 0.0;
    for (const auto& pid : pairs) {
        double z = getPairZscore(pid);
        if (std::abs(z) > std::abs(max_abs_z)) max_abs_z = z;  // 取 |z| 最大者
    }
    return max_abs_z;
}

bool SpreadArbitrageManager::hasActiveCloseIntent(const std::string& leg_code) const {
    auto pairs = _calculator_manager->getPairsForContract(leg_code);
    for (const auto& pid : pairs) {
        if (has_intent_for_pair(pid)) return true;  // any-match 语义
    }
    return false;
}
```

**接线**（`StrategyCoordinator.cpp:870-942`，processQuoting 内）：
```cpp
if (_arb_manager && _mm_enhancer) {
    double zscore = _arb_manager->getAggregateZscore(tc.code);
    bool active_close = _arb_manager->hasActiveCloseIntent(tc.code);
    double adjustment = _mm_enhancer->calculateSkewAdjustment(zscore, confidence);

    if (active_close) {
        // STOP_LOSS/TIMEOUT 退出期间: 抑制与 ARB 平仓同侧的 MM 报价
        // (避免自相成交; 反侧保留维持做市义务)
        suppressSameSideAsArbClose(adjustment, tc.code);
    }
}
```

**注意（v2.0 修正）**：事实核查发现 `MarketMakingEnhancer` 并非完全死代码——内部调用链存在（`calculateAdjustment:514` → `calculateSkewAdjustment/shouldSuppressBid/Ask`），仅外部入口 `getQuotingAdjustment`（`SpreadArbitrageManager.cpp:484`）无调用者。接线路径不变。

**Phase B 合计**：约 17h（2 工作日+）

---

### Phase C：分级解禁（v2.0 重构）

#### C0. 配置框架（2h）

**新增 `ArbCloseConfig`**（`SpreadArbitrageTypes.h`）：
```cpp
struct ArbCloseConfig {
    bool enabled = false;

    struct AllowSignals {
        bool close_long = false;    // v2.0: 永不解禁 (B-3 特性保留)
        bool close_short = false;   // v2.0: 永不解禁
        bool timeout_exit = false;  // C2 解禁, maker 单
        bool stop_loss = false;     // C1 解禁, taker 单 (最高优先级)
    } allow_signals;

    // 分级执行策略 (v2.0 核心)
    struct ExecutionPolicy {
        int order_flag;             // 0=GFD 1=FAK 2=FOK
        double price_offset_ticks;  // 相对 mid 的偏移 (0=mid, 负=更优价)
        uint64_t timeout_ms;
        bool upgrade_to_taker;      // 超时未成交是否升级对手价
    };
    ExecutionPolicy stop_loss_policy   {1, 0.0, 1000,  false};  // FAK 对手价
    ExecutionPolicy timeout_policy     {0, 0.0, 30000, true };  // GFD mid, 超时升级

    double max_close_size_pct = 0.5;
    uint64_t close_in_flight_timeout_ms = 5000;
    bool oversold_protection = true;
    uint64_t overshoot_cooldown_ms = 3600000;
    bool intent_broadcast = true;

    // per-strategy 覆盖 (v2.1, §十四建议2; 默认空 = 全局配置生效)
    // 用途: 趋势策略 CLOSE 低频且 timing 敏感, 可单独解禁走 timeout_policy (maker-first)
    struct StrategyOverride {
        bool close_long = false;
        bool close_short = false;
        std::string execution = "timeout_policy";  // "stop_loss_policy" | "timeout_policy"
    };
    std::unordered_map<std::string, StrategyOverride> strategy_overrides;  // key = strategy name

    bool is_allowed(SpreadSignalType t) const;
    bool is_allowed_for(const std::string& strategy, SpreadSignalType t) const;  // override 感知
};
```

**applyB3Gate 改造**（`SpreadArbitrageManager.cpp:761-773`）：
```cpp
if (is_close_signal(raw.type)) {
    if (!_arb_close_cfg.enabled || !_arb_close_cfg.is_allowed(raw.type)) {
        result.type = NONE;  // 原 B-3 行为 (CLOSE 永远走这里)
        return result;
    }
    // STOP_LOSS / TIMEOUT 通过:
    // → close in_flight 路径 (B4)
    // → arb 线程粗判 (B3 第一层)
    // → 广播 intent (B1)
}
```

#### C1. 解禁 STOP_LOSS（最高优先级，P0）

```yaml
arb_close:
  enabled: true
  allow_signals:
    close_long: false     # 保持 B-3 (特性)
    close_short: false    # 保持 B-3 (特性)
    timeout_exit: false
    stop_loss: true       # ← C1 唯一解禁项
  stop_loss_policy:
    order_flag: 1         # FAK
    price_offset_ticks: 0 # 对手价
    timeout_ms: 1000
    upgrade_to_taker: false
  overshoot_cooldown_ms: 3600000
```

**前置依赖**：A5（方向推导修复）+ A9（stop_loss_z 死配置接线，否则阈值是错的默认值 4.0）必须已上线。

**观察指标**：
- 趋势日 maxDD 改善（**期望显著**，这是 C1 的核心收益）
- STOP_LOSS 误触发率（应 < 5%；误触发代价 = 42 元/单位 taker 成本）
- B5 保险丝触发次数（应为 0）
- 事前 clamp 触发次数（B3 第二层，反映 stale 仓位频率）

**观察期**：回测 3 天 + 小资金实盘 3 天（WtBtRunner 加速，见 §7）。

#### C2. 加解禁 TIMEOUT_EXIT（maker 单）+ 趋势耗尽重分类（v2.1 合入）

```yaml
arb_close:
  allow_signals:
    stop_loss: true
    timeout_exit: true    # ← C2 新增
  timeout_policy:
    order_flag: 0         # GFD
    price_offset_ticks: 0 # 挂 mid 价
    timeout_ms: 30000     # 30s 未成交
    upgrade_to_taker: true # 升级对手价
```

**C2 合入项（v2.1，§十四建议 1）**：趋势耗尽信号重分类——`TrendFollowingStrategy.cpp:234,241` 趋势耗尽分支 `CLOSE_LONG/SHORT_SPREAD` → `TIMEOUT_EXIT`（1 行/处）。语义修正：持仓 bar 数超限本质是超时退出，应走 maker 退出路径而非永久抑制。

**观察指标**：
- 持仓时间分布（应明显缩短，消除"死扛到 EOD"）
- maker 成交率（挂 mid 30s 内成交占比；过低则调 price_offset_ticks=-1 更激进）
- 升级 taker 比例（应 < 20%）
- 组合 PnL（TIMEOUT 走 maker 成本 ≈ -30 元/单位，接近零成本退出）
- 趋势策略：趋势耗尽退出延迟（重分类后应从 ∞ 降至 ≤30s）

**观察期**：回测 1 周 + 实盘 1 周。

**C2 观察期后决策项（v2.1，§十四建议 2/3）**：
- 趋势反转回吐数据 > taker 成本 2 倍 → 启用 `strategy_overrides.trend_following`（CLOSE 走 timeout_policy maker-first）
- 若仍不足 → 评估趋势反转升级 STOP_LOSS 语义（taker 立即退出）

#### CLOSE 信号：永不解禁（v2.0 决策）

CLOSE_LONG/SHORT 永远保持 B-3 抑制，由 MM skew maker 消耗。**这是 v2.0 与 v1.0 的本质区别**：
- v1.0：视 B-3 为缺陷，全解禁
- v2.0：视 B-3 为特性（成本优势），仅修复其真正的缺陷（STOP_LOSS 失效 + 无超时兜底）

**Phase C 合计**：3h（C0）+ 灰度等待（回测加速后 2-3 周）

---

### Phase D：架构完善（可选，长期规划）

| ID | 改动 | 工作量 | 收益 |
|----|------|--------|------|
| **D1** | 引入 `PairExecState {PENDING, LEG1_FILLED, LEG2_FILLED, ORPHAN, COMPLETE, COOLDOWN}` 显式状态机，整合 4 处独立结构（`_oid_to_pair` / `_arb_hedge_on_fill` / `in_flight_qty` / `_orphan_legs_deferred`） | 8h | 大幅降低 bug 风险，可观测性增强 |
| **D2** | per-leg in_flight（彻底解决 ratio 兼容性，替代 A1 的临时修复） | 4h | 支持 arbitrary ratio |
| **D3** | hedge callback STP fallback（`stra_enter_long/short`） | 2h | hedge 单被 STP 拦截时仍能补救 |
| **D4** (v2.0 新增) | 主力合约自动切换（OI 监控 + 滑动窗口），当前手动维护（`MM_SOFT_RISK_V3.md:485` 自述） | 8h | 消除换月人工操作风险 |

**Phase D 合计**：季度规划

---

## 六、配置示例完整版

```yaml
# coordinator.yaml - modules 节点新增
modules:

  # ============================================================
  # 套利分级平仓配置 (v2.0)
  # ------------------------------------------------------------
  # 设计哲学: CLOSE 保持 B-3 抑制 (MM maker 消耗, 赚 spread + 免 fee)
  #           STOP_LOSS 立即 taker (止损成本 << 继续亏损)
  #           TIMEOUT 走 maker (挂 mid, 超时升级)
  # 灰度路径: C1(stop_loss) → C2(timeout_exit); CLOSE 永不解禁
  # ============================================================
  arb_close:
    enabled: false                      # 总开关 (false = 纯 B-3)

    allow_signals:
      close_long: false                 # v2.0: 永不解禁 (B-3 特性)
      close_short: false                # v2.0: 永不解禁
      timeout_exit: false               # C2 阶段启用
      stop_loss: false                  # C1 阶段启用 (最高优先级)

    # --------------------------------------------------------
    # 分级执行策略 (v2.0 核心)
    # --------------------------------------------------------
    stop_loss_policy:
      order_flag: 1                     # 0=GFD 1=FAK 2=FOK
      price_offset_ticks: 0             # 对手价
      timeout_ms: 1000                  # 极短 (止损紧急)
      upgrade_to_taker: false           # 已是 taker

    timeout_policy:
      order_flag: 0                     # GFD
      price_offset_ticks: 0             # 挂 mid 价
      timeout_ms: 30000                 # 30s 窗口
      upgrade_to_taker: true            # 超时升级对手价

    max_close_size_pct: 0.5             # 单次平仓量上限 (vs spread_position)
    close_in_flight_timeout_ms: 5000    # 平仓 in_flight 超时
    oversold_protection: true           # 过冲保险丝 (B5 事后)
    overshoot_cooldown_ms: 3600000      # 触发后 pair 冷却 1h
    intent_broadcast: true              # 与 Coordinator 协同 (B1+B2)
```

**spread_arbitrage.yaml 补充**（A9/A10 相关）：
```yaml
spread_arbitrage:
  minProfitThresholdTicks: 1.0    # A10: 开仓信号最低利润门槛
                                  # 语义: ≥ 2×(taker_spread+fee) - maker_rebate (见 §4.4)

  # A9: 对齐代码加载键名 (二选一方案)
  # 方案 a (推荐): 改代码读 yaml 现有键名
  statistical:
    meanReversion:
      entryZThreshold: 2.0        # 代码改读 entryZThreshold
      stopLossZ: 3.0              # 代码改读 stopLossZ (同时接线到 stop_loss_z)
      addSafetyRatio: 0.75
    pairsTrading:
      lookbackWindow: 100
      entryZThreshold: 2.0
    trendFollowing:
      stopLossPct: 0.02
      maxTrendBars: 50
```

---

## 七、回测验证方案（WtBtRunner 加速）

### 7.1 回测能力确认（事实核查）

- 项目内无回测入口（CMake 只产 SHARED 库，`CMakeLists.txt:81`）
- **仓库级 `src/WtBtRunner` 已适配 UFT 接口**：`UftMocker.cpp:592-618` 已 override `stra_quote`（stra_buy+stra_sell 模拟）
- 配置感知：`coordinator.yaml:22` `useAsyncArbThread: true # 实盘=true, 回测=false`
- 已有调参报告先例：`MM_V3_TUNING_RESULTS.md`（含 fee/trades 统计）

### 7.2 验证目标

对比 **纯 B-3（现状）** vs **v2.0 分级（C1/C2）** vs ~~v1.0 全解禁~~（仅回测参考，不上实盘）：
- PnL / Sharpe / MaxDrawdown（**重点：趋势日 maxDD**，C1 核心收益）
- 平仓成本分解（maker 消耗占比 vs taker 成本，验证 §4.2 模型）
- B5 保险丝触发次数（应为 0）
- 事前 clamp 触发次数（B3 第二层）
- STOP_LOSS 误触发率（< 5%）

### 7.3 数据集要求

- 至少 **3 个月历史 tick 数据**
- 必须包含：震荡市（z 频繁穿越 exit_z）、趋势市（z 持续走扩触发 stop_loss_z）、极端波动日（政策日/移仓日）

### 7.4 测试矩阵

| 测试维度 | 纯 B-3 baseline | v2.0-C1 (+STOP_LOSS) | v2.0-C2 (+TIMEOUT) | v1.0 全解禁(参考) |
|---------|----------------|---------------------|-------------------|------------------|
| 震荡日 PnL | 基准 | ≈持平 | ≈持平 | **预期下降**（CLOSE 走 taker 侵蚀利润） |
| 趋势日 maxDD | 基准（大） | **显著改善** | 进一步改善 | 改善但成本更高 |
| 平仓成本/单位 | -30 元（全 maker） | -30 元（CLOSE 仍 maker） | ≈-25 元 | +42 元（全 taker） |
| B5 触发 | N/A | 0 | 0 | ≥1 风险高 |
| 持仓时间 | 长（死扛 EOD） | 略降 | **显著缩短** | 最短 |

### 7.5 告警阈值

| 告警 | 阈值 | 应对 |
|------|------|------|
| B5 保险丝触发 | > 1 次/日 | 立即 `arb_close.enabled=false` 回滚 |
| 单 pair 过冲量 | > 2 手 | 立即回滚 |
| STOP_LOSS 误触发率 | > 5% | 上调 stop_loss_z 或回滚 C1 |
| TIMEOUT maker 成交率 | < 50% | 调 price_offset_ticks=-1 |

### 7.6 加速流程

1. Phase A 完成后跑纯 B-3 baseline（验证无回归）
2. Phase B+C0 完成后跑 baseline 复测（验证改造无回归）
3. C1 配置回测 3 天 → 小资金实盘 3 天 → 达标进入 C2
4. C2 配置回测 1 周 → 实盘 1 周 → 完成

---

## 八、风险与回滚

### 8.1 风险矩阵

| 场景 | 触发条件 | 缓解机制 | 回滚动作 |
|------|---------|---------|----------|
| 过冲（双重消耗） | STOP_LOSS 单 + MM skew 叠加 | B2 同侧抑制 + B3 事前 clamp + B5 事后保险丝 | `enabled=false` |
| STOP_LOSS 方向错判 | spread_position==0 时推导失效 | **A5 已修**（==0 直接降级 NONE） | - |
| 止损单卡死 | FAK 未成交（流动性真空） | 1000ms 超时 + B4 in_flight 清理 + 现有 60s 兜底 | 调大 timeout_ms |
| **涨跌停时平仓失败**（v2.0 新增） | 锁板时对手价无流动性 | **涨跌停识别跳过**（见下） | - |
| 残腿 hedge 失败 | STP 拦截或 broker 拒单 | A2/A4 修复 + D3 STP fallback | - |
| 多 pair 级联误撤 | cancelAllBySource 误伤 | A7 cancelByPair | - |
| 一合约多 pair 干扰 | rb2505 属 rb-hc 和 rb-i | B6 any-match 聚合 | - |
| 误触发 STOP_LOSS | z 短暂穿越阈值 | 误触发率监控（<5%）+ 成本可控（42 元/单位） | 上调 stop_loss_z |

**涨跌停识别（v2.0 新增，补充 MM 侧已有防护 `StrategyCoordinator.cpp:1002-1028` 到 ARB 侧）**：
```cpp
// B-3 门通过后, executeSignal 内
bool ArbExecutionBridge::isLimitLocked(const std::string& code, bool need_sell) {
    // need_sell=true (平多): 跌停锁板则卖不出
    // need_sell=false (平空): 涨停锁板则买不进
    // 锁板 → 信号降级 NONE + 告警 (等开板或 EOD closeout)
}
```

### 8.2 一键回滚

```yaml
arb_close:
  enabled: false    # 立即恢复纯 B-3 行为
```

回滚验证清单：
- [ ] applyB3Gate 恢复无条件抑制全部 5 类信号
- [ ] arb 线程不再广播 intent
- [ ] Coordinator 不再查询 hasActiveCloseIntent
- [ ] B5 事后保险丝仍记录但不动作

---

## 九、三方对比：纯 B-3 vs v2.0 分级 vs v1.0 全解禁

| 维度 | 纯 B-3（现状） | **v2.0 分级（本方案）** | v1.0 全解禁 |
|------|--------------|------------------------|------------|
| **CLOSE 退出** | MM maker 消耗（最优） | MM maker 消耗（最优） | taker（年成本高 ~180 万） |
| **STOP_LOSS** | ❌ 完全失效（P0 缺陷） | ✅ FAK 立即退出 | ✅ FAK 立即退出 |
| **TIMEOUT** | ❌ 死扛到 EOD | ✅ maker 挂 mid，30s 升级 | taker 立即 |
| **趋势日 maxDD** | 大（无止损） | **显著改善** | 改善 |
| **平仓成本** | 最优（全 maker） | 接近最优（CLOSE 仍 maker） | 最差（全 taker） |
| **过冲风险** | 0 | 低（三层防御） | 中 |
| **MM 报价义务** | 不受影响 | 不受影响（仅同侧短暂抑制） | 受影响（enhancer 全面介入） |
| **复杂度** | 极简 | 中 | 中高 |
| **可回滚性** | N/A | 一键回纯 B-3 | 一键回纯 B-3 |

---

## 十、实施时间表（回测加速版）

| 阶段 | 工作量 | 时序 |
|------|--------|------|
| Phase A 开发（A1-A10） | 1.5 工作日 | 第 1 周 |
| Phase A 回测验证（baseline 无回归） | 0.5 工作日 | 第 1 周 |
| **Phase A PR + 合并** | - | **第 1 周末** |
| Phase B 开发（B1-B6） | 2 工作日 | 第 2 周 |
| Phase C0 配置框架 | 0.5 工作日 | 第 2 周末 |
| **Phase B+C0 PR + 合并** | - | **第 2 周末** |
| **C1 回测 3 天**（趋势日 maxDD 验证） | - | 第 3 周 |
| **C1 小资金实盘 3 天** | - | 第 3 周 |
| **C2 回测 1 周 + 实盘 1 周** | - | 第 4 周 |
| Phase D 长期规划 | 季度 | - |

**总工作量**：约 4.5 工作日开发 + 2 周灰度（v1.0 的 6 周压缩至 2-3 周）。

---

## 十一、关键代码位置速查（实施时参考）

### 11.1 B-3 门相关（已核实行号）

| 内容 | 位置 |
|------|------|
| B-3 门抑制逻辑 | `SpreadArbitrageManager.cpp:761-773` |
| applyB3Gate 完整实现 | `SpreadArbitrageManager.cpp:742-904` |
| computeIntent 滞回逻辑（死代码） | `SpreadArbitrageManager.cpp:724-740` |
| in_flight_qty 硬编码 `* 2.0` | `SpreadArbitrageManager.cpp:895` |
| yaml leg2_ratio 硬编码 | `SpreadArbitrageManager.cpp:146-147` |
| in_flight 超时清理 | `SpreadArbitrageManager.cpp:822-833` |
| in_flight 双发抑制 | `SpreadArbitrageManager.cpp:835-840` |
| onArbOrderFilled 递减 | `SpreadArbitrageManager.cpp:914-933` |

### 11.2 executeSignal 与残腿

| 内容 | 位置 |
|------|------|
| executeSignal 主入口 | `AsyncArbitrageExecutor.cpp:302-532` |
| STOP_LOSS 方向推导 bug（==0 默认卖 leg1） | `AsyncArbitrageExecutor.cpp:316-323` |
| tagOrderPair / consumePairTag | `AsyncArbitrageExecutor.cpp:165-182` |
| orphan hedge 三档超时 | `AsyncArbitrageExecutor.cpp:538-644` |
| 残腿 hedge on_trade 触发（FAK+对手价，已核实） | `ArbExecutionBridge.cpp:267-295`（提交于 :288-290） |

### 11.3 持仓与协同

| 内容 | 位置 |
|------|------|
| Portfolio SSOT 单账本（无 source 字段，已核实） | `FutuPortfolio.h:57-174` |
| getTotalDelta 合并求和 | `FutuPortfolio.h:270-276` |
| refreshPositionsFromPortfolio | `SpreadArbitrageManager.cpp:562-572` |
| computeDerivedSpread (arb 线程读) | `SpreadArbitrageManager.cpp:681-722` |
| **一合约多 pair 映射（1:N，已核实）** | `SpreadCalculator.h:285` `_contract_to_pairs`；查询 `SpreadCalculator.cpp:550-555` |
| Coordinator 注入 total_delta | `StrategyCoordinator.cpp:553` |
| Coordinator processQuoting | `StrategyCoordinator.cpp:870-942` |
| MM 侧涨跌停三级防护 | `StrategyCoordinator.cpp:1002-1028` |

### 11.4 死代码（待激活/修复）

| 内容 | 位置 | 激活方式 |
|------|------|---------|
| MarketMakingEnhancer（内部链活，外部入口死） | `MarketMakingEnhancer.cpp:37-106`；实例化 `SpreadArbitrageManager.cpp:43` | B6 接线 |
| getQuotingAdjustment（真·无调用者） | `SpreadArbitrageManager.cpp:484` | B6 接线 |
| shouldPauseQuoting（any-match 语义可参考） | `SpreadArbitrageManager.cpp:525-560` | B6 参考 |
| UnifiedOrderTracker.trackArbOrder | `UnifiedOrderTracker.h:333-338` | A8 接线 |
| **setMinProfitThreshold（无调用者）** | `AsyncArbitrageExecutor.h:342`；消费点 `:451` | A10 接线 |
| onSpreadTrade（双计风险） | `UftFutuMmStrategy.cpp:1916-1989` | **删除**（不激活） |

### 11.5 A9 yaml 键错配详情（v2.0 新增）

| yaml 键（spread_arbitrage.yaml） | 代码加载键（Manager.cpp:118-131） | 状态 |
|--------------------------------|----------------------------------|------|
| `meanReversion.entryZThreshold` | `entryThreshold` | ❌ 错配 |
| `meanReversion.stopLossZ` | （无加载） | ❌ 死配置 |
| `meanReversion.addSafetyRatio` | （无加载） | ❌ 死配置 |
| `pairsTrading.lookbackWindow` | `spreadWindow` | ❌ 错配 |
| `pairsTrading.entryZThreshold` | `entryThreshold` | ❌ 错配 |
| `trendFollowing.stopLossPct` | （无加载） | ❌ 死配置 |
| `trendFollowing.maxTrendBars` | `maxTrendBars`（pair 级已接线 :154） | ⚠️ 全局段未接线 |

### 11.6 各策略 exit 信号生成位置

| 策略 | 文件:行 | 信号优先级 |
|------|---------|-----------|
| MeanReversion | `MeanReversionStrategy.cpp:60-134` | STOP_LOSS > TIMEOUT > CLOSE > 加仓 |
| PairsTrading | `PairsTradingStrategy.cpp:280-328` | STOP_LOSS > TIMEOUT > CLOSE |
| TrendFollowing | `TrendFollowingStrategy.cpp:180-250` | STOP_LOSS > 趋势反转 CLOSE > 趋势耗尽 CLOSE |
| StatisticalArb | `StatisticalArbStrategy.cpp:270-320` | STOP_LOSS > TIMEOUT > CLOSE |

---

## 十二、待讨论的开放问题

### 12.1 ~~跨线程实时读 Portfolio~~（已解决）

v2.0 采用 B3 双层校验：arb 线程粗判（读 _pair_states，接受 1-tick 滞后）+ 主线程精判（下单前读 Portfolio SSOT，无 data race）。无开放问题。

### 12.2 onSpreadTrade 死代码处理（G7）

`UftFutuMmStrategy.cpp:1916-1989` 内部调用 `_portfolio->updatePosition`（line 1963），误接线会导致 arb 成交双计。

**建议**：直接删除该函数，文档记录历史用途。

### 12.3 平今/平昨费率差异对 STOP_LOSS 成本的影响

**背景**：框架 actpolicy 自动拆分开平（`StrategyCoordinator.cpp:1221` 注释），`OrderRouter::submitExitLong/Short` 有 `isToday` 参数但无费率逻辑。做市商豁免通常仅覆盖部分情形（如平今豁免、平昨不豁免）。

**问题**：STOP_LOSS 平仓若涉及昨仓，手续费可能高于 §4.2 估算。

**建议**：实施时确认做市商协议豁免范围；若平昨不豁免，STOP_LOSS 成本上调但仍 << 继续亏损（§4.3 论证不变）。**不阻塞 C1 上线，但需在成本监控中记录实际费率。**

### 12.4 一合约多 pair 的 zscore 聚合策略

B6 采用 `|z| 最大者`。备选：持仓加权平均。

**建议**：初版用 `|z| max`（保守，响应最强信号）；实盘观察后决定是否改加权。**不阻塞实施。**

### 12.5 TIMEOUT maker 单的 mid 价定义

双腿 mid 价 = `(bid1+ask1)/2`。但套利 TIMEOUT 需两腿同时成交，单腿 mid 挂单可能导致一腿成交一腿挂单（残腿）。

**建议**：TIMEOUT 单复用现有 orphan leg 处理机制（`AsyncArbitrageExecutor.cpp:538-644` 三档超时），30s 内单腿未成交 → 升级对手价 + 残腿 hedge。**实施时验证该路径对 GFD 单的兼容性。**

---

## 十四、分策略影响分析（趋势 vs 均值回归，v2.1）

> 实施前评审补充：v2.0 分级平仓对两类套利策略的影响**不对称**，根因是 CLOSE 信号语义不同。

### 14.1 退出信号语义对比（代码实证）

| 信号 | 均值回归（`MeanReversionStrategy.cpp:67-124`） | 趋势跟踪（`TrendFollowingStrategy.cpp:188-247`） |
|------|-----------------------------------------------|--------------------------------------------------|
| **CLOSE** | z 回归到 `±exit_z×0.3`（**≈0，价差已回归，利润已锁定**）:87-99 | ①趋势反转 `is_trend_reversal`（**z 未回归，利润暴露中**）:211-227 ②趋势耗尽 `bars > max_trend_bars`（语义≈超时）:229-246 |
| **STOP_LOSS** | `\|z\| > stop_loss_z`（回归失败，z 继续走扩）:70-76 | **价格反向 stop_loss_pct**（`pnl_pct < -2%`）:191-208 |
| **TIMEOUT** | 有（`checkTimeout`，不收敛退出）:78-84 | **无**（趋势耗尽发的是 CLOSE，不是 TIMEOUT） |
| **加仓** | 有（z 安全区内 0.5 倍加仓）:105-124 | 无 |

**关键**：均值回归的 CLOSE 是"利润实现"（z≈0）；趋势的 CLOSE 是"逃顶/逃底"（z 仍在高位，价差正快速反向）。**两者对平仓延迟的容忍度完全不同**。

### 14.2 对均值回归：几乎纯收益

1. **CLOSE 保持 B-3 = 零代价 + 成本最优**：CLOSE 触发时 z≈0，两腿对冲敞口无方向风险；MM per-contract skew 消耗方向天然匹配（leg1 +N → skew 偏卖 leg1；leg2 -N → 偏买 leg2），消耗慢无利润可回吐
2. **STOP_LOSS 解禁修复加仓场景致命漏洞**：加仓后仓位 1.5x，B-3 下 z 破 stop_loss_z 死扛到 EOD；C1 后 FAK 立即退出全部仓位，亏损锁定。**这是 C1 收益最大的场景**
3. **TIMEOUT maker 兜底**：不收敛仓位零成本退出（-30 元/单位 vs taker +42 元）
4. **净评估**：趋势日 maxDD 显著改善 + 平仓成本不变 + 无新增风险

### 14.3 对趋势跟踪：主要收益 + 1 个残留风险

1. **STOP_LOSS 解禁 = 核心保护**：趋势策略亏损主因是反转，B-3 下价格反向 2% 的止损完全失效 = 裸奔。C1 后 FAK 立即退出，收益重大
2. **残留风险：趋势反转 CLOSE 被抑制 → 温和反转利润回吐**：
   ```
   多 spread 趋势 +3σ → 反转信号 (:211) → B-3 抑制 → MM 消耗需 T 时间
     Δ < 2%: 利润温和回吐, 不触发止损 → 残留损失 (无保护)
     Δ ≥ 2%: STOP_LOSS 触发 → taker 退出 (已兜底)
   ```
   与均值回归的本质区别：趋势 CLOSE 时 z 可能在 +2σ，**持仓利润暴露中**；MM 随机消耗在温和反转（0~2% 回吐区间）场景会真实损失利润
3. **趋势耗尽 CLOSE 语义错配**：`bars > max_trend_bars` 本质是超时，却发 CLOSE → v2.0 中被永久抑制，实际应走 TIMEOUT maker 路径
4. **成本论证强度减弱**：v2.0"CLOSE 保持 B-3"基于"高频信号成本累积"；趋势 CLOSE 是低频信号（每趋势周期 1 次），该论证对趋势策略强度减弱

### 14.4 改进建议（v2.1，已合入实施计划）

| # | 建议 | 位置 | 合入阶段 | 工作量 |
|---|------|------|---------|--------|
| 1 | **趋势耗尽信号重分类** `CLOSE_*` → `TIMEOUT_EXIT` | `TrendFollowingStrategy.cpp:234,241` | **C2** | 1 行/处 |
| 2 | **per-strategy override 结构**（默认空，趋势可单独解禁 CLOSE 走 timeout_policy maker-first） | `ArbCloseConfig.strategy_overrides` | **C0**（仅预留结构） | 1h |
| 3 | 趋势反转回吐数据 > taker 成本 2 倍时启用 override；仍不足则评估升级 STOP_LOSS 语义 | 配置决策 | C2 观察期后 | 0 |

### 14.5 对 PairsTrading / StatisticalArb 的推论

两者退出语义与均值回归同构（z 回归 CLOSE + z 止损 + 超时）→ **影响与均值回归一致：几乎纯收益，无残留风险**。

### 14.6 对实施计划的影响

| 问题 | 结论 |
|------|------|
| 阻塞 Phase A/B？ | 否 |
| 改变 C0？ | 是，微调——`strategy_overrides` 结构预留（+1h） |
| 改变 C1？ | 否，STOP_LOSS 对两类策略均纯收益 |
| 改变 C2？ | 是，合入趋势耗尽重分类（+0.5h） |
| 总评 | v2.0 对均值回归/pairs/statistical 零风险纯收益；对趋势策略重大收益 + 可控残留风险（2% 止损兜底 + v2.1 两个低成本出口） |

---

## 十三、附录

### 13.1 成本模型数学公式

```
===== 成本构成 =====
TakerCost_per_leg  = spread_cost + fee_taker + slippage
                   = 1 tick×mult + fee_taker + ε
MakerCost_per_leg  = -spread_capture + fee_maker
                   = -1 tick×mult + 0        (做市商豁免)

===== 往返净收益 =====
NetPnL = |Δz| × σ_spread × mult × Q - OpenCost - CloseCost

OpenCost (taker 两腿) = 2 × (1 tick×mult + fee_open)

CloseCost 分级:
  MM maker 消耗 (CLOSE, B-3):   -2 × 1 tick×mult          [负成本=收益]
  ARB maker (TIMEOUT):          -2 × 1 tick×mult × P_fill  [P_fill=maker成交率]
  ARB taker (STOP_LOSS):        +2 × (1 tick×mult + fee_close)

===== 正期望条件 =====
CLOSE (B-3):   |Δz|×σ_sp > 2×fee_open - 2×tick×mult
               → 阈值极低, exit_z=0.5 充分正期望

TIMEOUT:       同 CLOSE (maker 路径), 加资金占用机会成本

STOP_LOSS:     非正期望决策, 是减亏决策:
               执行 iff 浮亏 + TakerCloseCost < E[继续亏损 | 趋势延续]
               → z 已破 stop_loss_z 时近似恒真, 立即执行

===== minProfitThreshold (A10) =====
threshold_ticks ≥ [2×(1 + fee_open_ticks) - 2×1] = 2×fee_open_ticks
                ≈ 0.8 ticks (ag, fee≈0.5bp)
建议初值: 1.0 tick (安全裕度), 按品种配置
```

### 13.2 残腿覆盖矩阵（Phase A 修复前）

| 失败模式 | D1 | D2 | D3 | D4 | D5 | 完整覆盖 |
|---------|----|----|----|----|----|---------|
| leg2 队列 push 失败 | ✓ | | | | | ✅ |
| leg2 router 限速 | | ✓ | | ✓ | | ✅ |
| leg2 router STP 拦截 | | ✓ | | ✓ | | ✅ |
| **leg2 broker 拒单** | | | ✓(撤) | ✗ | △ | ❌ → A2 修复 |
| leg2 网络超时未回报 | | | | | ✓(60s) | △ |
| **leg1 partial + leg2 0** | | | | ✗ | △ | ❌ → A3 修复 |
| **leg1 满 + leg2 撤单** | | | | ✗ | △ | ❌ → A4 修复 |
| hedge 自身被 STP 拦 | | | | | | ❌ → D3 (Phase D) |

✓ = 即时处理，△ = 被动延迟处理（60s），✗ = 不处理

### 13.3 过冲保护机制层级（v2.0 实施后）

| 层级 | 机制 | 位置 | 强度 |
|------|------|------|------|
| L1 | B-3 门抑制 CLOSE（永久保留） | `SpreadArbitrageManager.cpp:765-773` | 绝对 |
| L2 | **B3 事前 clamp（下单前预测 sign-flip）** | `ArbExecutionBridge::processPendingOrders` | 事前 |
| L3 | B2 同侧 MM 报价抑制（STOP_LOSS/TIMEOUT 期间） | `StrategyCoordinator::processQuoting` | 协同 |
| L4 | B4 close_in_flight 防双发 | `SpreadArbitrageManager` | 同向同 pair |
| L5 | **B5 事后保险丝（sign-flip 检测 + COOLDOWN + EventNotifier 告警）** | `FutuPortfolio::onPositionUpdate` | 事后兜底 |
| L6 | MM skew 截断 half_spread | `SpreadOptimizer.cpp:141-145` | 单边 |
| L7 | signal_cooldown_ms | `SpreadArbitrageManager.cpp:108` | 2s 节流 |

### 13.4 监控告警通道（事实核查）

项目现有通道（无邮件/钉钉/webhook）：
1. `WTSLogger::warn/error` 日志
2. **EventNotifier（UDP 广播/MQ 模块）**：`FutuRiskMonitor.cpp:89-97` `broadcastAlert` 已实现，`src/WtUftCore/EventNotifier.h` 定义
3. 内存态 RiskAlert + 回调：`SpreadRiskManager.h:138,238`

**v2.0 决策**：B5 保险丝触发接 EventNotifier（复用现有通道，不引入新依赖）。外部告警系统（钉钉等）由运维侧消费 UDP/MQ 消息实现，不在本方案范围。

---

## 修订记录

| 日期 | 版本 | 内容 |
|------|------|------|
| 2026-07-19 | v1.0 | 初版：全解禁路径 |
| 2026-07-19 | **v2.0** | 重大修订：分级执行路径。依据用户 6 项决策确认（Q1-Q6）+ 2 组事实核查（13 项声明核查 + 7 项新发现）。核心变更：CLOSE 永不解禁（B-3 特性保留）、STOP_LOSS 提前 C1、TIMEOUT 走 maker、成本模型含 spread cost、Phase A 扩至 A10、B5 双层、B6 处理 1:N 映射、回测加速灰度压缩 |
| 2026-07-19 | **v2.1** | 新增 §十四 分策略影响分析（趋势 vs 均值回归）：CLOSE 语义不对称（利润实现 vs 逃顶）。C0 预留 `strategy_overrides` 结构（+1h）；C2 合入趋势耗尽重分类 `CLOSE_*`→`TIMEOUT_EXIT`（`TrendFollowingStrategy.cpp:234,241`，+0.5h）；趋势反转 override 留待 C2 观察期后数据决策 |
| 2026-07-19 | **v2.1+实施** | **Phase A (A1-A10) 全部实施完成，编译通过无新警告**。实施明细：A1 in_flight 按 (leg1_ratio+leg2_ratio) 计算 + yaml `ratio2` 键；A2 on_entrust 拒单补 `markLegRejected`；A3 `_arb_hedge_on_fill` 改 `map<pair_id, CloseHedgeState>` 分笔安全对冲（min(vol, 剩余未对冲量)，覆盖完毕才 erase）；A4 on_order isCanceled → `onLegCancelled`（撤对侧+标记+释放 in_flight）；A5 零持仓平仓信号降级 NONE；A6 补 WIDEN_SPREAD(×2.0)/REDUCE_SIZE(×1.5 近似)/FLATTEN_POSITION(撤单+停arb+anchor强平) 三风控分支 + `_risk_spread_mult` 注入 processQuoting；A7 `OrderRouter::cancelByPair`（`_oid_to_pair` 映射，含加仓组）替换 5 处 `cancelAllBySource(ARBITRAGE)`（HALT/FLATTEN 全局风控保留原接口）；A8 `trackArbOrder` 接线（录入后现有 recordOrderFill/untrack 链路自动生效）；A9 statistical 段键名对齐 + `_default_*` 变为 pair 级默认值来源 + `stop_loss_z` 接线；A10 `minProfitThresholdTicks` yaml→Manager config→`setMinProfitThreshold` 接线。spread_arbitrage.yaml 同步更新（minProfitThresholdTicks/ratio2/stopLossZ/exitZThreshold 示例） |
| 2026-07-19 | **v2.1+实施2** | **Phase B (B1-B6) + C0 配置框架全部实施完成，编译通过零新警告**。实施明细：**C0** `ArbCloseConfig`（Types.h，含 ExecutionPolicy/StrategyOverride）+ spread_arbitrage.yaml `arb_close` 节点加载 + applyB3Gate 平仓门控改造（默认 enabled=false 纯 B-3，行为不变）；**B1** `_active_close_intents` map（spin lock，arb 线程写/主线程读）+ set/clear/hasActiveCloseIntent/getArbCloseDirection（1:N any-match）；**B2** `Coordinator::setArbManager` + processQuoting §3.05 同侧抑制（arb 卖 leg→抑 MM bid，防反向重建库存+防自相成交）；**B3** 双层校验（applyB3Gate 第一层：零持仓降级+clamp 至 derived×max_close_size_pct；bridge 第二层：下单前读 Portfolio 事前过冲 clamp+零残留丢弃）；**B4** PairArbState `close_in_flight_qty/set_time` + onArbOrderFilled 优先扣 close + 超时强制清理（复用 _timed_out_pairs 撤单通道）；**B5** Portfolio 两入口（onPositionUpdate/updatePosition）sign-flip 检测 + `hasActiveCloseIntent` 条件（无 intent 的 MM 正常 flip 不报）+ `onOvershootDetected`（cooldown+`_overshoot_pairs` 轮询撤单+RiskAlert CRITICAL 告警）+ bridge popOvershootPairs 轮询；**B6** getPairZscore/getAggregateZscore（\|z\| max）+ getQuotingAdjustmentForLeg + processQuoting enhancer **观测模式**（仅 debug 日志，不注入 skew，C2 决策）；**执行策略** ArbOrderRequest 加 `order_flag/is_close`，executor 按 policy_for 设置（STOP_LOSS→FAK），bridge FAK 单对手价替换（portfolio bid1/ask1）。注入点：UftFutuMmStrategy `coordinator->setArbManager` + `portfolio->setArbManager`。**C1/C2 上线仅需改 yaml：`arb_close.enabled=true` + `allow_signals.stop_loss/timeout_exit=true`** |
| 2026-07-19 | **v2.1+实施3** | **风控模块完整修复（Phase R1-R3），编译通过零新警告**。修复 4 项致命 + 2 项严重问题：**F1** A6 三分支不可达（determineActionWithCategory 早返回拦截 breachCount 升级段）→ R2.3 重构单向 BLOCK 路径前先检查 breachCount≥flatten_threshold → FLATTEN 可达；R2.2 新增 `checkSoftLimits`（util 0.8/0.9 → WIDEN_SPREAD）在 hard check 前执行，WIDEN 从"breach 后响应"改为"breach 前预警"。**F2** EventNotifier 通道未接线 → R1.2 `UftFutuMmStrategy::setEventNotifier` 注入 RiskMonitor 直达（22 处 broadcastAlert 立即生效）。**F3** SpreadArbitrageManager alert_callback 未接线 → 方案 B 策略层转发：`setAlertCallback` 注册 `handleRiskAlert` lambda → `_event_notifier->notify("ARB_RISK", ...)`（解耦：ArbManager 不依赖 EventNotifier，保持模块边界）。**F4** updatePortfolioPnL 死代码 → R3.1 generateSignals 入口接线 Portfolio.getTotalUnrealizedPnL/getTotalPnL → `_current_drawdown` 从恒 0 变真实值 → STOP_LOSS alert 可达。**S1** BLOCK_SIDE 状态泄漏 → R2.7 BLOCK_SIDE_LONG/SHORT case 设 `qphase=RISK_HALTED`（统一恢复路径，此前 block 永久残留）。**M1** PAUSE_QUOTING 不同步 atomic → R2.6 补 `pauseQuoting()`。**D5** 删除 `REDUCE_SIZE`（做市最低报价数量要求）+ `BLOCK_CONTRACT_OPENING`（死代码），WIDEN_SPREAD 分级倍数（L1→×1.5, L2→×2.0）。**注入链路** WtUftRunner.initUftStrategies → UftStraContext.setEventNotifier → on_init dynamic_cast → setEventNotifier（覆盖 init 前后两时序）。配置：RateLimits 加 `position_warning_l1/l2`（yaml `positionWarningL1/L2`，默认 0.8/0.9） |
