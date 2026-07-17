# quantbox vs WtOptionCore: 风控与报单管理对比分析

## 概述

本文档对比原 quantbox/optiontrader (longbeach框架) 与迁移后的 WtOptionCore (HFT框架) 在风控和报单管理方面的差异，识别原项目中优秀但未迁移的功能，为后续增强提供依据。

---

## 一、风控模块差异

### 1.1 架构差异

原项目采用**三层风控架构**：

| 层级 | 原项目 quantbox | WtOptionCore |
|------|----------------|--------------|
| **策略层** | `RiskManager` + `PositionManager`(矩阵风险模型) + 6个`IOrderFilter` + `CtaRiskModel`(因子模型) | `RiskSignals`(ToxicitySignal/PnlLimitSignal) + `RiskLimits`结构体 + CTG内联检查 |
| **期权层** | `OptionRisk` + `OptionRiskData`(含`IPositionOffset`) | `OptionRisk` + `OptionRiskData`(含dirty flag) + `OptionExpiryGreeks` |
| **TD层** | `TdRiskControl`(20+限制) + `TdOptionsRiskControl`(short call/put) | 无对应层 |

WtOptionCore 缺失了策略层和TD层的绝大部分风控基础设施。

### 1.2 未迁移的风控功能

#### (1) PositionManager 矩阵风险模型 — **重要缺失**

原项目 `PositionManager` 使用 **NxN Eigen 矩阵**计算风险调整后头寸：

```
riskAdjustedPosition = Matrix × rawPositions
```

即每个合约的风险头寸是所有相关合约头寸的加权和。对于同标的不同月份的期权/期货，这能正确反映**跨合约相关性风险**。WtOptionCore 完全没有这个能力，各合约风险独立计算。

#### (2) CtaRiskModel 多因子风险模型 — **重要缺失**

原项目 `CtaRiskModel` 实现：
- **加权最小二乘法因子收益估计**：`β = (X'WX)⁻¹ X'Wy`，权重为 `1/specificRisk²`
- **协方差矩阵**验证（对称性）
- **因子暴露**：`loading × factorReturn`
- **Specific Risk / Total Risk** 查询

WtOptionCore 没有任何因子风险模型。

#### (3) 6个可组合的 IOrderFilter 链 — **重要缺失**

原项目有6个可插拔的订单过滤器，按链式执行：

| Filter | 功能 | WtOptionCore 对应 |
|--------|------|-------------------|
| `MaxOrderSizeFilter` | 胖手指限制（拒绝或截断） | OptionQuoteManager 无显式限制 |
| `MinSellPriceFilter` | 最低卖价限制 | 无 |
| `InstrMaxPositionFilter` | 3模式：拒绝/允许/截断到最大值 | 有 `check_potential_position`（仅拒绝） |
| `InstrMaxCancelFilter` | 软/硬限制，软限制允许减仓单 | 有 `max_cancels_allowed`（仅硬限制） |
| `MaxNewOrdersFilter` | 硬平仓/拒绝双模式+回调 | 有 `hard_flat_after_n_fills` + `reject_max_new_orders` |
| `MaxOpenOffsetFilter` | 开仓偏移限制（委托计算） | 无 |

原项目的 `InstrMaxCancelFilter` 在软限制时**允许减仓订单通过**（abs(final) < abs(current)），WtOptionCore 只能全量拒绝。

原项目的 `InstrMaxPositionFilter` 有 **MODIFY_TO_MAX** 模式——将订单截断到最大允许量而非直接拒绝，WtOptionCore 不支持。

#### (4) TdRiskControl 20+预交易限制 — **重要缺失**

原项目 TD 层有完整的预交易风控检查链：

**账户级**：exposure、position_gross、position_net、max_open_orders、loss、burst_limit(触发TD halt)、avg_orders_per_sec

**品种级**：symbol_exposure、symbol_position、symbol_shares、symbol_loss

**合约级**：order_size、order_value、potential_side_size、side_size、loss、exposure、position

**特殊检查**：clearly_erroneous_percent（价格偏离参考价百分比）、allowed_tifs、allowed_symbols、allowed_derivatives

**聚合检查**：按symbol和product维度的聚合头寸限制

WtOptionCore 仅有 `RiskLimits` 结构体（Delta/Gamma/Vega/DailyLoss）+ OptionQuoteManager 内联检查（max_position/max_cancels/hard_flat），覆盖面远不及原项目。

#### (5) TdOptionsRiskControl — **重要缺失**

- **Short call/put 限制**：按symbol聚合计算净short call和short put头寸
- **Position stacking**（`pushPosition/popPositions`）：临时"what-if"头寸模拟，不修改实际状态即可预检查
- **Per-symbol option grouping**：同一标的的所有期权按symbol聚合

#### (6) RiskManager 头寸名义价值跟踪 — **中等缺失**

- `getPositionNotionalValue()`：`position × price × multiplier`
- **Position delta threshold alerting**：风险调整后头寸变化超过阈值时触发回调
- **Multi-account support**：跨账户头寸跟踪

### 1.3 WtOptionCore 风控的新增功能（原项目没有的）

| 功能 | 说明 |
|------|------|
| `ToxicitySignal` | 连续逆向成交检测，自动升级 Widen->Panic |
| `PnlLimitSignal` | 日内亏损限制触发 Panic |
| 4级风险动作 | None->Widen->StopQuoting->Panic |
| Panic TPS boost | Panic模式下提升TPS以加速减仓 |
| 7因子优先级排序 | isBest、crossing、delta urgency、spread tightness等 |
| Drop tracking + retry | TPS超限时保存落单，下周期重试 |

---

## 二、报单管理模块差异

### 2.1 架构差异

| 维度 | 原项目 quantbox | WtOptionCore |
|------|----------------|--------------|
| **Order对象** | `Order`(843行) + 可扩展InfoSlot系统(type erasure) | `BaseOrder`(67行) + 固定字段 |
| **订单管理器** | `DefaultOrderManager`(1607行) + 4个变体 | `OptionQuoteManager`(316行) + `OrderManager`(967行) |
| **中央注册表** | `IssuedOrderTracker`(1195行) | 无 |
| **状态机** | `TdOrderState`(4状态) + `TdOrderCancelState`(5状态) | `BaseOrder`(7状态，简单枚举) |
| **头寸跟踪** | `PositionTracker`+`PortfolioPositionTracker`+`OrderPositionTracker` | OQM内联 `m_position` |
| **PnL** | `PnLTracker`(FIFO匹配deque) | `PnlTracker`(简单增量) |
| **Fill监控** | `FillPriceChecker`+`FillDequeLedger`+`MarkerToMarket` | `ToxicitySignal`(不同维度) |

### 2.2 未迁移的报单管理功能

#### (1) PositionOffsetManager — **关键缺失**

原项目的 `PositionOffsetManager` 处理 **SHFE 平今/平昨/开仓** 区分：

- `getNumCloseableToday(dir)` / `getNumCloseablePrev(dir)`：可平今/昨量
- `getOrderBreakdown(dir, size)`：将订单拆分为 `(CLOSETODAY, qty) + (CLOSE, qty) + (OPEN, qty)` 三段
- `getRecommendation(dir, size)`：推荐开平仓标志

这对中国期货期权做市**至关重要**——SHFE要求明确指定平今还是平昨，不同方向手续费不同。WtOptionCore 完全没有这个功能，所有订单都当作 OPEN 处理。

#### (2) LocalPositionOffsetManager — **重要缺失**

多策略共享同一账户时，每个策略**claim一个比例**的可平仓位：

```cpp
fraction_to_ignore = 1 - claim_fraction;
closeable = global_closeable - ignored_closeable;
```

WtOptionCore 无此机制，多策略会争抢可平仓位。

#### (3) 保证金计算 — **重要缺失**

原项目 `DefaultOrderManager::getDesiredMargin()` 计算期望保证金：
```
margin = markings + existing_orders + desired_orders
if margin > max_margin: reject/cancel outer orders
```

WtOptionCore 无保证金检查。

#### (4) 头寸不一致检测 — **重要缺失**

原项目 `DefaultOrderManager::onFill()` 比较：
- 内部头寸（check_position + fill）
- 外部头寸（position provider）
- 不一致 -> **禁用交易 + 告警**

WtOptionCore 无此安全检查。如果成交回报丢失或重复，不会被检测到。

#### (5) 拒绝重试机制 — **中等缺失**

原项目：
- 订单拒绝 -> 400ms后重试 `retryUpdateOrders()`
- 撤单拒绝 -> 重新调度撤单

WtOptionCore 无重试逻辑，拒绝后丢弃。

#### (6) FillPriceChecker — **中等缺失**

原项目监控成交价与发单价偏离：
- 0.25% -> Warning
- 0.5% -> Panic

WtOptionCore 的 `ToxicitySignal` 检测的是**连续逆向成交**（方向维度），不是**价格偏离**（价格维度）。两者互补但不可替代。

#### (7) FillDequeLedger + PnLTracker FIFO匹配 — **中等缺失**

原项目用双端队列实现**FIFO匹配**：
- 买单入bid deque，卖单入ask deque
- 对手方deque头部匹配 -> 实现已现PnL
- 支持二进制文件持久化（save/load）

WtOptionCore 的 PnlTracker 是简单增量式（`realized += mult × pos × (fillpx - lastfillpx)`），不追踪每笔成交的匹配关系。

#### (8) MarkerToMarket — **中等缺失**

原项目跟踪**加权平均入场价**：
- 增仓：`weightedEntryPrice += fillpx × fillqty`
- 减仓：`weightedEntryPrice -= avgEntryPrice × fillqty`
- 平仓：重置为0

然后计算 mark-to-market PnL。

WtOptionCore 的 `OptionOrder::getRealizedPnL()` 用 theoPrice 而非加权平均入场价。

#### (9) OrderHarmonizer — **中等缺失**

原项目用**双指针归并算法**比较 desired vs issued 订单：
- 按 aggressiveness 排序
- 三种情况：匹配（跳过）、issued更激进（撤单）、desired更激进（发单）
- 增量更新

WtOptionCore 的 `OptionQuoteManager::updateOrders()` 是更简单的全量对比（check_markets），非增量。

#### (10) IssuedOrderTracker — **重要缺失**

原项目中央订单注册表：
- **IOTTrader装饰器**：拦截 sendOrder/sendCancel/sendQuote，先过filter再转发
- **SHFE DONE-before-FILL 竞态处理**：DONE回报先于FILL到达时设置pending position
- **Order-not-found fill 处理**：创建dummy order + PANIC告警
- **Overfill检测**：remaining_size < fill_qty 时告警

WtOptionCore 无中央注册表，订单状态由 OQM 各自管理。

#### (11) TdOrderState / TdOrderCancelState 状态机 — **中等缺失**

原项目有**经典GoF状态模式**：
- 订单状态：New->Transit->Open->Done
- 撤单状态：None->Pending->Transit->Confirmed/Reject
- 每个状态封装合法事件处理
- 撤单拒绝后可重试（Reject->Pending）

WtOptionCore 的 `BaseOrder` 是简单枚举状态机，无状态封装。

#### (12) 交易所特定报价API — **中等缺失**

原项目 `QuoteOrderManager` 针对不同交易所：
- SHFE/CZCE/INE：支持"顶单"（top order）模式
- DCE/CFFEX：使用报价API（paired bid+ask）
- 不同的 quote_id 约定（`quote_id-1` = bid, `quote_id` = ask）

WtOptionCore 统一使用 WT 的 `stra_quote`，不区分交易所差异。

#### (13) OrderCounter 内存映射持久化 — **低缺失**

原项目订单计数器持久化到**内存映射文件**，进程重启后不丢失。WtOptionCore 计数器仅在内存中。

#### (14) Cautious flipping mode — **低缺失**

头寸即将翻面（从多到空或反之）时，额外谨慎处理。

#### (15) Scale factor — **低缺失**

运行时可调整的订单大小缩放因子。

#### (16) PortfolioPositionTracker — **中等缺失**

- 组合服务器头寸集成
- **过期检测**（10秒无更新 -> 告警 + 回退到本地跟踪）
- **头寸钳制**（`|abs - eff| > maxPosition` 时钳制）

#### (17) Position broadcast reconciliation — **中等缺失**

原项目 `PositionTracker` 的**乐观协调with回滚**：
- 收到广播头寸与本地不一致时调整
- 2秒内有fill到达 -> **撤销调整**（undo）
- 两种模式：立即接受（with undo window）vs 延迟接受（2秒静默期后）

---

## 三、总结

### 按重要性排序的未迁移功能

**P0（关键，影响实盘可用性）**：
1. PositionOffsetManager（平今/平昨/开仓区分）
2. 头寸不一致检测（安全护栏）
3. TdRiskControl 预交易限制链（20+限制）
4. 保证金检查

**P1（重要，影响风控完整性）**：
5. LocalPositionOffsetManager（多策略claim）
6. PositionManager 矩阵风险模型
7. FillPriceChecker（成交价偏离监控）
8. 拒绝重试机制
9. IssuedOrderTracker（中央注册表+竞态处理）
10. TdOptionsRiskControl（short call/put限制）

**P2（中等，提升健壮性）**：
11. IOrderFilter 可组合过滤器链
12. FillDequeLedger + PnLTracker FIFO匹配
13. MarkerToMarket（加权平均入场价）
14. PortfolioPositionTracker（组合服务器+过期检测）
15. Position broadcast reconciliation（乐观协调）
16. OrderHarmonizer（增量归并）
17. TdOrderState/TdOrderCancelState 状态模式
18. OrderStatisticsCollector（生命周期统计）

**P3（低，锦上添花）**：
19. CtaRiskModel 因子风险模型
20. 内存映射持久化
21. Cautious flipping
22. Scale factor
23. 交易所特定报价API

### WtOptionCore 的优势（原项目没有的）

1. **Lock-free SPSC队列** — 原项目用mutex+deque
2. **ToxicitySignal** — 连续逆向成交检测（方向维度）
3. **4级风险动作升级**（None->Widen->StopQuoting->Panic）
4. **7因子优先级排序**（isBest、crossing、delta urgency等）
5. **Panic TPS boost** — 紧急时加速撤单
6. **Drop tracking + retry** — TPS超限不丢弃
7. **Dirty flag增量Greeks** — 避免无变化的重复计算
8. **Underlying-driven compute** — 标的驱动定价，减少不必要计算
