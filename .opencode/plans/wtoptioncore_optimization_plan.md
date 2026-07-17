# WtOptionCore 全面诊断与优化升级方案

> 基于原项目 `quantbox/optiontrader` 与迁移项目 `wondertrader/src/WtOptionCore` 的逐行对比分析

## 目录

1. [项目架构维度](#一项目架构维度)
2. [业务逻辑维度](#二业务逻辑维度)
3. [代码优化维度](#三代码优化维度)
4. [性能提升维度](#四性能提升维度)
5. [实施路线图](#五实施路线图)
6. [风险与注意事项](#六风险与注意事项)

---

## 一、项目架构维度

### A1: HftOptionStrategy / UftOptionStrategy 代码重复率 ~90%

**严重程度**: 高

**描述**:
- `HftOptionStrategy.cpp` (922行) 和 `UftOptionStrategy.cpp` (738行) 几乎完全相同
- 以下方法在两个文件中重复实现:
  - `init()` — 配置读取 (仅 expiry config 解析位置不同)
  - `setupGrid()` — holiday 解析逻辑完全相同
  - `setupPricer()` — pricer 创建完全相同
  - `setupCTG()` — CTG 创建完全相同
  - `setupAsyncCallbacks()` — 回调注册几乎相同
  - `executeQuote/executeCancel` — 完全相同
  - `on_params_updated()` — 热更新逻辑相同
- 任何 bug 修复或功能新增需同步修改两处, 维护成本高

**修复方案**:
```
新建 OptionStrategyBase (公共基类)
├── init()          — 公共配置读取
├── setupGrid()     — 公共 grid 创建
├── setupPricer()   — 公共 pricer 创建
├── setupCTG()      — 公共 CTG 创建
├── setupSignals()  — 公共信号创建
├── setupScanners() — 公共 scanner 创建 (修复 A6)
├── setupAsyncCallbacks() — 公共回调
├── executeQuote/executeCancel — 公共执行
└── on_params_updated() — 公共热更新

HftOptionStrategy : OptionStrategyBase  (仅保留 HFT 特有逻辑)
  - auto-discovery (stra_get_comminfo + CodeHelper)
  - hedge contract subscription
  - hedge UTD market update in on_tick
  - PnlTracker initialization + price updates
  - Portfolio PnL -> PnlLimitSignal
  - setupSignals()

UftOptionStrategy : OptionStrategyBase  (仅保留 UFT 特有逻辑)
  - on_trade/on_order 的 isLong/offset -> isBuy 转换
  - 从 config 读取 optionContracts 列表
```

### A2: ControllableTradingGrid 职责过载

**严重程度**: 中

**描述**:
- 原始 CTG 继承 `OptionTradingGrid + CommandServicesHelper + IPositionListener + IOrderStatusListener`
- 迁移后虽简化但仍承担: 排序 + TPS 限流 + 报价管理 + 事件分发
- refresh() 方法内联了 combineMarkets 逻辑, 职责不清晰

**修复方案**: 将 refresh() 拆分为:
- `collectDesiredMarkets()` — 收集所有 option/future 的 desired market
- `rankAndSort()` — 排序
- `dispatchOrders()` — TPS 限流后分发

### A3: OptionGrid 与 OptionRisk / OptionPricer 集成未完成

**严重程度**: 高 (阻塞性问题)

**描述**: (经详细代码验证)
- `OptionRisk.cpp` 包含 **10处** `TODO[OptionGrid migration]` 标记:
  - L39: `getAllOptions()` 返回类型不确定
  - L47: `grid->addListener(this)` 被注释掉, OptionRisk 不注册为 grid listener
  - L57: `expired()` 检查被注释掉, 对过期合约也创建 risk data
  - L78: `get()` 无法从 grid 查找懒创建
  - L168: SpotTradingData 的 `m_pfuDelta` override 被注释
  - L212: `getUnderlierDelta()` SpotTradingData delta override 为空操作
  - L235: `totalDelta()` SpotTradingData delta 贡献缺失
  - L244: `getNonZeroPositions()` API 可用性不确定
  - L271: `onAddOption()` expired() 检查被注释
- `OptionPricer.cpp` 包含 **6处** `TODO[OptionGrid migration]`:
  - L42: `ExpiryInfo::computeForwardPrice()` 函数体为空
  - L113: `computeValues()` 不遍历 options, 直接返回 false
  - L124: `computeImpliedValues()` 函数体 `(void)grid; return true;`
  - L132: `initValuesCompute()` 函数体 `(void)grid; return true;`
  - L250: `finalizeCompute()` 不调用 `notifyMarketsPriced`
  - L24: `GvvVolCurve.h` include 被注释 (GSL 依赖未解决)
  - 文件头 L12-13 明确标注: "This file will not link until OptionGrid/OptionRisk are migrated"
- `OptionPricer2.cpp`:
  - L664: `finalizeCompute()` 循环体为空, `notifyMarketsPriced` 被注释
  - L216-225: `updateGvvParams()` 函数体为空 (MySQL 读取被移除)
- `CompositeOptionPricer.cpp`:
  - L1729-1734: `finalizeCompute()` 循环体为空操作
  - L379: UnderlyingTradingData 未连接到 ExpiryData
  - L625: `updateDistortValuesFuture()` 仅更新 forward, 跳过 distort values
  - L594: fees 硬编码为 0
  - L743: bid_tick_size 硬编码为 1.0
- `StrikeData.cpp`:
  - L31: `create()` 不构造 OptionData 对象
  - L61: `getImpliedVol()` 返回 NaN
  - L70: `getPosition()` 总是返回 0
- `OptionOrderInfo.cpp` L42: `m_atmsig0` 和 `m_pfdelta0` 总是 NaN
- `FutureOrderInfo.cpp` L39: 同上
- `WtOptEngine.cpp` L249: `on_minute_end()` 整个函数体被注释掉

**修复方案**:
1. 完成 OptionGrid API 对接: 实现 `addListener`, `expired`, `getAllOptions` 等接口
2. 完成 OptionPricer 的 `computeForwardPrice`, `computeValues`, `finalizeCompute` 实现
3. 恢复 `notifyMarketsPriced` 调用链
4. 将 UnderlyingTradingData 连接到 ExpiryData
5. 恢复 `on_minute_end()` 事件分发 (用于定时 curve fitting)
6. 接入合约费用和 tick_size 信息

### A4: 缺少分层策略基类

**严重程度**: 中

**描述**:
- 原始有 OptionTraderStrategy 作为单一策略入口 (205行 header + 760行 impl)
- 迁移后散布到 HftOptionStrategy / UftOptionStrategy / WtOptionStrategy 三个文件
- 无公共基类 (OptStrategy.h 仅定义回调接口, 无实现)

**修复方案**: 与 A1 方案合并, 创建 OptionStrategyBase

### A5: WtOptContext 职责膨胀

**严重程度**: 中

**描述**:
- 原始 OptionTraderContext 仅 83行 header + 49行 impl, 是一个薄协调器
- 迁移后 WtOptContext 膨胀到 302行 header + 821行 impl
- 承担了: grid/risk/order_mgr 所有权 + 交易 API + 异步事件队列 + 检查点恢复 + 多重职责

**修复方案**:
```
WtOptContext (瘦化) — 仅保留 IOptStraCtx 接口实现 + 交易 API 代理
OptionEventLoop (新) — 异步事件队列 + worker 线程 (从 WtOptContext 剥离)
CheckpointManager (新) — save_data/load_data 序列化恢复
```

### A6: Scanner 未接入 HFT/UFT 主流程 + 接口冲突

**严重程度**: 高

**描述**: (经详细代码验证)
- `WtOptionStrategy` (legacy): Scanner **已完整接入** — 从 config 创建、生命周期管理、事件分发
- `HftOptionStrategy`: 仅有 `#include "IScanModule.h"` (死代码), 无 scanner 成员, 无 factory 调用, 无运行时调用
- `UftOptionStrategy`: 同上, 完全未接入
- `ControllableTradingGrid`: 有 `addScanner/removeScanner/onOptionHit` + `m_scanners` vector, 但**从未被调用** (grep `.addScanner(` / `->addScanner(` 零结果)
- **接口冲突**: 存在两个 `IScanModule.h`:
  - `src/WtOptionCore/IScanModule.h` (顶层): 最小 stub, HFT/UFT/CTG 使用
  - `src/WtOptionCore/Scanners/IScanModule.h` (子目录): 完整接口 (ScannerFactory, ScannerHitEvent, IScannerListener, REGISTER_SCANNER), WtOptionStrategy 和所有具体 scanner 使用
  - 两者定义同名 `wt_option::IScanModule` 但接口不同, 无法在同一编译单元共存
- 配置文件 `option_config_demo.json` 定义了 10 个 scanner, 但 HFT/UFT 不解析 `scanners` 配置段

**修复方案**:
1. 统一两个 `IScanModule.h` — 删除顶层 stub, 保留 `Scanners/IScanModule.h` 的完整版本
2. 在 HftOptionStrategy/UftOptionStrategy 中增加 scanner 成员和配置解析
3. 在 init() 中增加 `setupScanners()` 调用
4. 在 on_tick/on_batch_complete 回调中分发 scanner 事件

---

## 二、业务逻辑维度

### B1: Panic Signal 自动检测缺失

**严重程度**: P0

**描述**:
- 原始 `OptionTraderStrategy::onSignalChanged()` (L418-445):
  - 使用 `ISignal` + 双阈值 (`panic_signal_thresh1`, `panic_signal_thresh2`)
  - `sig1 = signal.getSignalState()[1]`, `sig2 = sig1 + signal.getSignalState()[2]`
  - 超阈值 -> `m_spOptionPricer->onPanic(signal)` + 通知所有 scanner
  - 阈值可通过 `notifiable<double>` 热配置
- 迁移后: 无信号自动检测, panic 仅靠手动 `command=2`

**修复方案**:
- 在 `Signals/RiskSignals.h` 扩展 `PanicSignal`, 支持双阈值
- 在 `on_batch_complete` 中检查信号状态, 超阈值自动触发 panic
- CTG panic 模式恢复 `m_maxPanicTPS` 逻辑 (见 B18)

### B2: Risk Free Rate Curve (期限结构) 缺失

**严重程度**: P0

**描述**:
- 原始 `initRiskFreeRateCurve()` (L447-498):
  - 从 Lua 配置创建 `IVolCurve` (day->rate 数据点)
  - 默认: 0%@1day, 0%@90days, 0%@365days
  - 传入 `OptionInfoInitializer` 影响定价
- 迁移后: 使用 flat `_riskFreeRate = 0.03`
- 影响: 长到期期权定价不准, forward price 计算偏差

**修复方案**:
- 扩展 `IVolCurve` 支持 rate curve 配置
- 从 JSON config 读取 day->rate 数据点, 用插值或 WLS3 拟合
- 传入 `ExpiryData::getForward()` 计算

### B3: Compute Values 防抖触发缺失

**严重程度**: P0

**描述**:
- 原始 `setupComputeValuesTrigger()` (L379-416):
  - 模式 a: `compute_values_on_book` -> 订阅前月合约 IBookListener
  - 模式 b: `TriggerListDriver` + 特定消息类型 + 20ms 超时
  - `onFire()` 触发 `computeValues()`
  - `onComputeValuesTimeout()` 防抖: `min_compute_values_interval` 内不重复计算
- 迁移后: 每个 tick batch 都全量调用 `computeValues()`, 无防抖
- 影响: 数百合约 × BS定价 + Greeks = 高 CPU, 不必要的 vol curve refit

**修复方案**:
- 在 `OptionAsyncEventProcessor` 中增加 dedup timer (20ms)
- 仅当前月 tick 变化或超时时触发 computeValues
- 支持 `min_compute_values_interval` 配置

### B4: Mid-Day 自动停止调度缺失

**严重程度**: P1

**描述**: (经验证修正)
- 迁移后 `tradingStopMidDay()` 本身**已实现** (禁用交易 + refresh), 但:
  - 原始 CTG 按交易所特定时间**自动调度**:
    - SHSE: 11:29:30, 14:56:30
    - CFFEX: 11:29:30, 14:59:30
    - DCE: 10:14:30, 11:29:30, 14:59:30
  - 使用 `ClockMonitor::scheduleWakeupCall` 精确调度
  - 迁移后无自动调度, 需手动调用

**修复方案**: 基于品种交易 Session 的自动调度 (区别于原项目的按交易所硬编码方式)

**设计原理**:
- 原项目按交易所硬编码时间 (SHSE: 11:29:30, CFFEX: 11:29:30, DCE: 10:14:30 等), 不够灵活
- WT 框架的 `WTSSessionInfo` 已完整定义每个品种的交易时段 (`sections`), 例如:
  - `FD0900` (期货日盘): `900-1015, 1030-1130, 1330-1500` — 有 10:15-10:30 和 11:30-13:30 休息时段
  - `SD0930` (股票): `930-1130, 1300-1500` — 有 11:30-13:00 午休
  - `FN0100` (期货夜盘): `2100-100, 900-1015, 1030-1130, 1330-1500` — 夜盘+日盘
- **直接利用 `WTSSessionInfo::getTradingSections()` 获取品种自身的交易时段**, 在每个 section 结束前自动停止, 每个 section 开始后自动恢复

**数据来源**:
- `WtOptContext::init()` 已获取 `commInfo->getSessionInfo()` (L155), 可直接传给 CTG 或 strategy
- `WTSSessionInfo::getTradingSections()` 返回 `vector<TradingSection>`, 每个 section 含 `first_raw` (起始 HHMM) 和 `second_raw` (结束 HHMM)

**触发位置**:
- `WtOptRtTicker` 的 100ms 定时器中, `curTime` 格式为 `HHMMSS` (6位, 精确到秒)
- 在 `WtOptEngine::on_timer()` 中增加 session 边界检测, 通过 `enqueue_session_schedule` 事件分发到 context
- 或直接在 strategy 的 `on_timer` 回调中检测 (精度为分钟级, 因 `stra_get_time()` 返回 HHMM)

**调度逻辑**:
```
1. 初始化时获取 underlying 的 WTSSessionInfo
2. 提取所有 TradingSection 边界 (first_raw, second_raw)
3. on_timer 回调中检查当前时间:
   - 如果当前时间接近某个 section 的 second_raw (结束时间) -> 调用 tradingStopMidDay()
   - 如果当前时间刚过某个 section 的 first_raw (开始时间) 且当前处于 stopped 状态 -> 调用 resumeTrading()
4. 维护 _sessionStopped 状态标志, 避免重复触发
```

**时间比较** (支持秒级精度):
```cpp
// curTime 为 HHMMSS (6位), section 边界为 HHMM (4位)
// 将 section 边界转换为秒: HH*3600 + MM*60
// curTime 转换: (curTime/10000)*3600 + ((curTime/100)%100)*60 + curTime%100
// 提前量: stopLeadSeconds (默认 30 秒)
// 恢复延迟: resumeLagSeconds (默认 0 秒)

bool shouldStop(uint32_t curTime, uint32_t sectionEnd, uint32_t leadSecs) {
    uint32_t curSec = toSeconds(curTime);        // HHMMSS -> seconds
    uint32_t endSec = toSeconds(sectionEnd * 100); // HHMM -> HHMM00 -> seconds
    return curSec >= (endSec - leadSecs) && curSec < endSec;
}

bool shouldResume(uint32_t curTime, uint32_t sectionStart, uint32_t lagSecs) {
    uint32_t curSec = toSeconds(curTime);
    uint32_t startSec = toSeconds(sectionStart * 100);
    return curSec >= (startSec + lagSecs);
}
```

**配置项**:
```json
{
    "sessionSchedule": {
        "enabled": true,
        "stopLeadSeconds": 30,
        "resumeLagSeconds": 0
    }
}
```

**与原项目方案的差异**:

| 方面 | 原项目方案 | 修订方案 |
|------|-----------|---------|
| 时间来源 | 按交易所硬编码 (SHSE/CFFEX/DCE) | 按品种 session 动态获取 (`WTSSessionInfo`) |
| 维护 | 新增品种需改代码 | 自动适配所有 WT 支持的品种 |
| 灵活性 | 固定时间, 不可配置 | 可配置提前量和延迟 |
| 适用范围 | 仅 SHSE/CFFEX/DCE | 所有品种 (含夜盘) |
| 精确度 | 秒级 | 秒级 (基于 `WtOptRtTicker` 的 `HHMMSS`) |

**实现步骤**:
1. 在 `ControllableTradingGrid` 中增加 `setSessionInfo(WTSSessionInfo*)` 方法
2. 在 `ControllableTradingGrid` 中增加 `checkSessionSchedule(uint32_t curTime)` 方法
3. 在 strategy 的 `on_timer` 回调中调用 `checkSessionSchedule`
4. 或在 `WtOptEngine::on_timer` 中统一检测后通过事件分发

### B5: Late Fill 检测缺失

**严重程度**: P1

**描述**:
- 原始 CTG::`onFillWithFees()` (L768-830):
  - 迟到成交检测: 如果 fill 到达时合约在当前周期未被更新 -> `setLateFill(true)`
  - 递增 `m_lateFills` 和 `m_totalFills` 计数器 (PropertyManager 持久化)
  - 通知 `services()->notifyFill()` 含 forward price
- 迁移后: 完全缺失, 无迟到成交检测, 无 fill 计数, 无通知
- `BaseOrder.h` 有 `m_isLateFill` 字段和 `setLateFill()` 方法, 但无逻辑设置

**修复方案**:
- 在 `OptionQuoteManager::onFill` 中增加 late fill 检测
- 比较 fill 时间与最后一次 computeValues 时间
- 设置 `m_isLateFill` 标志并记录

### B6: Front Month 自动换月缺失

**严重程度**: P1

**描述**:
- 原始 `getFrontMonthInstrument()` (L500-522):
  - 根据当日日期推算前月合约
  - 检查合约是否过期, 过期则滚到下月
  - 使用 `db::getExpiryDate()` 和 `ExpiryData::getNumberOfTradingDays()`
  - `onBeginOfDay` 重新评估
- 迁移后: 使用 `_underlyingCode` 从 config (如 "EXCHANGE.PRODUCT.main"), 依赖 WT "main" 合约解析
- 影响: 无自主换月逻辑

**修复方案**:
- 在 `on_session_begin` 中增加前月合约重新评估
- 或确认 WT "main" 合约解析机制满足需求并记录文档

### B7: AttributePublisher (监控广播) 缺失

**严重程度**: P2

**描述**:
- 原始 `AttributePublisher` (142行 header + 582行 impl):
  - 15+ 属性字段: delta, position, enabled, bqpos/aqpos, mbid/mask, qmode, obid/oask, fwd, theo, vshift
  - JSON 序列化 + dirty-field 追踪
  - 1500ms 节流发布
  - `notifier()->sendRaw("optiongreeks"/"udata")`
- 迁移后: 完全缺失, 仅 WTSLogger 文本日志

**修复方案**:
- 可选: 适配 WT 监控体系, 用 WTSLogger + 结构化数据发布
- 或: 通过策略 API 直接暴露 Greeks/位置数据

### B8: ManualOrderManager (手动下单) 缺失

**严重程度**: P2

**描述**:
- 原始 `ManualOrderManager` (42行 header + 177行 impl):
  - 文本命令: `order_new {instr} {B|S} {price} {size}`, `order_cxl {index|all}`
  - JSON 订单列表广播: `"morder-list"`, `"morder-update"`
- 迁移后: 完全缺失, 仅程序化 API (`stra_buy/sell/cancel`)

**修复方案**:
- 通过 hot-param 机制增加手动下单命令
- 或通过 WT 的管理接口实现

### B9: ExpirationSimulator (到期模拟) 缺失

**严重程度**: P2

**描述**:
- 原始 `ExpirationSimulator` (66行 header + 178行 impl):
  - 组合级 PnL 模拟: 交易额 + 结算值 + 收盘值
  - TAIFEX TXF/MXF 到期逻辑
  - `vector<DayResult>` 逐日 PnL 累计
- 迁移后: 替换为 `PnlTracker` (55行 header + 45行 impl), 仅单合约 tick-by-tick mark-to-mid
- 两者用途不同: ExpirationSimulator 是回测/模拟工具, PnlTracker 是实时跟踪

**修复方案**:
- 保留 PnlTracker 用于实时
- 如需回测 PnL 归因, 可后续实现简化版 ExpirationSimulator

### B10: OptionValueWriter (值记录) 缺失

**严重程度**: P2

**描述**:
- 原始: 定时将理论值、波动率、Greeks 写文件, 用于历史分析
  - 配置: `ovw_start_time`, `ovw_end_time`, `ovw_output_period`, `ovw_keep_history`
- 迁移后: 完全缺失

**修复方案**: 后续实现, 适配 WT 的数据输出机制

### B11: Predictor/Forecast Model 缺失

**严重程度**: P3

**描述**:
- 原始: `IPredictor` + `IPredictorFactory` + `TriggerEngine`
  - `SignalsPredictor` + 控制信号
  - `onTriggerUpdate` 刷新预测
- 迁移后: 完全缺失 (有 IAlphaSignal/IRiskSignal 但无 predictor 基础设施)

**修复方案**: 按需实现, 优先级低

### B12: Multi-level Market (多档行情) 缺失

**严重程度**: P1

**描述**:
- 原始 `MultiMarket` 包含 `MktLevels` (sorted map<price, size> 多档)
  - `check_markets` 逐档比较
  - `isBest()` 用 `fade_price` 1-tick 放松检查
  - `take_inner()` 多源行情合并 (slot 0 + slot 2)
- 迁移后 `MultiMarket` 仅单档 (best bid + best ask)
  - 无法多档报价
  - 无法多源合并
  - `isBest` 逻辑无法实现

**修复方案**:
- 扩展 `MultiMarket` 支持 top-N levels (vector<PriceSize>)
- 恢复 `check_markets` 多档比较
- 恢复 `take_inner` 多源合并

### B13: Multi-source Market 合并缺失

**严重程度**: P2

**描述**:
- 原始: slot 0 (主市场) + slot 2 (次市场) 通过 `take_inner` 合并
- 迁移后: 单源, 无合并
- `OptionData` 的 `MAX_VALUES=5` 双缓冲仅用 slot 0

**修复方案**: 与 B12 合并实现

### B14: Secondary Hedge (次要对冲) 缺失

**严重程度**: P2

**描述**:
- 原始: `secondary_hedge_symbol` + `hedge` vector (多对冲合约)
- 迁移后: 仅 `setHedgeOverride` 单一对冲

**修复方案**: 扩展 expiry config 支持多对冲合约

### B15: Expiry Readiness Tracking 缺失

**严重程度**: P3

**描述**:
- 原始: `onAddExpiry` 订阅 `valuesReadyChangedEvent`, 追踪 `m_ntExpiryFwdReady` / `m_ntExpiryFitReady`
- 迁移后: `onAddExpiry` 空实现 (CTG), `OptionTradingGrid::onAddExpiry` 仅设 hedge override

**修复方案**: 后续实现

### B16: Drop Tracking + Retry 缺失

**严重程度**: P2

**描述**:
- 原始: `m_txDrop` (丢弃计数) + 1秒 wakeup call 重试
- 迁移后: 无丢弃追踪, 无重试

**修复方案**: 在 CTG drainPendingQuotes 中增加丢弃计数和重试调度

### B17: Active Counter 缺失

**严重程度**: P3

**描述**: 原始追踪 `activeOptions`, `activeFutures`, `activeSides`, 迁移后缺失

**修复方案**: 增加统计计数器

### B18: Panic TPS 增强缺失

**严重程度**: P1

**描述**:
- 原始 panic 模式:
  - `txn_limit = m_maxTPS + m_maxPanicTPS` (提升 TPS 以加速清仓)
  - 清空 option markets
  - **不清空** future markets (保留对冲能力)
- 迁移后 panic 模式:
  - 取消所有, 直接返回
  - **不提升 TPS** (无法快速平仓)
  - **清空所有** (无对冲能力)
  - 所有仓位同时暴露于市场风险

**修复方案**:
- CTG 增加 `m_maxPanicTPS` 参数
- Panic 时: `txn_limit = m_maxTPS + m_maxPanicTPS`
- 仅清空 option markets, 保留 future markets

### B19: 排序算法关键偏差

**严重程度**: P0

**描述**: (经逐行验证, 详见对比)

**rankOption 类型权重差异**:
| 类型 | 原始 | 迁移 | 影响 |
|------|------|------|------|
| CANCEL | +1000 | +1000 | 相同 |
| UPDATE | +500 | +10 | 迁移版远低于原始 |
| NEW | 499-rank (反转, 低于UPDATE) | +100 (高于UPDATE) | **相反的优先级设计** |

原始 `NEW = 499 - rank` 的反转公式使 NEW 优先级低于 UPDATE (>500), 设计意图是"先更新现有订单, 再发新单"。迁移版 `NEW = +100` 使 NEW 高于 UPDATE (+10), **完全相反**。

**isBest 检查缺失**:
- 原始 (L583-590): `isBest(BID/ASK, our_mkt, mkt)` -> +5(最优) / +1(非最优)
  - `isBest` 用 `fade_price(s, mkt.getPrice(s), 1)` 做 1-tick 放松
- 迁移: **完全缺失** (文件中零个 `isBest` 符号)

**穿越检测价格偏差**:
- 原始 (L571-573): `midpx = mkt.getMidPrice()` — 市场最优买卖价中值
- 迁移 (L258): `midpx = od->values(0).theo()` — 理论价值
- 语义错误: 应基于市场价判断穿越, 不应基于理论值

**rankFuture 因子大量缺失**:
- 原始有 7 个因子: crossing(+10), isBest(+5), not-best(+1), flat-delta(+15), spread-tightness, +5(important), type-weight
- 迁移仅保留: crossing(+10), type-weight
- **6/7 因子缺失**

**修复方案**:
1. 恢复 `isBest` 辅助函数, 用 `fade_price` 1-tick 放松
2. 恢复 +5/+1 因子
3. 修正穿越检测: 用市场 mid 替代理论值
4. 恢复原始类型权重: `CANCEL=+1000, UPDATE=+500, NEW=499-rank`
5. 恢复 rankFuture 的全部 7 个因子

---

## 三、代码优化维度

### C1: 时间解析不一致且可能均不正确

**严重程度**: 高

**描述**: (经详细验证)
- `HftOptionStrategy.cpp` L500-506: 假设 `stra_get_time()` 返回 `HHMMSSmmm` (10位), 手动提取各位
- `UftOptionStrategy.cpp` L433: `stra_get_time() / 1000.0`, 假设返回毫秒计数
- **WT 框架标准**: `stra_get_time()` 返回 `HHMM` (4位, 如 0901), `stra_get_secs()` 返回 `SSmmm` (5位)
  - 来源: `WtEngine.h` L93-94: `_cur_raw_time` 注释为"当前真实时间"
  - `HftStraBaseCtx.cpp` L697-700: `stra_get_time()` -> `get_raw_time()` -> `_cur_raw_time`
  - `WtFutuCore/UftFutuMmStrategy.cpp` L1988 注释: "UFT: stra_get_time()=HHMM, stra_get_secs()=SSmmm"
- **结论**: 两个策略的时间解析都可能与 WT 标准不兼容
- 影响: FAST/SLOW 计算调度错误, TPS 限流计算错误

**修复方案**:
```cpp
// 确认 WT API 返回格式后, 在 OptionStrategyBase 中统一:
double getTimeInSeconds() {
    // WT 标准: stra_get_time()=HHMM, stra_get_secs()=SSmmm
    uint32_t hhmm = _ctx->stra_get_time();
    uint32_t ssms = _ctx->stra_get_secs();
    uint32_t hh = hhmm / 100;
    uint32_t mm = hhmm % 100;
    uint32_t ss = ssms / 1000;
    uint32_t ms = ssms % 1000;
    return hh * 3600.0 + mm * 60.0 + ss + ms / 1000.0;
}
```

### C2: TPS 限流逻辑缺陷

**严重程度**: 中

**描述**:
- `CTG::drainPendingQuotes()` 检测超限但设置 `cancelOnly = true` 后继续遍历
- 虽然 `otd->updateOrders(pq.isCancel || cancelOnly)` 正确传递了 flag
- 但 NEW 订单未被显式跳过, 可能导致不必要的处理

**修复方案**: 超限时仅处理 CANCEL, 跳过 NEW/UPDATE

### C3: OptionList.h 残留 boost::multi_index

**严重程度**: 低

**描述**: 已用 vector+unordered_map 替代, 但模板文件仍在

**修复方案**: 删除 `OptionList.h`

### C4: 裸指针所有权不清

**严重程度**: 低

**描述**: CTG 持有 `OptionTradingGrid*` 裸指针

**修复方案**: 改为 shared_ptr 或明确注释所有权

### C5: TODO 标记未完成

**严重程度**: 高

**描述**: 共发现 22处 TODO + 25处 "not yet migrated" 标记 (详见 A3)

**修复方案**: 逐一完成或明确记录为已知限制

### C6: IScanModule.h 重复定义

**严重程度**: 中

**描述**: 根目录和 Scanners/ 子目录各一份, 接口不同 (详见 A6)

**修复方案**: 删除根目录副本, 统一引用 Scanners/ 版本

### C7: SignalFactory 重复

**严重程度**: 低

**描述**: 根目录 (`SignalFactory.h`) 和 Signals/ 子目录 (`SignalFactory.h/.cpp`) 各一份

**修复方案**: 删除根目录副本

### C8: tradingStopMidDay 接口已实现但缺自动调度

**严重程度**: 中

**描述**: (修正) 函数本身已实现, 但缺少自动调度 (详见 B4)

**修复方案**: 增加自动调度, 或如果手动调用则文档说明

### C9: WtOptContext::update_pnl() 空实现

**严重程度**: 中

**描述**: (经验证) 函数体完全为空, 仅两行注释

**修复方案**: 明确 PnL 跟踪责任归属, 或在 context 层实现

### C10: OptionData MAX_VALUES=5 双缓冲未充分使用

**严重程度**: 低

**描述**: 仅用 slot 0, slot 2 的 multi-source 合并未实现 (详见 B13)

### C11: WtOptEngine::on_minute_end() 被注释

**严重程度**: 中

**描述**: (新发现) L249 整个函数体被注释掉, 分钟结束事件不分发到 context, 影响:
- 定时 curve fitting 无法触发
- 分钟级 PnL 更新无法触发

**修复方案**: 恢复 `on_minute_end()` 事件分发

### C12: 合约信息未接入

**严重程度**: 中

**描述**: (新发现)
- `CompositeOptionPricer.cpp` L594: `black_values.m_fees = 0` (硬编码)
- `CompositeOptionPricer.cpp` L743: `bid_tick_size = 1.0` (硬编码)
- 应从合约信息获取

**修复方案**: 接入 WT 的 `WTSSessionInfo` / `WtCommInfo` 获取费用和 tick_size

---

## 四、性能提升维度

### P1: 每个 tick batch 全量计算所有期权

**严重程度**: P0

**描述**:
- 数百合约 × BS定价 + Greeks 计算 = 高 CPU
- 与 B3 (防抖缺失) 和 B19 (排序偏差) 叠加影响

**修复方案**:
1. 标记脏合约 (dirty set) — 仅市场变化的合约
2. computeValues 仅计算脏合约 + 受影响的到期月
3. GVV 曲线仅当 ATM 合约变化时重新拟合
4. 增加前月合约触发 + 20ms 防抖 (修复 B3)

### P2: 排序算法缺 isBest 导致非最优排序

**严重程度**: P1

**描述**: 不必要的订单修改, 浪费 TPS (详见 B19)

**修复方案**: 恢复 isBest 因子

### P3: OptionAsyncEventProcessor 无优先级队列

**严重程度**: P1

**描述**:
- 单一 `boost::lockfree::spsc_queue<AsyncEvent, capacity<4096>>`
- CANCEL 与 NEW 同优先级, 应优先 CANCEL
- 高峰期队列饱和 (78% 阈值监控)

**修复方案**:
```
替换为多优先级队列:
- Priority 0 (最高): CANCEL, PANIC
- Priority 1: TRADE, ORDER 回调
- Priority 2: TICK
- Priority 3: TIMER, FIT
```

### P4: boost::object_pool 非线程安全

**严重程度**: P0

**描述**: `OptionOrder` 使用 `boost::object_pool<OptionOrder>` 单例池, 多线程下可能 crash

**修复方案**:
- 使用 `tbb::concurrent_pool` 或加锁的 `boost::pool`
- 或简化: 使用 `std::shared_ptr` + `make_shared`

### P5: OptionRisk::update() 全量遍历

**严重程度**: P1

**描述**: 每次计算所有到期月所有合约 Greeks

**修复方案**:
1. 仅重算有变化的 OptionRiskData
2. 到期月级别增量聚合 (而非全局重算)

### P6: 无增量更新机制

**严重程度**: P2

**描述**:
- 原始 WorkScheduler 异步更新丢失
- `onOptionHit` 仅插入 set, 无异步处理
- 下一个 `refresh()` 周期才处理

**修复方案**: 恢复 WorkScheduler 等效机制, 或在 refresh 中优先处理 optUpdateSet

### P7: 锁竞争: WtOptContext worker thread

**严重程度**: P2

**描述**: 单线程消费 lock-free queue, 高峰期可能饱和

**修复方案**: 监控队列深度, 考虑多 worker 线程 (需保证策略线程安全)

---

## 五、实施路线图

### 阶段一: 关键修复 (P0, 预计 2-3 周)

| # | 任务 | 对应问题 | 预估工时 |
|---|------|---------|---------|
| 1 | 修复排序算法偏差 (B19) | 恢复 isBest、修正类型权重、修正穿越检测 | 2天 |
| 2 | 完成 OptionGrid <-> OptionRisk 集成 (A3) | 清除 TODO, 实现 expired/getAllOptions/addListener | 3天 |
| 3 | 完成 OptionPricer 实现 (A3) | computeForwardPrice, computeValues, finalizeCompute | 3天 |
| 4 | 恢复 notifyMarketsPriced 调用链 (A3) | OptionPricer2 + CompositeOptionPricer | 1天 |
| 5 | 恢复 on_minute_end() 事件分发 (C11) | WtOptEngine | 0.5天 |
| 6 | 修复 TPS 限流逻辑 (C2) | drainPendingQuotes 超限时跳过 NEW | 0.5天 |
| 7 | 修复时间解析不一致 (C1) | 确认 WT API 格式, 统一解析 | 1天 |
| 8 | 线程安全内存池 (P4) | OptionOrder object_pool | 1天 |
| 9 | 接入合约信息 (C12) | fees, tick_size | 1天 |

**阶段一验收标准**:
- 所有 TODO 标记清除或明确记录为已知限制
- OptionPricer.cpp 可正常编译链接
- 排序结果与原始逻辑一致 (单元测试验证)
- 时间解析在回测和实盘下均正确

### 阶段二: 业务逻辑恢复 (P1, 预计 3-4 周)

| # | 任务 | 对应问题 | 预估工时 |
|---|------|---------|---------|
| 10 | 恢复 Panic Signal 自动检测 (B1) | 双阈值, onPanic 传播 | 2天 |
| 11 | 恢复 Compute 防抖 + 增量计算 (B3, P1) | dirty set, 20ms timer, 前月触发 | 3天 |
| 12 | 恢复 Mid-Day 停止自动调度 (B4) | 基于品种 WTSSessionInfo 的 section 边界检测, 可配置提前量/延迟 | 1.5天 |
| 13 | 恢复 Late Fill 检测 (B5) | OptionQuoteManager | 1天 |
| 14 | 恢复 Multi-level Market (B12) | MultiMarket 扩展, check_markets 多档 | 3天 |
| 15 | 恢复 Panic TPS 增强 (B18) | m_maxPanicTPS, 保留 future markets | 1天 |
| 16 | 接入 Scanner 到 HFT/UFT 主流程 (A6) | 统一 IScanModule, setupScanners | 3天 |
| 17 | 恢复 Risk Free Rate Curve (B2) | IVolCurve rate curve, 配置数据点 | 2天 |
| 18 | 增量 Greeks 更新 (P5) | 仅重算变化部分 | 2天 |

**阶段二验收标准**:
- Panic 可由信号自动触发, 且 TPS 提升
- computeValues 仅在需要时调用 (CPU 降低)
- Scanner 在 HFT/UFT 中可创建和接收事件
- 排序考虑多档行情和 isBest

### 阶段三: 架构优化 (P2, 预计 2-3 周)

| # | 任务 | 对应问题 | 预估工时 |
|---|------|---------|---------|
| 19 | 抽取 OptionStrategyBase (A1, A4) | 消除 HFT/UFT 90% 重复 | 3天 |
| 20 | 拆分 WtOptContext (A5) | OptionEventLoop + CheckpointManager | 2天 |
| 21 | 优先级事件队列 (P3) | 多优先级 AsyncEvent | 2天 |
| 22 | 恢复 Front Month 换月 (B6) | on_session_begin 重评估 | 1天 |
| 23 | 恢复 Drop Tracking + Retry (B16) | txDrop 计数 + 重试调度 | 1天 |
| 24 | 清理重复文件 (C3, C6, C7) | 删除 OptionList.h, 统一 IScanModule.h | 0.5天 |
| 25 | 恢复 UnderlyingTradingData <-> ExpiryData 连接 (A3) | distort values | 2天 |

**阶段三验收标准**:
- HFT/UFT 代码重复 < 20%
- WtOptContext 行数 < 400
- 事件按优先级处理

### 阶段四: 完善补充 (P2-P3, 预计 2-3 周)

| # | 任务 | 对应问题 | 预估工时 |
|---|------|---------|---------|
| 26 | 恢复 AttributePublisher 适配 (B7) | WT 监控体系适配 | 2天 |
| 27 | 恢复 ManualOrderManager (B8) | hot-param 命令机制 | 2天 |
| 28 | 恢复 Multi-source Market 合并 (B13) | take_inner slot 0+2 | 2天 |
| 29 | 恢复 Secondary Hedge (B14) | 多对冲合约 | 1天 |
| 30 | 恢复 Active Counter (B17) | 统计计数器 | 0.5天 |
| 31 | 恢复 WorkScheduler 增量更新 (P6) | 异步处理 onOptionHit | 2天 |
| 32 | 明确 PnL 跟踪责任 (C9) | context vs strategy | 1天 |

**阶段四验收标准**:
- 可通过外部接口监控 Greeks/位置
- 可手动下单和撤单
- 多源行情可合并

### 阶段五: 高级功能 (P3, 按需)

| # | 任务 | 对应问题 | 预估工时 |
|---|------|---------|---------|
| 33 | ExpirationSimulator 适配 (B9) | 简化版组合 PnL 模拟 | 2天 |
| 34 | OptionValueWriter (B10) | 定时值记录 | 1天 |
| 35 | Predictor 基础设施 (B11) | IPredictor + TriggerEngine | 3天 |
| 36 | Expiry Readiness Tracking (B15) | fwd/fit ready 通知 | 1天 |
| 37 | GvvVolCurve GSL 依赖解决 (A3) | WLS3 替代 GSL | 已完成, 验证 |
| 38 | 增加单元测试 | CTG/ranking/risk | 3天 |

---

## 六、风险与注意事项

### 6.1 不破坏现有可用功能
- 修复应增量进行, 每步回归测试
- 保留 `WtOptionStrategy` (legacy) 路径作为 fallback
- HFT/UFT 修改前确保 WtOptionStrategy 仍可运行

### 6.2 HFT/UFT 同步
- 在 OptionStrategyBase 抽取完成前 (阶段三), 两个文件需同步修改
- 每次修改 HftOptionStrategy 后, 检查 UftOptionStrategy 是否需要相同修改

### 6.3 配置兼容性
- 新增配置项需有默认值, 保证旧配置文件不 break
- `option_config_demo.json` 应同步更新并测试

### 6.4 测试覆盖
- 当前仅有 `test_blackscholes` 和 `test_optiongrid`
- 阶段一应增加: CTG ranking 测试, TPS 限流测试, 时间解析测试
- 阶段二应增加: panic signal 测试, scanner 接入测试
- 建议建立性能基线: tick 处理延迟, CPU 占用, 内存

### 6.5 WT 框架兼容性
- 确认 `stra_get_time()` / `stra_get_secs()` 的确切返回格式
- 确认 `stra_get_comminfo` 可获取的费用和 tick_size 信息
- 确认 `TraderAdapter::quote` 的接口和行为

### 6.6 优先级说明
- P0: 影响正确性和可用性, 必须优先
- P1: 影响业务逻辑完整性, 应尽快
- P2: 影响完整性和可维护性, 计划内
- P3: 增强功能, 按需

---

## 附录: 问题索引

| 编号 | 维度 | 严重程度 | 简述 |
|------|------|---------|------|
| A1 | 架构 | 高 | HFT/UFT 代码重复 90% |
| A2 | 架构 | 中 | CTG 职责过载 |
| A3 | 架构 | 高 | OptionGrid/Risk/Pricer 集成未完成 (22处TODO) |
| A4 | 架构 | 中 | 缺少策略基类 |
| A5 | 架构 | 中 | WtOptContext 职责膨胀 |
| A6 | 架构 | 高 | Scanner 未接入 HFT/UFT + 接口冲突 |
| B1 | 业务 | P0 | Panic Signal 自动检测缺失 |
| B2 | 业务 | P0 | Risk Free Rate Curve 缺失 |
| B3 | 业务 | P0 | Compute 防抖触发缺失 |
| B4 | 业务 | P1 | Mid-Day 自动调度缺失 (基于品种 Session) |
| B5 | 业务 | P1 | Late Fill 检测缺失 |
| B6 | 业务 | P1 | Front Month 换月缺失 |
| B7 | 业务 | P2 | AttributePublisher 缺失 |
| B8 | 业务 | P2 | ManualOrderManager 缺失 |
| B9 | 业务 | P2 | ExpirationSimulator 缺失 |
| B10 | 业务 | P2 | OptionValueWriter 缺失 |
| B11 | 业务 | P3 | Predictor 缺失 |
| B12 | 业务 | P1 | Multi-level Market 缺失 |
| B13 | 业务 | P2 | Multi-source 合并缺失 |
| B14 | 业务 | P2 | Secondary Hedge 缺失 |
| B15 | 业务 | P3 | Expiry Readiness 缺失 |
| B16 | 业务 | P2 | Drop Tracking + Retry 缺失 |
| B17 | 业务 | P3 | Active Counter 缺失 |
| B18 | 业务 | P1 | Panic TPS 增强缺失 |
| B19 | 业务 | P0 | 排序算法关键偏差 |
| C1 | 代码 | 高 | 时间解析不一致且可能均不正确 |
| C2 | 代码 | 中 | TPS 限流逻辑缺陷 |
| C3 | 代码 | 低 | OptionList.h 残留 |
| C4 | 代码 | 低 | 裸指针所有权不清 |
| C5 | 代码 | 高 | TODO 标记未完成 |
| C6 | 代码 | 中 | IScanModule.h 重复 |
| C7 | 代码 | 低 | SignalFactory 重复 |
| C8 | 代码 | 中 | tradingStopMidDay 缺自动调度 |
| C9 | 代码 | 中 | update_pnl 空实现 |
| C10 | 代码 | 低 | MAX_VALUES 未充分使用 |
| C11 | 代码 | 中 | on_minute_end 被注释 |
| C12 | 代码 | 中 | 合约信息未接入 |
| P1 | 性能 | P0 | 全量计算 CPU 浪费 |
| P2 | 性能 | P1 | 排序非最优浪费 TPS |
| P3 | 性能 | P1 | 无优先级事件队列 |
| P4 | 性能 | P0 | object_pool 非线程安全 |
| P5 | 性能 | P1 | Greeks 全量遍历 |
| P6 | 性能 | P2 | 无增量更新机制 |
| P7 | 性能 | P2 | worker thread 锁竞争 |
