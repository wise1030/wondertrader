# WtOptionCore 期权做市策略深度诊断与完善方案

> **诊断日期**：2026-10-18
> **对象**：`src/WtOptionCore`（约 136 个文件、24.7k 行），HFT 策略插件 `OptionStraFact.OptionMM`
> **方法**：策略链路逐段阅读 + 现有 2026-08-24 诊断逐项复核 + 与 WtFutuCore 生产级能力对照
> **约束**：诊断阶段只读；2026-10-18 阶段 0 部分已按确认实施，见 v1.1 与 §0.3；仍未实施的项在 §8 标注
> **结论口径**：`P0` 可直接导致错误持仓/挂单/资金风险；`P1` 高风险或明显能力缺口；`P2` 架构/可维护性
> **v1.2（2026-10-18）**：阶段 0 完成；阶段 1 大部分完成（热参数/TPS/ConfigValidator/撤单/到期/队列）；阶段 2 首批完成：Scanner 验收、`OptionScanCoordinator`/`OptionHedgeCoordinator`/`OptionExpiryLifecycle` 拆分、`StressAnalyzer` 情景压力。`test_scanners`/`test_stress`/`test_hedge_coordinator`/`test_enhancements`/`test_ctg_ranking` 全过。下文保留诊断时的状态描述，实施清单以本节与 §0.3 为准。

---

## 0. 结论摘要

### 0.1 总评

WtOptionCore 的**定价与报价骨架是完整的**：Black-76/Black-Scholes、GVV/线性/幂波动率曲线、Greeks 聚合、组合风险、报价 diffing、TPS、撤单节流、保证金估算、涨跌停护栏、FIFO PnL 都已落地，异步双队列（MD SPSC + 交易 mutex）也是正确方向。它不是一个“玩具策略”。

但它距离“可放心实盘”仍有明显差距，核心矛盾不是定价数学，而是 **交易生命周期的收尾能力**：

- 组合单（combo）没有真正的两腿状态机；
- Delta 对冲手数单位错配且无在途单抑制；
- 撤单 ack 丢失会把报价侧永久卡死；
- 到期/行权没有强制平仓或结算，期权到期后仍可能留在网格里；
- 热参数、配置校验、测试与可观测性都弱于 WtFutuCore。

| 维度 | 评分（1–5） | 判断 |
|---|---:|---|
| 定价/波动率/Greeks | 4.0 | 数学内核扎实，但 IV/NaN/theta 边界有洞 |
| 报价与订单管理 | 3.5 | diffing/节流/wait-for-cancels 完整，缺撤单超时升级 |
| 组合单 | 1.5 | 类存在，状态机不完整，可产生裸敞口 |
| Delta 对冲 | 2.0 | 决策器存在，手数错配且无在途控制 |
| 到期/行权/收盘 | 1.5 | 只有模拟/报表，无 live 结算与强制平仓 |
| 风控/保证金 | 3.5 | 限仓/Greeks/保证金/涨跌停齐全，缺 SPAN/情景压力 |
| 热参数/配置 | 2.0 | 少量 sync_param；校验只覆盖硬编码键 |
| 架构与可测试性 | 2.5 | 组件化方向正确，但 2.4k 行上帝类 + 仅 2 个测试程序 |
| **综合** | **2.7** | 骨架好、细节欠账多，**当前不建议直接实盘** |

### 0.2 四个必须先处理的 P0

| # | 问题 | 影响 |
|---|---|---|
| 1 | Combo leg2 发出即 `done`，残腿无补/撤 | leg1 成交、leg2 拒单 → 裸敞口 |
| 2 | Delta 对冲 `minHedgeQty` 恒 1.0，而 `excess` 已乘合约乘数 | 手数被放大（IO 约 300×），反复叠单 |
| 3 | `OptionQuoteManager::cancelPending` 无超时/重试 | 撤单 ack 丢失 → 该侧永久停止报价 |
| 4 | 到期日只做“减仓侧报价”，无强制平仓/结算；过期合约不摘除 | 到期持仓、空头期权、结算 PnL 失控 |

### 0.3 本次已实施（2026-10-18，阶段 0 部分）

| 项 | 状态 | 说明 |
|---|---|---|
| Spread/Syn Combo 终态 | ✅ | 两腿/三腿 `filledQty >= desiredQty` 才完成；任一腿撤/拒会撤其余腿并 FAILED |
| Delta 对冲 `delta_per_lot` | ✅ | 缺省从对冲合约乘数解析；解析失败禁用并告警；在途 hedge 单不重复发 |
| OQM 撤单 ack 看门狗 | ✅ | `cancel_ack_timeout_ms` + `cancel_max_retries`；耗尽后 cancelAll 清扫 + 冻结，终态回调解冻 |
| 过期/收盘 | ✅（部分） | 过期合约禁报；`enable_auto_close` 不再受 delta 区间门控；session_end 显式 cancelAll + gross 告警；**仍未做**到期强平/结算/移仓 |
| theta / NaN | ✅ | theta 参数语义修正；非有限 `theoVol` 不再回落 1% |
| 构建可移植 | ✅ | `-march=native` → `-march=x86-64-v3` |
| 热参数 | ✅ | `on_params_updated` 只置位，worker 应用命令/TPS/QMode；Composite alpha 权重经 `setAlphaWeights` 热更新（Gamma 策略独立，不适用） |
| TPS 计数 | ✅ | `updateOrders` 返回实际发单/撤单数，不再恒 1 |
| ConfigValidator | ✅ | 改用 `memberNames()` 通用遍历；动态 signals/scanners 跳过 |
| CommandBus / overflow | ✅ | 解析异常兜底；MD overflow 有上限并计数 drops |
| Scanner 验收 | ✅ | 9 类 scanner 接入 `onOptionUpdate`/`publishHit`；生命周期、数值参数、expiryOverrides、GARCH warmup、panic 门控验收 |
| 阶段 2 组件 | ✅（首批） | ScanCoordinator / HedgeCoordinator / ExpiryLifecycle / CommandHandlers / StressAnalyzer |
| 策略 pricer 边界 | ✅ | `OptionPricerMode` 区分做市 vs gamma scalping；共享 OptionPricer2，不合并策略逻辑 |
| Grid 状态归属 | ✅（文档） | 三层职责/单写者约束写入 README；未做物理合并 |
| 配置迁移 | ✅（部分） | `minHedgeQty`→`delta_per_lot` 自动迁移并告警；完整 schema 迁移仍待做 |
| 组合/情景压力 | ✅（部分） | `StressAnalyzer` 组合 + per-expiry 情景；`riskLimits.maxStressLoss` 可选触发 Panic |
| 测试 | ✅ | `test_scanners`/`test_stress`/`test_hedge_coordinator`/`test_command_handlers` 新增；6 个测试程序全过 |

**仍未做（阶段 2）**：完整 SPAN/多 underlying 组合压力（已加 per-expiry 情景）、到期强平/结算/移仓、GVV 数据化、回测 harness；`HftOptionStrategy` 仍承担 Shell 职责。Composite/Gamma 是独立策略 pricer，不做物理合并。

---

## 1. 定位与架构

### 1.1 宿主与加载方式

`WtOptionCore` 是 **HFT 引擎上的策略插件**，不是独立引擎：

- `HftOptionStrategy : HftStrategy`（`HftOptionStrategy.h:61`）；
- 工厂 `OptionStraFact` / 策略 `OptionMM`（`HftOptionStraFact.cpp:12-42`）；
- 由 `WtRunner` 的 `HftStrategyMgr` 从 `./hft/libWtOptionCore.so` 加载（`WtRunner.cpp:330-339`）；
- 配置节点 `strategies.hft[]`，部署实例 `dist/WtRunnerOption/`。

### 1.2 组件与数据流

```text
CTP/XQP Tick
  │  MD 回调线程
  ▼
OptionAsyncEventProcessor（MD SPSC + trader mutex 双队列，worker 线程）
  │  优先级：Session → Channel → Position → Trade → Order → Timer → Tick
  ▼
OptionGrid（Expiry → Strike → Call/Put；合约发现、盘口、远期、波动率）
  ▼
CompositeOptionPricer / GammaScalpOptionPricer（定价/Greeks/ourMarket）
  ▼
ControllableTradingGrid（check_markets → rank → TPS → updateOrders）
  ▼
OptionQuoteManager（diffing / 撤单节流 / wait-for-cancels / RiskFilterChain）
  ▼
IHftStraCtx::stra_quote / stra_buy / stra_sell
  ▲
OptionRisk（组合 Greeks） / PnlTracker / QuoteStatistics / AttributePublisher
```

**做得好的部分**：

- 异步双队列正确解决了 SPSC 多生产者 UB；`stop()` 会 join worker（`OptionAsyncEventProcessor.cpp:130-136`）；
- tick 去重按内容而非指针（`OptionAsyncEventProcessor.cpp:315-319`），避免指针复用误判；
- 报价是增量 diff，不是全量撤重发；有 TTL、软/硬撤单上限、wait-for-cancels、cautious flipping；
- 风控有多层：`RiskFilterChain`（单笔/仓位/卖价/撤单数）、`RiskLimitsEx`（Greeks/保证金/涨跌停）、`PositionGuard`、`FillPriceChecker`、`PnlLimitSignal`；
- FIFO PnL、QuoteStatistics、AttributePublisher 提供了初步绩效与监控；
- 支持商品期权、CZCE C/P 双品种、CFFEX IO/MO/HO 的标的/对冲分离。

### 1.3 主要架构问题

| 问题 | 证据 | 影响 |
|---|---|---|
| 上帝类 | `HftOptionStrategy.cpp` 2,417 行，配置 + 命令 + 扫描 + 组合 + 对冲 + 生命周期全在一处 | 改动半径大，难以单测 |
| 三套定价路径 | `CompositeOptionPricer`（1,760 行）、`GammaScalpOptionPricer`、legacy `OptionPricer2` | 职责重叠、行为不一致 |
| 三层网格 | `OptionGrid`（数据）→ `OptionTradingGrid`（交易数据）→ `ControllableTradingGrid`（执行） | 状态归属与失效路径不清晰 |
| 配置扁平且校验弱 | 100+ 键；`ConfigValidator` 只探测硬编码键 | 拼写错误静默失效 |
| 热参数半接线 | `sync_param` 只注册十余个键；alpha 权重只打日志 | “热更新”名不副实 |
| 测试薄弱 | `tests/` 2 个程序、1,296 行；CTest 已注册但没有定价/组合/对冲用例 | 改交易逻辑无回归网 |

---

## 2. 策略逻辑逐段评估

### 2.1 事件与线程

- `on_tick/on_trade/on_order/on_position/on_channel_*` 都只做轻量入队；worker 统一处理，方向正确。
- `on_params_updated` 是例外：它直接在框架回调线程里修改 `_ctg`、`_otg`、`_grid`、`TradingState`，而 worker 线程也在读这些对象（`HftOptionStrategy.cpp:2339-2408`）。这是**跨线程写**，不是命令化。
- `on_session_end` 只把状态设为 `Disabled`，没有 `cancelAll`/平仓（`HftOptionStrategy.cpp:1710-1725`），收盘后的挂单/持仓依赖 OQM TTL 与交易所收盘，策略自身不负责收尾。

### 2.2 定价与波动率

- `BlackCalc`/`BlackImpliedCalculator` 与 QuantLib 的解析式基本一致；IV 用 put-call parity 反推远期，整体可信。
- **`thetaPerDay` 头/实现语义不一致**：头文件声明 `(double spot, double maturity)`，实现是 `(double rate, double vol)`（`BlackCalc.h:49` vs `BlackCalc.cpp:190`）；唯一调用按 `(0, m_theoVol)` 传参（`OptionPricer2.cpp:514`）。类型相同，编译器不报错，但任何按头文件语义调用的新代码都会得到错误 theta。
- **NaN 波动率被静默洗成 1%**：`std::max(0.01, theoVol)`（`OptionPricer2.cpp:540`）在 `theoVol=NaN` 时返回 0.01，全链按 1% 定价且无告警。
- **GVV 参数仍是 stub**：`OptionPricer2::updateGvvParams` 只 `(void)exp; (void)curve;`（`OptionPricer2.cpp:231-240`），曲线参数没有数据驱动来源；`PeriodicCurveFitter` 同一点集拟合两条曲线的旧问题仍在。
- **到期近似**：`OptionGrid.cpp:401-402` 在拿不到精确到期日时硬编码“当月 15 日”，对 CFFEX IO（第三个周五）偏差可达一周；`ExpiryData::getMaturity` 把剩余期限钳到 `1/252`（`ExpiryData.cpp:123-126`），已到期合约仍可能被定价。

### 2.3 报价与订单

- `OptionQuoteManager` 的增量 diff、TTL、软/硬撤单上限、`wait-for-cancels`、`cautious flipping`、`PositionGuard` 都是生产级思路。
- **缺撤单超时升级**：`cancelPending` 在发撤单时置 true（`OptionQuoteManager.cpp:250-258`），只在 `onOrderStatusChange` 终态清除（`:425-432`）；如果柜台撤单回报丢失，`wait-for-cancels` 会一直阻止该侧发新单（`:146-150`）。没有 B+ 式的重试/zombie/升级。
- **TPS 计数口径失真**：`updateOrders` 正常路径恒 `return 1`（`:264`），不是实际发单/撤单数；`ControllableTradingGrid` 用该返回值累加 TPS，导致限流低估或高估。
- `on_entrust` 只打日志（`HftOptionStrategy.cpp:2044-2050`），没有失败分类/重试；与 FutuCore 的 B+ 相比差距明显。

### 2.4 组合单（Combo）

`SpreadComboOrder` 的当前逻辑：

```cpp
// ComboOrders.cpp:146-150
if (!m_leg2Sent) return false;
return m_leg2Sent;   // “leg2 已发”即完成
```

- `onLegFill` 只累计 leg1，leg2 成交只标记 `matched`（`:119-121`），没有 leg2 成交量；
- `hedge()` 每次覆盖 `m_leg2LocalId`（`:81-85`），旧对冲单成为孤儿；
- `cancelAll` 只撤最新的 leg2（`:168-177`）；
- 没有 leg2 拒单/撤单回调，也没有“leg1 成交而 leg2 失败”的撤 leg1 路径。

**后果**：leg1 成交、leg2 被拒/部分成交时，策略认为 combo 已完成并把对象移出 `_activeCombos`，裸敞口无人负责。

### 2.5 Delta 对冲

`DeltaHedgeExecutor::decide` 的手数公式：

```cpp
const double lotsRaw = std::fabs(excess) / std::max(cfg.minHedgeQty, 1e-9); // :61
```

- `minHedgeQty` 默认 1.0，且 `HftOptionStrategy.cpp:359-368` 只加载 `threshold/target/maxOrderSize/cooldown`，**从未从配置读取 minHedgeQty**；
- `excess` 来自 `getPortfolioDelta`，后者已乘合约乘数（`OptionRiskData.cpp:64-66`），IO 乘数 300 时 1 手 ATM 的 delta 约 150 → `lotsRaw` 约 150；
- 再被 `maxOrderSize=5` 截断，每 3 秒重复触发，没有在途单检查。

**后果**：超量对冲/过冲，且“1 手 ATM 多头只发 1 手对冲”的预期完全不成立。

### 2.6 到期、行权与收盘

- `ExpirationSimulator::checkExpiration` 在实盘代码中**从未被调用**；`_expSim` 只用于 `onFill` 记账、`clear` 和 `printSummary`（`HftOptionStrategy.cpp:469/1536/1704/1718`）。它不是结算/行权引擎，只是成交台账。
- `QM_CLOSE` 仅在 `QM_AUTO && delta 在 [delta_min, delta_max]` 时进入（`CompositeOptionPricer.cpp:968-975`），且只报价“减仓侧”（`:980-995`），不是强制平仓；如果期权已超出 delta 区间或盘口没有流动性，`QM_CLOSE` 不会触发。
- `OptionGrid`/`IOptionGrid` 没有删除到期合约的接口，`reevaluateFrontMonth` 只切换 front month（`OptionGrid.cpp:472-517`），旧 expiry 仍留在 `m_allOptions`，可能继续被定价/报价。
- `on_session_end` 不 `cancelAll`、不检查持仓，只 `Disabled` + 打印统计。

**后果**：到期日/收盘时可能留下期权或对冲腿；卖方期权到期进入实值会产生行权/被行权风险；结算 PnL 无法在策略内闭环。

### 2.7 风控、保证金与状态

- `RiskLimitsEx` 覆盖单笔、订单金额、Greeks、日内亏损、单合约/总持仓、short call/put、涨跌停；`checkMarginLimits` 估算期货/卖出期权保证金并 latch panic（`HftOptionStrategy.cpp:2186-2235`）。
- `TradingState` 有 Normal/Disabled/Panic；`PnlLimitSignal` 触发 panic（`RiskSignals.cpp:142-161`），且 `Panic` 与 `FillPriceChecker` 的 panic 已做 OR 合并（`HftOptionStrategy.cpp:1353-1365`）——这是已修复项。
- 不足：没有 SPAN/组合保证金、没有情景压力（标的跳空/波动率跳变）、没有“到期日专门风控”、没有独立的 closeout/halt 编排；`Panic` 主要靠降报价/撤单，不是强制减仓。

### 2.8 热参数与命令

- `on_params_updated` 只对 `maxTPS`、`command`、`qmode_override`、`manual_order` 做了实际动作；alpha 权重（`wgt_vegaflow` 等）只打日志（`HftOptionStrategy.cpp:2344-2349`），`CompositeOptionPricer` 的 config 是构造期快照，没有 setter。
- `CommandBus` 的 `ORDER` 处理器直接用 `std::stod/std::stoul`（`HftOptionStrategy.cpp:2294-2295`），`CommandBus::dispatch` 没有 try/catch（`CommandBus.h:35-60`）；畸形手工指令会抛异常，穿过框架回调，轻则丢指令，重则 terminate。
- `on_params_updated` 跨线程直接改 grid/CTG/OQM 状态，缺少命令队列或锁；`_hot` 字符串指针指向共享内存，生命周期依赖外部。

---

## 3. 缺陷清单

### 3.1 P0（实盘前必须修）

| ID | 问题 | 证据 | 影响 | 建议 |
|---|---|---|---|---|
| O-P0-1 | Combo leg2 发出即完成，残腿无补/撤 | `ComboOrders.cpp:123-150`、`:81-85`、`:168-177` | leg1 成交、leg2 拒单 → 裸敞口；旧对冲单不可撤 | 两腿状态机（sent/filled/terminal/rejected），残腿撤/补，id 列表 |
| O-P0-2 | Delta 对冲手数单位错配 + 无在途抑制 | `DeltaHedgeExecutor.h:61`；`HftOptionStrategy.cpp:359-368` | 超量对冲（IO 约 300×），每 3s 叠单 | 增加 `delta_per_lot`（默认合约乘数）；在途/working 检查；冷却基于成交/终态 |
| O-P0-3 | `cancelPending` 无超时/重试 | `OptionQuoteManager.cpp:146-150`、`:250-258`、`:425-432` | 撤单 ack 丢失 → 该侧永久停报 | `cancelPendingSince` + 重试 + 升级 cancelAll/重连；B+ 化 |
| O-P0-4 | 到期/收盘无强制平仓、过期合约不摘除 | `ExpirationSimulator.cpp:31`（无调用）；`CompositeOptionPricer.cpp:968-995`；`OptionGrid.cpp:401-402/472-517`；`HftOptionStrategy.cpp:1710-1725` | 到期持仓/卖权行权、结算 PnL 失控 | 到期生命周期：T-1 强平/移仓、过期摘除、session_end 校验 |

### 3.2 P1（高风险/明显能力缺口）

| ID | 问题 | 证据 | 建议 |
|---|---|---|---|
| O-P1-1 | `thetaPerDay` 头/实现签名不一致 | `BlackCalc.h:49` vs `BlackCalc.cpp:190` | 统一为 `(rate, vol)` 或强类型；补单测 |
| O-P1-2 | NaN 波动率被洗成 1% | `OptionPricer2.cpp:540` | `isfinite` 检查；失败则不报价/告警，不回退 1% |
| O-P1-3 | `ConfigValidator` 只探测硬编码键，typo 静默 | `ConfigValidator.h:78-82`、`:100-130` | 通用子键遍历/显式 schema；启动 fail-fast 或 warn |
| O-P1-4 | 热参数半接线、跨线程直写 | `HftOptionStrategy.cpp:2339-2408`、`:866-867` | 命令队列 + pricer setter；只允许 worker 线程改状态 |
| O-P1-5 | GVV 参数 stub，曲线未数据驱动 | `OptionPricer2.cpp:231-240`、`PeriodicCurveFitter.cpp` | 从配置/曲线文件加载，拟合单曲线验证 |
| O-P1-6 | TPS 计数恒为 1/次 | `OptionQuoteManager.cpp:264`；`ControllableTradingGrid.cpp:545/552` | 返回实际发单/撤单数；窗口按 drain 重置 |
| O-P1-7 | 到期日 fallback 15 日 | `OptionGrid.cpp:401-402` | 从合约信息/tick.expireDate 获取；无则拒绝报价 |
| O-P1-8 | 过期合约仍留在网格 | `IOptionGrid` 无 remove；`OptionGrid.cpp:472-517` | 增加 removeExpiry；计算/报价跳过 |
| O-P1-9 | `on_session_end` 不 cancel/flatten | `HftOptionStrategy.cpp:1710-1725` | cancelAll + 持仓校验 + 告警 |
| O-P1-10 | CommandBus 解析异常 | `HftOptionStrategy.cpp:2294-2295`；`CommandBus.h:35-60` | try/catch + 数值校验 |
| O-P1-11 | `-march=native` + LTO 进发行库 | `CMakeLists.txt:181-189` | `x86-64-v3` 或目标机矩阵 |
| O-P1-12 | 测试只有 2 个程序 | `tests/` 1,296 行；`CMakeLists.txt:239-251` | 补定价/IV/Combo/Hedge/Cancel 单测；CI |
| O-P1-13 | Scanner 部分为占位/未验证 | `Scanners/*`；README 自述 | 逐项验收或禁用，避免“配置了但不生效” |
| O-P1-14 | 异步 overflow 无上限 | `OptionAsyncEventProcessor.cpp:141-166`（`_md_overflow` push） | 设上限 + 丢弃计数/告警 |

### 3.3 P2（架构/性能/可维护）

| ID | 问题 | 建议 |
|---|---|---|
| O-P2-1 | `HftOptionStrategy` 2,417 行上帝类 | 拆 `OptionStrategyShell` / `CommandHandlers` / `HedgeCoordinator` / `ExpiryLifecycle` |
| O-P2-2 | 三套 pricer 路径（Composite/Gamma/legacy） | 明确主路径，legacy 标记/删除 |
| O-P2-3 | 三层 Grid 职责重叠 | 明确 data/trading/execution 边界，状态单一事实源 |
| O-P2-4 | 配置扁平、无 schema | 分层 schema + 校验 + 迁移工具 |
| O-P2-5 | `AttributePublisher` 只增不删 | TTL/清理 |
| O-P2-6 | 缺 tick→quote / cancel→ack 延迟环 | 复用 FutuCore `PerformanceMonitor` 思路（策略侧实现） |
| O-P2-7 | 无回测/回放 harness | 增加 replay 驱动与黄金输出 |
| O-P2-8 | 无版本/构建信息 | 注入 git sha / 构建时间 |

---

## 4. 与 2026-08-24 既有诊断的对照（避免重复）

`src/WtOptionCore/analysis/diagnostic-report-2026-08-24.md` 已列 10 项 P0。本次复核：

| 既有项 | 当前状态 |
|---|---|
| B01 `updateRiskShiftsVega` 单边 strike 空指针 | **已修**：`CompositeOptionPricer.cpp:1338-1344` 已判 `call()/put()` |
| B02 `PeriodicCurveFitter::fitToExpiry` 空指针 | **已修**：`PeriodicCurveFitter.cpp:155-160` 已 `continue` |
| B03 tickSize/multiplier 恒 1.0 | **已缓解**：`OptionData` 默认 0，`OptionTradingGrid.cpp:56-61/125/259` 从 `stra_get_comminfo` 注入 |
| P1-B 热参数 8/13 不落盘 | **已修**：worker 应用命令/TPS/QMode，Composite alpha 权重经 `setAlphaWeights` 热更新 |
| P1-C TPS 计数双重失真 | **已修**：`updateOrders` 返回实际发单/撤单数 |
| P1-D GVV 参数 stub | **仍存在**：`OptionPricer2.cpp:231-240` |
| P1-F panic 双源覆盖 | **已修**：`:1353-1365` OR 合并 |
| P1-G secondaryHedgeCodes 未聚合 | 未复核，保留 |
| P1-H frac_delta 临期衰减未配置 | 未复核，保留 |
| Scanner 注册/占位 | **已修**：9 类 scanner 接入 `onOptionUpdate`/`publishHit`，`test_scanners` 验收 |

**本次新增、且既有诊断未覆盖的 4 个 P0**：Combo 状态机、Delta 对冲手数、撤单 ack 超时、到期/收盘强制平仓。

---

## 5. 功能完善度矩阵

| 能力 | 现状 | 缺口/建议 |
|---|---|---|
| 期权定价（Black76/BS） | ✅ | 边界/NaN/theta 修正 |
| 隐含波动率 | ✅（Brent/parity） | 无解时整档丢弃，需降级/告警 |
| 波动率曲面 | ⚠️ GVV/Linear/Power | 参数 stub、无无套利校验 |
| Greeks | ✅ delta/gamma/vega/theta | theta 签名、vanna/volga 口径 |
| 组合风险 | ✅ 多 expiry 聚合 | 无 SPAN/情景压力 |
| 保证金 | ⚠️ 估算 | 非交易所 SPAN，仅 warn/panic |
| 报价/撤单 | ✅ diff/TPS/TTL | 撤单超时升级、TPS 计数 |
| 组合单 | ⚠️ 类存在 | 状态机不完整 |
| Delta 对冲 | ⚠️ 决策器 | 手数/在途/冷却 |
| 到期/行权 | ❌ 仅模拟/报表 | 强制平仓、结算、摘除、移仓 |
| 收盘收尾 | ❌ | cancelAll + 持仓校验 |
| 热参数 | ⚠️ 少量 sync_param | 命令队列 + 全量参数 |
| 配置校验 | ⚠️ 硬编码键 | 通用 schema |
| 监控/KPI | ⚠️ QuoteStatistics/Attribute | 延迟环、告警、日终报表 |
| 回测/回放 | ❌ | replay harness |
| 多标的/多品种 | ⚠️ 商品/CZCE/CFFEX | 多 underlying 组合风险 |
| 测试 | ⚠️ 2 个程序 | 定价/组合/对冲/撤单单测 |
| 部署 | ⚠️ `-march=native`/LTO | 目标机矩阵/可移植构建 |

---

## 6. 与 WtFutuCore 的对照（可借鉴的成熟能力）

| 能力 | WtFutuCore | WtOptionCore | 建议 |
|---|---|---|---|
| 订单台账 | B+ 槽状态机、撤单重试、zombie | OQM cancelPending 无超时 | 移植 B+ 思路（策略内实现） |
| 收盘平仓 | `CloseoutOrchestrator` + `CloseoutExecutor` | `QM_CLOSE` 仅减仓侧报价 | 增加 `OptionCloseoutOrchestrator` |
| 停机/恢复 | HaltGate、HaltReasons、Recovery | TradingState/Panic | 增加 halt 原因位图 + 恢复预算 |
| 热参数 | FutuHotParamManager（校验/边界/审计） | 少量 sync_param | 独立 HotParamManager |
| 配置审计 | 四源审计 | ConfigValidator 部分 | 完整 schema + 审计 |
| 性能监控 | 延迟环/KPI/积压 | QuoteStatistics | 增加 tick→quote/fill 延迟环 |
| 账本/回放 | membin + replay | PnL/OQM counters | 增加交易/持仓 journal |
| 测试 | 215 用例 | 2 个程序 | 补 30–50 个核心用例 |

---

## 7. 完善路线图

### 阶段 0：止血（1 周，P0）

| 任务 | 具体动作 | 验收 |
|---|---|---|
| Combo 状态机 | 两腿 `sent/filled/terminal/rejected`；残腿撤/补；id 列表；`done` 仅在两腿终态且净敞口 0 | 注入 leg2 拒单/部分成交，leg1 被撤、敞口归零 |
| Delta 对冲 | `delta_per_lot` 配置（默认乘数）；在途单集合；冷却基于成交/终态 | 1 手 ATM 只发 1 手；重复 decide 不叠单 |
| 撤单超时 | `cancelPendingSince` + 重试 K 次 + 升级 cancelAll/重连 | 模拟无 ack，侧别在 K 次后恢复或 halt，不永久卡死 |
| 到期/收盘 | `ExpirationLifecycle`：T-1 强平/移仓；过期摘除；`session_end` cancelAll + 持仓校验 | 到期日无残留、无过期报价 |
| 定价边界 | theta 签名统一；NaN/Inf 不报价；IV 无解降级 | 单测覆盖 NaN/无解/零波动 |
| 配置 | 修 `ConfigValidator` 的 typo 漏检；启动 fail-fast | 拼写错误必告警 |
| 构建 | 移除 `-march=native`，改 `x86-64-v3` 或目标矩阵 | 异构机器可加载 |

### 阶段 1：生产化（2–4 周）

- 热参数命令化：`on_params_updated` 只投递命令，worker 线程应用；pricer 增加 setter。
- TPS 计数改为实际发单/撤单数；`drain` 时重置窗口。
- GVV 参数从配置/曲线文件加载；`PeriodicCurveFitter` 单曲线校验。
- 到期生命周期：T-1 强平、T 日结算/行权、过期摘除、移仓。
- 监控：tick→quote、quote→fill、cancel→ack 延迟环；日终 PnL/持仓/成交报表。
- 回测/回放：replay harness + 黄金输出；CI + ASan/UBSan。
- 测试：定价/IV/Greeks/Combo/Hedge/Cancel/Expiry 至少 30 个用例。

### 阶段 2：架构（1–2 月）

- 拆 `HftOptionStrategy`：Shell / CommandHandlers / HedgeCoordinator / ExpiryLifecycle / ScanCoordinator。
- 统一 pricer 路径（Composite 为主，Gamma 为策略变体，legacy 删除/标注）。
- 明确三层 Grid 的状态归属；单一事实源。
- 配置 schema + 迁移；SPAN/情景压力；多 underlying 组合风险。
- 与监控/告警/部署流水线集成。

---

## 8. 验收标准（建议）

1. **Combo**：leg2 拒单/部分成交/撤单三种注入，leg1 自动撤或补，净敞口 = 0；旧 leg2 id 可撤。
2. **Hedge**：1 手 ATM 多头（IO 乘数 300）只发 1 手 IF；在途时 `decide` 不重复发；冷却基于成交。
3. **Cancel**：模拟 cancel ack 丢失，K 次后升级 cancelAll/重连；侧别不永久阻塞。
4. **Expiry**：到期日 T-1 强平/移仓；过期合约不再出现在 `getAllOptions` 报价路径；结算 PnL 有记录。
5. **Pricing**：NaN/Inf/IV 无解不报价，不回落 1%；theta 与 Black-76 数值对标。
6. **Config**：拼写错误必告警；未知键不静默。
7. **Performance**：tick→quote p99 有基线；无 busy-spin；无每 tick 字符串分配。
8. **Test/CI**：核心用例 ≥30；ASan/UBSan 通过；回放三次结果一致（若可消除随机源）。

---

## 9. 结论与建议

WtOptionCore 的“定价/报价/Greeks/风控”已经具备做市策略的主体能力，**真正的短板在交易生命周期**：组合单、对冲、撤单、到期/收盘。这四块不补，实盘会出现“裸腿、超量对冲、报价卡死、到期持仓”四类事故。

建议按 **阶段 0（P0）→ 阶段 1（生产化）→ 阶段 2（架构）** 推进；阶段 0 的改动都可限制在 `src/WtOptionCore/` 内，不需要改框架。若确认，我可以先按阶段 0 出逐文件修改方案并补单元测试，再提交代码。
