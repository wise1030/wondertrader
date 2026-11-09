# WtOptionCore — WonderTrader 期权做市引擎

WtOptionCore 是 WonderTrader 量化交易平台下的**期权做市引擎**，从 quantbox/optiontrader（longbeach 框架）迁移而来。以 C++17 共享库（`libWtOptionCore.so`）形式被 WtRunner/WtBtRunner 以 HFT 策略工厂插件动态加载，实现完整期权做市管线：

**行情接入 → 异步事件处理 → 3 级合约网格 → 平价合成远期 + Black76 反推 IV → GVV 波动率曲面拟合 → 组合 Greeks 风控 → 双边报价/对冲执行 → PnL 跟踪**，并附带 9 类套利/做市扫描器与一整套回测验证工具链。

支持三种期权业务场景：

| 场景 | 交易所 | 定价标的 | 对冲标的 |
|------|--------|---------|---------|
| 商品期货期权 | SHFE / DCE / INE | 对应月份期货 | 同一期货合约 |
| 郑州商品期权 | CZCE | C/P 双品种自动发现（如 SRC+SRP） | 对应期货 |
| 股指期权 | CFFEX IO/MO/HO | 现货指数（不可交易） | 股指期货（分月对冲） |

---

## 目录

1. [系统架构](#系统架构)
2. [核心模块](#核心模块)
3. [IV 定价链路（平价远期 → Black76 → GVV）](#iv-定价链路)
4. [线程模型与状态归属](#线程模型与状态归属)
5. [配置参考](#配置参考)
6. [回测与 Replay 验证](#回测与-replay-验证)
7. [构建与测试](#构建与测试)
8. [文件清单](#文件清单)
9. [迁移历史](#迁移历史)

---

## 系统架构

```
Market Tick / Order / Trade / Position 回调 (WtRunner 主线程或回测 replayer)
    │
    ▼
OptionAsyncEventProcessor                ← 回调只入队，绝不在回调线程直写状态
    │  双队列: MD SPSC 无锁队列 + Trader mutex 队列
    │  事件优先级桶: Session > Channel > Position > Trade > Order > Timer > Tick
    │  Tick 按合约去重 (保留最新)  → batch
    ▼
Worker 线程 (唯一状态写者)
    │
    ├── on_tick → OptionGrid → 合约自动发现 / 盘口快照 / per-expiry 标的路由
    │
    ├── on_timer → PeriodicCurveFitter → AsyncVolFitter(SingleFlightTaskRunner)
    │       └── 平价合成远期 + Black76 反推 IV → GVV 拟合 → 发布曲线
    │
    ├── on_batch_complete (始终执行, 防抖)
    │       └── CompositeOptionPricer.computeValues()  (FAST/SLOW 双速)
    │             ├── initValuesCompute (forward / maturity / discount)
    │             ├── BlackCalc (Black76) × GVV 曲线 → 理论价 + Greeks
    │             ├── alpha 信号调整 (vega/delta/ATM flow/front spread/roll)
    │             └── computeOurMarkets → desired bid/ask
    │
    ├── ControllableTradingGrid.refresh()
    │       └── check_markets → 排序(isBest/crossing/δ/spread/expiry) → TPS 限流
    │             └── OptionQuoteManager.updateOrders()
    │                   ├── RiskFilterChain (5 过滤器)
    │                   ├── PositionGuard / FillPriceChecker 预检
    │                   └── 增量 diffing → OrderGateway → stra_quote/buy/sell
    │
    ├── OptionRisk → 组合 Greeks 聚合 → 风险限额 → panic / widen
    ├── OptionScanCoordinator → 9 类 scanner 派发 → ScannerHit → CTG 执行
    ├── OptionHedgeCoordinator → delta 对冲决策 / 冷却 / 在途抑制
    └── PnlTracker → mark-to-mid → PnlLimitSignal → 自动 panic
```

### Grid 三层状态归属

| 层 | 拥有的状态 | 写入者 | 读取者 |
|---|---|---|---|
| `OptionGrid` | 合约发现、expiry/strike、盘口快照、定价输出 | async worker | pricer、scanner、strategy |
| `OptionTradingGrid` (OTD/UTD) | 每 option/underlying 的 OQM（订单、持仓、QuoteMode） | worker | CTG、strategy |
| `ControllableTradingGrid` (CTG) | desired quote/执行、scanner 注册、TPS/状态机 | worker | CTG 自身、strategy |

约束：MD/交易回调只入队；`HftOptionStrategy` 作为 Shell 只做装配/生命周期/回调路由。

### 策略 pricer 边界

- `pricerType=composite_mm` → `CompositeOptionPricer`：做市报价 + alpha + 风控调整
- `pricerType=gammascalp` → `GammaScalpOptionPricer`：gamma scalping + 动态对冲
- 两者共享 `OptionPricer2` 的 Black/GVV/vol 内核，策略状态与报价逻辑各自独立，禁止合并

---

## 核心模块

### 1. 引擎入口层

| 组件 | 文件 | 职责 |
|------|------|------|
| HftOptionStrategy | HftOptionStrategy.h/.cpp | HFT 策略入口（Shell）：装配所有组件、生命周期、回调路由、executeQuote/Order/Cancel 桥接到 IHftStraCtx |
| HftOptionStraFact | HftOptionStraFact.cpp | DLL 工厂，供 WtRunner `strategies.hft[]` 动态加载 |

### 2. 异步事件处理

| 组件 | 文件 | 职责 |
|------|------|------|
| OptionAsyncEventProcessor | OptionAsyncEventProcessor.h/.cpp | 双队列（MD SPSC + Trader mutex）、优先级桶排序、tick 按合约去重、overflow 有界背压、`on_batch_complete` 始终执行、worker 异常兜底 |
| SingleFlightTaskRunner | SingleFlightTaskRunner.h | 昂贵任务（曲线拟合）单飞执行，过期快照直接丢弃 |
| LatencyHistogram | LatencyHistogram.h | tick→quote 延迟直方图，batch 日志输出 p50/p95/p99/p99.9 |
| OptionTimeService | OptionTimeService.h | 连续时钟（跨午夜回绕检测）、FAST/SLOW 计算调度、timer 100ms 源头合并 |
| TradingState | TradingState.h | 统一交易状态机（Normal/Recovering/Disabled + panicked），enabled/panicked 唯一写者 |

### 3. 数据网格（3 级：Expiry → Strike → Call/Put）

| 组件 | 文件 | 职责 |
|------|------|------|
| OptionGrid | OptionGrid.h/.cpp | 3 级网格、tick 驱动合约自动发现、per-expiry 标的路由、平价合成 ATM forward（`__getBestSyntheticPrice`）、`refreshExpiryDays` |
| OptionData | OptionData.h/.cpp | 单合约：盘口/last、计算值双缓冲（atomic swap）、`getMid()`（双边缺失回退 last） |
| ExpiryData | ExpiryData.h/.cpp | 日历（节假日/周末感知交易日计数）、forward、maturity（下限 1/252）、discount factor、利率曲线 |
| StrikeData | StrikeData.h/.cpp | 行权价层 call/put 对（scanner 与 parity 策略依赖 sibling 腿） |
| OptionTradingData / UnderlyingTradingData | OptionTradingData.* / UnderlyingTradingData.* | 交易域状态：OQM、持仓、QuoteMode、multiMarket |

### 4. 定价与波动率

| 组件 | 文件 | 职责 |
|------|------|------|
| BlackCalc | BlackCalc.h/.cpp | 统一 BS/Black76 计算：d1=ln(F/K)/σ√T+½σ√T，value/delta/gamma/vega/vanna/theta/volga/rho |
| BlackImpliedCalculator | BlackImpliedCalculator.h/.cpp | Black76 隐含波动率 Brent 求解（PriceError 残差函数） |
| OptionPricer2 | OptionPricer2.h/.cpp | 定价内核：forward/maturity/discount 装配、理论价 + Greeks、**per-strike 平价合成远期反推 IV**（`computeImpliedValues`）、支持 OpenMP |
| GvvVolCurve | GvvVolCurve.h/.cpp | GVV 波动率曲面：WLS3 栈上回归 + Brent α 搜索、α=1 闭式快路径、log-moneyness 约定 x=ln(K/F)、归一化输出 iv/atmVol、判别式负值防护 |
| LinearVolCurve / ConstantVolCurve / PowerVolCurve | 同名文件 | 备用曲线：线性 / 常数 / 幂律双曲线 |
| PeriodicCurveFitter | PeriodicCurveFitter.* | 拟合调度（period/decay_window/start/end、good-points 阈值、ups+downs 象限检查） |
| AsyncVolFitter | AsyncVolFitter.h | 快照收集 → SingleFlightTaskRunner 异步拟合 → 完成后发布曲线版本 |
| IVolCurve | IVolCurve.h | 曲线接口（dataset_t / fit / eval） |

### 5. 执行与订单

| 组件 | 文件 | 职责 |
|------|------|------|
| ControllableTradingGrid | ControllableTradingGrid.* | 执行调度：refresh → check_markets → 排序（isBest+5/+1、crossing+10、delta、spread、expiry、类型权重）→ TPS 限流（panic 增强）→ dropped-quote 保留重试 |
| OptionQuoteManager | OptionQuoteManager.* | 单合约订单生命周期：增量价位 diffing、撤单三层节流、STP 自成交保护、late-fill 检测、cancel ack 看门狗（`cancel_ack_timeout_ms`+重试）、wait-for-cancels、谨慎翻面、TPS 实返计数 |
| OrderGateway | OrderGateway.h | 下单/撤单统一出口（实盘→IHftStraCtx，回测→HftMocker） |
| OptionTradingGrid | OptionTradingGrid.* | OTD/UTD 容器、QuoteExecutor 桥 |
| BaseOrder / OptionOrder | BaseOrder.* / OptionOrder.* | 订单状态机、ValuesAtIssue、腿级 PnL |
| OptionOrderInfo / FutureOrderInfo | 同名文件 | 订单快照 |
| ComboOrders | ComboOrders.* | 多腿组合单：SpreadCombo（两腿）/ SynCombo（三腿）；**同时多腿发出**，任一腿撤/拒→撤其余腿并置 FAILED，终态要求 filledQty≥desiredQty 或残量匹配，杜绝裸腿 |
| check_markets | check_markets.* | desired vs current 市场比较（UT_NONE/NEW/MODIFY/CANCEL） |

### 6. 风控

| 组件 | 文件 | 职责 |
|------|------|------|
| OptionRisk | OptionRisk.* | 组合 Greeks 聚合、hedge 仓位维护、风险限额 |
| OptionRiskData | OptionRiskData.* | 单合约仓位 Greeks（dirty flag 增量更新） |
| OptionExpiryGreeks | OptionExpiryGreeks.* | 到期月 Greeks 聚合 |
| RiskFilterChain | RiskFilterChain.* | 5 个可组合过滤器（MaxOrderSize/MinSellPrice/MaxPosition/MaxCancel/MaxNewOrders），配置驱动、REJECT 短路 |
| RiskLimitsEx | RiskLimitsEx.* | 预交易风控：单笔/金额限制、clearly-erroneous 价格检查、Greeks 限额（Delta/Gamma/Vega/DailyLoss）、涨跌停护栏 |
| PositionGuard | PositionGuard.* | internal fill 累计 vs broker 持仓对比，偏差超容忍→禁用+告警（tolerance 默认 5 手，覆盖回报延迟窗口） |
| PositionOffsetMgr | PositionOffsetMgr.* | 策略侧可平量跟踪（平今/平昨/开仓拆分）、冻结量管理 |
| FillPriceChecker | FillPriceChecker.* | 成交价偏离监控（warning/panic 阈值、latch 直至清除） |
| IssuedOrderRegistry | IssuedOrderRegistry.h | 单一 per-order 登记表：unknown-fill 计数告警/升级、done-before-fill 迟到分类、overfill 检测、成交质量归因数据源 |
| StressAnalyzer | StressAnalyzer.* | spot/vol/theta 情景压力（周期输出 STRESS worst） |

### 7. 扫描器（Scanners/，9 类）

统一经 `OptionScanCoordinator` 生命周期管理（`setEnabled → onStart → onOptionUpdate/onUnderlyingUpdate → onStop/onPanic`），分数 > `min_edge` 时 `publishHit` 发 `ScannerHitEvent` 到 CTG 执行；支持 `delta_min/max`、`max_spread`、`min_size`、GARCH `warmup_returns` 等数值参数；`expiryOverrides` 按 expiry 过滤。

| Scanner | 说明 |
|---------|------|
| MMScanner | 做市机会 |
| StrikeSpreadScanner | 同行权价 C/P 价差 |
| VolSpreadScanner | 波动率价差（两腿同时执行） |
| ButterflyScanner | 蝶式（三腿同时执行） |
| SyntheticFutureScanner | 合成期货 vs 期货 |
| SimplexScanner | 单纯套利 |
| OpenScanner | 开盘机会 |
| LowBidsScanner | 低价买盘异常 |
| GarchScanner | GARCH 波动率预测信号 |

> **已注册未默认启用**：9 个 Scanner 全部完成工厂注册与加载链接线，shipped 配置默认只启用 MMScanner。
> 其余（VolSpread/Butterfly/SyntheticFuture/Simplex/Open/LowBids/Garch/StrikeSpread）在
> `config/option_strategy_full.json` 中给出了 `enable:false` 的现成配置段——改为 `true` 即接线，
> 无需改代码。其中 Open/Simplex/StrikeSpread/LowBids 四个 scanner 共享 `EdgeScannerBase` 实现
> （仅 ident 与 hit-reason 不同）。

### 8. 信号（Signals/）

- **Alpha 信号**（AlphaSignals.*）：VegaFlow / DeltaFlow / ATM 信号 / ForwardSpread / RollEMA — 驱动报价偏移
- **Risk 信号**（RiskSignals.*）：ToxicitySignal（逆向成交毒性→自动 widen/panic）、PnlLimitSignal（日内 PnL 限额→auto panic）
- **SignalFactory**：信号工厂与数值参数接线

> **已注册未默认启用**：Alpha 信号共 7 个（VegaFlow/DeltaFlow/AtmSig/RollEma/FrontFutSkew/
> FrontAtmvFlow/ForwardSpread），默认配置只启用前两个；其余 5 个在 full.json 中有
> `enable:false` 现成配置段。Risk 信号（Toxicity/PnlLimit）默认启用。
>
> **可选模式**：`pricerType: "gammascalp"` 激活 GammaScalpOptionPricer（默认 composite_mm）；
> `pricer.blackCalc.volCurve.fitter.mode: "sync"` 走同步拟合链 PeriodicCurveFitter
> （默认 async，异步拟合在独立线程）。两者均完整接线，仅默认休眠。

### 9. 协调器与命令

| 组件 | 文件 | 职责 |
|------|------|------|
| OptionScanCoordinator | OptionScanCoordinator.* | scanner 生命周期/派发单一归属 |
| OptionHedgeCoordinator | OptionHedgeCoordinator.* | delta 对冲决策、冷却、在途抑制（上一批未终态不重复发单） |
| OptionExpiryLifecycle | OptionExpiryLifecycle.* | session begin refresh/purge/roll、过期合约 deactivate 停止报价、session end cancelAll+持仓告警 |
| OptionCommandHandlers | OptionCommandHandlers.* | STOP/RESUME/PANIC/QMODE/ORDER/CANCEL 热指令 |
| CommandBus | CommandBus.h | 热指令总线（异常兜底） |
| ConfigValidator | ConfigValidator.h | 启动配置校验，未知键 WARN |

### 10. 辅助

| 组件 | 文件 | 职责 |
|------|------|------|
| PnlTracker | PnlTracker.* | 单合约 tick-by-tick mark-to-mid PnL（FIFO + 加权入场价） |
| QuoteStatistics | QuoteStatistics.* | 回调驱动报单统计：双边时长、加权价差、fill/cancel/reject 率、报价延迟（µs）、会话级报告 |
| ExpirationSimulator | ExpirationSimulator.* | 到期 PnL 模拟 |
| OptionValueWriter | OptionValueWriter.* | 定时 CSV 记录理论价/Greeks |
| AttributePublisher | AttributePublisher.* | 结构化属性发布 |
| PredictionEvaluator / Predictor | PredictionEvaluator.h / Predictor.h | 价格预测闭环（IPredictor + TriggerEngine，hitRate/MAE 评估） |
| StrikeFinders | StrikeFinders.cpp + CffexIOStrikeFinder.h / DceMOStrikeFinder.h / IStrikeFinder.h | 各交易所行权价/合约查找 |

---

## IV 定价链路

期权 IV 与波动率曲面的完整计算链（**平价合成远期 → Black76 反推 → GVV 拟合**）：

### ① 平价合成远期（Put-Call Parity）

两个层面，公式均为 `F_syn = K + (C_mid − P_mid) / discount`（discount = e^(−rT)）：

| 层面 | 位置 | 过滤条件 |
|------|------|---------|
| expiry 级 ATM forward | `OptionGrid::__getBestSyntheticPrice` | **双边四价齐全**（call/put bid+ask > 0）；ask/bid < 10 异常剔除；1/spread 加权；与期货 mid 加权合并 |
| per-strike forward（IV 反算用） | `OptionPricer2::computeImpliedValues` | 同 strike 的 call/put **双边齐全**（单边时 `getMid()` 会回退陈旧 last 价污染合成远期，故要求四价） |

### ② Black76 反推 IV

`BlackImpliedCalculator::volatility(price)`：PriceError 残差 `Black76(K, F_syn, σ√T) − marketPrice`，Brent 求解。per-strike 合成远期消除了行权价层面的盘口偏置。`bid/ask/mid` 三价各反推一次，价差超 `bid_offer_vol_spread_cap` 时置 -2（该合约不进拟合）。

### ③ GVV 拟合

- `submitAsyncVolFit` 收集拟合点：每 strike 取 **OTM 腿**（C/P 中 mid 较小者）的 `iv/atmVol`，x = K − F
- `GvvVolCurve::fit`：WLS3 加权最小二乘（栈上零分配）+ BrentMinimize 搜索 α；good-points 阈值按到期天数插值
- `eval`：x = ln(K/F_atm)（log-moneyness），解 GVV 隐式方程求 σ(K)，输出归一化 σ/σ_ATM；α=1 时走闭式快路径
- 失败防护：解 bracket 失败回退 ATM（告警节流）；maturity≤0 返回 1.0；拟合点 <4 或 ATM vol 越界拒绝拟合

### Forward 防崩保护（P0）

- 平价点不足 5 个 strike → `forwardReady=false`，定价跳过，**不回退标的价格**
- `ExpiryData::getMaturity()` 下限 1/252；`updateDaysToExpiration()` bdays=0 且 calendarDays>0 时取 1
- `computeImpliedValues()` forward/maturity/discount 非正或 NaN 时跳过 IV
- `refreshExpiryDays()` 每 session begin 刷新全部 expiry 的剩余天数（防近月 maturity 冻结）
- 回测模式（bdMgr=null）holidays.json 按候选路径加载，且周末感知（与实盘 `IBaseDataMgr::isHoliday` 语义对齐）

---

## 线程模型与状态归属

```
TraderAdapter 回调线程 / 回测 replayer      ShareManager 监控线程        Async Worker 线程（唯一状态写者）
    │                                           │                             │
    ├─ on_tick (enqueue MD)                     ├─ on_params_updated          ├─ 事件桶排序 + tick 去重
    ├─ on_trade/on_order (enqueue Trader)       │  (atomic 参数)              ├─ Session/Channel/Position
    ├─ on_position (enqueue)                                                  ├─ Trade/Order/Timer
    └─ on_session_begin/end (enqueue)                                         ├─ grid 快照更新
                                                                              ├─ vol fit 调度/发布
                                                                              ├─ computeValues (防抖)
                                                                              ├─ risk update + panic
                                                                              ├─ scanner dispatch
                                                                              └─ CTG refresh + drain + 下单
```

**线程安全保证**：

- `_positions` / `_pnlPendingInit`：仅 worker 访问（回调已改为 enqueue）
- `_traderCtx->enabled` / `panicked` / `_channelReady` / `_initialized` / `_tickCount`：`std::atomic`
- `OptionGrid` 容器：`std::shared_mutex`
- 热参数：`on_params_updated` 只置位，命令/TPS/QMode 在 async worker 应用，避免跨线程直写 grid/CTG
- worker_loop：`try/catch` 包裹，单 batch 异常只跳过该 batch

### Worker batch 处理顺序

```
1. on_session (begin/end)                    5. on_order
2. on_channel (ready/lost)                   6. on_timer (fit 调度/盘中停复/超时重算)
3. on_position (broker 持仓)                 7. on_tick_batch → per-tick 快照更新
4. on_trade (fill)                           8. on_batch_complete（始终执行）
                                                ├─ position sync / PnlTracker
                                                ├─ computeValues（_underlyingChanged || _needsRefresh）
                                                ├─ CTG refresh → OQM updateOrders → 下单
                                                └─ attribute publish / scanner
```

### 关键设计决策

1. **回调只入队** — MD/交易回调绝不在回调线程直写 grid/CTG 状态
2. **on_batch_complete 始终执行** — 无 tick 也能处理 trade/position/order 事件
3. **eager compute 已移除** — 定价统一由 batch 防抖控制（minComputeInterval）
4. **tradingStopMidDay 无条件 drain** — CANCEL 立即发出，不受 enabled 门控
5. **dropped-quote 保留重试** — TPS 丢弃的报价不清除，下周期重发
6. **channel_ready 延迟启用** — 等首个标的 tick 到达才真正激活交易（恢复门）
7. **session_end 清场** — 显式 cancelAll 并告警非零 gross 持仓；OQM 计数器 session 复位
8. **多腿组合原子性** — SpreadCombo/SynCombo 同时发出，残腿自动撤销，终态严格校验

---

## 配置参考

完整配置参见 `config/option_strategy_full.json` / `config/option_strategy_advanced.json`。启动时 `ConfigValidator` 校验，未知键 WARN。

### 顶层键

| 配置项 | 类型 | 默认值 | 说明 |
|--------|------|--------|------|
| exchange | string | - | 交易所 (SHFE/CFFEX/CZCE/DCE/INE) |
| futuresProduct | string | - | 期货品种 (cu/IF/SR…) |
| optionProduct | string | 自动推导 | 期权品种；不配则按交易所规则推导（见下表） |
| underlyingCode | string | - | 全局定价标的 |
| underlyingType | string | "future" | "future" 或 "index" |
| riskFreeRate / riskFreeRateCurve | double / array | 0.03 / [] | flat 利率 / 期限结构 [{days, rate}] |
| maxTPS | int | 50 | 每秒最大交易数 |
| minComputeInterval | double | 0.02 | 最小计算间隔（秒） |
| marketFreshTimeoutMs | int | 3000 | 行情新鲜度门限（超时停止报价刷新） |
| use_parallel | bool | false | OpenMP 并行定价 |
| sessionSchedule | object | - | 盘中停止调度（stopLeadSeconds/resumeLagSeconds） |
| pricer | object | - | 定价器参数（含 volCurve.fitter，见下） |
| orderManager | object | - | OQM 参数 + riskFilters |
| margin / pnl / delta_hedge | object | - | 保证金 / FIFO PnL / delta 对冲 |
| expiries | **array** | - | 到期月配置（**必须数组格式**，见下） |
| riskLimits | object | - | RiskLimitsEx 参数（含 maxShortCall/PutPerSymbol） |
| signals / scanners | object / array | - | 信号与扫描器 |
| optionValueWriter | object | - | 理论值记录器 |

### 期权品种自动推导

| 交易所 | 期货品种 | 期权品种 | 规则 |
|--------|---------|---------|------|
| SHFE/INE/DCE | ag, cu, m … | ag_o, cu_o, m_o | 期货品种 + "_o" |
| CZCE | SR … | SRC, SRP | 期货品种 + "C"/"P"（双品种发现） |
| CFFEX | IF/IC/IH | IO/MO/HO | 固定映射 |

> ⚠️ **SHFE 商品期权必须挂独立的 `{prod}_o` 品种**（commodity `category=2`）：框架 `CodeHelper::extractStdChnFutOptCode` 将标准代码 `SHFE.cu2607.C.104000` 的 product 解析为 `cu_o`，合约 metadata 与发现均按 `cu_o` 查找。

### expiries（数组格式，键为 YYYYMM）

> ⚠️ **格式强校验**：`expiry` 必须是 6 位 `YYYYMM` 且 ∈ [202001, 209912]（loader 启动报错）。
> 写成 `2607` 会被拒绝——网格所有容器都以 6 位到期键索引。

**标的（underlyingCode）优先级规则**（P2/G3 修复后）：

| 场景 | 定价标的 | 说明 |
|------|---------|------|
| `expiries[].underlyingCode` 非空 | 该代码（**专属**） | 全局 `underlyingCode` 的 tick **不会**再路由进该 expiry |
| `expiries[].underlyingCode` 为空 | 全局 `underlyingCode` | 标准单标的模式 |
| 对冲标的 | `expiries[].hedgeCode`，空则同定价标的 | 由 `OptionTradingGrid::setHedgeOverride` 在 ExpiryData 创建时生效 |

> **单品种边界**：一个策略实例 = 一个期权品种（`_optionProduct` 单值，所有容器以纯 expiry 键索引）。
> 唯一例外是 CZCE 的 C/P 双品种（SRC+SRP）——它们共享同一 strike 空间，设计上就应合并进同一网格。
> 两个不同品种（如 CFFEX IO+MO）**不能**共用一个策略实例，否则同到期月会串数据
> （同一 ExpiryData/strike 桶互覆 call/put 槽）。多品种请部署多个策略实例（WtRunner 支持）。

```json
"expiries": [
    {
        "expiry": 202607, "enable": true,
        "underlyingCode": "SHFE.cu.2607", "hedgeCode": "SHFE.cu.2607",
        "secondaryHedgeCodes": [],
        "delta_min": 0.05, "delta_max": 0.95,
        "sprd_fwd": 0.01, "sprd_atmvol": 0.1, "sprd_corr": 0.0,
        "max_pos_fut": 10, "max_pos_stk": 50, "max_pos_opt": 50,
        "max_qsize": 5, "enable_auto_close": false, "close_pos_thresh": 0
    }
]
```

### 持仓限制的执行顺序（4 套，P2 文档化）

从外到内依次生效，任一层拒绝即不下单：

| 顺序 | 机制 | 配置位置 | 粒度 | 行为 |
|------|------|---------|------|------|
| 1 | RiskLimitsEx 预交易 | `riskLimits.maxPositionPerOption/maxTotalPosition`、Greeks/DailyLoss | 合约/组合 | REJECT（含保证金估算） |
| 2 | RiskFilterChain | `orderManager.riskFilters.{option,future}.max_position` | 合约 | REJECT / ALLOW / MODIFY |
| 3 | ExpiryRiskConfig 报价尺寸 | `expiries[].max_pos_{fut,stk,opt}` + `max_qsize` | 报价尺寸计算 | 限制报价手数（`__computeQuoteSize`） |
| 4 | OQM 内部 | `orderManager.option.max_position`（默认 50）、check_potential_position | 合约 | 开平仓路由约束 |

### volCurve.fitter

| 字段 | 默认 | 说明 |
|------|------|------|
| mode | "async" | 拟合执行模式："async"=SingleFlightTaskRunner 后台拟合（默认，不阻塞 worker）；"sync"=PeriodicCurveFitter 同步链（回测确定性/拟合调试）。两链互斥 |
| start_time / end_time | 0 / 86400 | 拟合窗口（秒，支持夜盘） |
| period | 60 | 拟合周期（秒，async 模式同样生效，提交节流） |
| decay_window | 300 | 跨快照 wing 点 carry 老化界（秒，0=关闭） |
| threshold | 0 | 中间价下限（低于此价的合约不进拟合） |
| fit_all_expiries | false | 是否拟合全部到期月 |
| good_points_thresh | [{days:0, thresh:3}] | 有效点数阈值（按剩余天数插值，下限 4 点，且须 up/down 双象限齐备） |

### volCurve 曲线选择（P1-b）

| 字段 | 默认 | 说明 |
|------|------|------|
| type | "gvv" | 主曲线类型："gvv" \| "linear"（K−F 线性 σ，流动性差远月的稳健降级）\| "constant"（钉住 ATM，新挂 expiry 冷启动） |
| curve2_type | "gvv" | 第二曲线（volcurve_weight 混合用）："gvv" \| "power" \| "linear" \| "constant" |
| fallback | true | 主曲线拟合失败时 fallback 链 power→linear→constant(1.0)，保证定价不中断（旧曲线仍按版本号优先） |

### orderManager.riskFilters.{option,future}

| 字段 | 默认 | 说明 |
|------|------|------|
| enabled | false | 过滤器链开关 |
| max_order_size / _reject | 100 / false | 单笔上限；超限 reject 或截断 |
| min_sell_price | 0.0001 | 最低卖价保护 |
| max_position / _mode | 0 / 0(REJECT) | 持仓限制（0=关）；1=ALLOW 2=MODIFY |
| max_cancel_soft / hard | 100 / 200 | 撤单软告警 / 硬阻止 |
| max_new_orders_hard_flat / reject | 100 / 200 | 新单数硬平仓 / 拒绝阈值 |

执行顺序：MaxOrderSize → MinSellPrice → MaxPosition → MaxCancel → MaxNewOrders，REJECT 短路。

### OQM 参数（orderManager.option / orderManager.future）

| 字段 | 默认 | 说明 |
|------|------|------|
| enable_quote_api | true | 双边报价 API（false=拆 buy+sell） |
| avoid_trade | true | desired=current 时跳过 |
| leave_outer_orders | true | 保留外层订单 |
| quote_style | - | 报价风格 |
| enable_offset_guard | - | 偏移守卫 |
| wait_for_cancels | - | 撤单未确认不发新单 |
| cancel_ack_timeout_ms / cancel_max_retries | 3000 / - | 撤单 ack 看门狗：超时重发，耗尽 cancelAll 并冻结 |
| time_in_force_ms | - | 订单存活时间 |
| max_side_orders | - | 每合约每方向最大订单数 |
| stp_enabled | true | 自成交保护 |

### 热参数（ShareManager 运行时更新）

| 参数 | 类型 | 说明 |
|------|------|------|
| wgt_vegaflow / wgt_frontfut_skew / wgt_deltaflow / wgt_atmsig / wgt_rollema | double | alpha 信号权重 |
| sticky_base / improve_retreat_ratio | double | 报价粘性 |
| max_tps / command / qmode_override | int32 | TPS / 0=正常 1=停 2=panic 3=恢复 / 报价模式 |
| manual_order | string | "B,code,price,qty" / "S,…" / "C,code" |

### 三种业务场景配置示例

**商品期权（SHFE，1:1）**：
```json
{ "exchange": "SHFE", "futuresProduct": "cu",
  "underlyingCode": "SHFE.cu.2607", "underlyingType": "future",
  "expiries": [{"expiry": 202607, "enable": true}] }
```

**CZCE 多系列**：
```json
{ "exchange": "CZCE", "futuresProduct": "SR", "underlyingCode": "",
  "expiries": [
      {"expiry": 202609, "underlyingCode": "CZCE.SR309", "hedgeCode": "CZCE.SR309"},
      {"expiry": 202701, "underlyingCode": "CZCE.SR701", "hedgeCode": "CZCE.SR701"}] }
```

**股指期权（指数定价 + 期货分月对冲）**：
```json
{ "exchange": "CFFEX", "futuresProduct": "IF",
  "underlyingCode": "CFFEX.000300", "underlyingType": "index",
  "expiries": [
      {"expiry": 202607, "hedgeCode": "CFFEX.IF2607"},
      {"expiry": 202608, "hedgeCode": "CFFEX.IF2608"}] }
```

### delta 对冲（delta_hedge）

- `enable / threshold_delta / target_delta / max_order_size / cooldown_sec`
- `delta_per_lot`：一手对冲合约的 delta（不配置则从合约乘数解析，解析失败禁用对冲并告警）
- 在途抑制：上一批对冲单未终态前不发新批

### fillAttribution（成交质量归因，3.6）

默认关闭。关闭时热路径仅保留异常检测必需的成交记录（一条 push + 时间戳）；
开启时额外在**发单时**拷贝 `values(0)` 上下文快照、在**成交时**取 per-expiry 标的价
（均为在手边数据直拷，无锁无查找链；成本可忽略但非字面"零"）：

```json
"fillAttribution": { "enable": true, "outputDir": "outputs_bt/opt_mm_cu",
                     "keepMaxRecords": 20000, "graceSeconds": 300 }
```

- 每个成交订单导出一行到 `<outputDir>/fill_attribution.csv`（append-only
  **增量行**：每行覆盖该订单自上次导出以来的成交；迟到成交因此带原 session 归属，
  不会丢也不会重复）：
  `date`（导出行所属 session）/ `sessionDate`（订单所属 session）/ `code`/`side`/
  发单价/量（`issueQty`，拆分单此列留空并置 `splitFlag` 语义——见 registry）/
  `filledQty`（live 成交合计）/ `fillQty`（本行成交量，含迟到/拒单部分）/
  `theo0/mid0/iv0/delta0/spot0/fwd0`（发单时刻快照，spot 为 per-expiry 标的价）/
  量加权成交价 `vwFillPx`（按本行成交量加权；spot 缺失的成交不参与 `spotDriftBps` 加权）/
  `wallLagMs`（steady_clock 处理耗时）与 `simLagMs`（**模拟时钟**报价寿命，回放/live 同语义）/
  `edgeBps`（卖：(成-理)/理，买取反——做市 edge）/ `captureBps`（相对 mid 的捕获/让渡）/
  `spotDriftBps`（现货漂移，用于分离"标动"与"真 edge 衰减"）/
  `fillClass`（normal/late/mixed/rejected）/ `rejectFlag`
- 无法归类的交易所回报（unknown localid、拒单后成交）不进主 CSV，追加到
  `<outputDir>/fill_anomaly.csv`（localid/code/count/首末时间），不再只留在日志里
- 聚合日志：按合约与全体的 edgeBps / |captureBps| / lagMs 均值（rejected 行不参与聚合）
- 数据真源为 `IssuedOrderRegistry`（3.3 合并后的单一 per-order 登记表）；
  `keepMaxRecords` 为**容量边界**淘汰，`graceSeconds` 为**年龄下界**——done 未满 grace
  的记录永不淘汰（跨 session 迟到成交归因窗口）；`keepMaxRecords=0` 表示仅按时间淘汰。
  登记表在 session begin **不再整表清空**，由容量+年龄双约束兜底内存
- 表头只在文件不存在或为空时写出（修复了 app 模式先建文件再判断导致表头恒缺失的问题）
- 回测末日：进程退出路径（worker 停止后）有一次最终 flush，覆盖最后一个 session end
  之后到达的成交；聚合日志若因 logger 关闭丢失，以 CSV 为准

回放复现：`YAML=configbt_option_replay_attr.yaml bash replay/run_option_replay.sh`。

---

## 性能模型（计算链路）

单个 tick **不触发全量重算**，链路为五层防护：

1. **攒批**：worker 每批统一处理一次（同批 N tick 合并）
2. **来源门控**：仅标的 tick 置重定价标志；期权自身 tick 只更新快照（`HftOptionStrategy` tick 路由）
3. **防抖**：`minComputeInterval`（默认 0.02s = 50Hz 上限）+ 同秒去重
4. **FAST/SLOW 分档**（`CompositeOptionPricer`）：
   - FAST（默认）：不重算 BS/IV——理论价用 **delta+gamma 泰勒修正**平移缓存值（每期权几次乘法），
     仅补算未定价者；全量 BS 仅在 forward 跳变超 `fast_full_reprice_forward_step`、临到期、SLOW 周期到时
   - SLOW（`slow_compute_interval`）：全量重定价 + 每期权 IV 反推（Brent）
5. **重活外移**：曲线 WLS 拟合在独立线程（默认 60s 周期），worker 只交快照；
   scanner/PnL 按 `_dirtyOptions` 门控

**容量规划**：SLOW 档全量 IV 反推的成本为 `期权数 × (1/slow_compute_interval)` 次 Brent/秒。
56 期权 × 10Hz（interval=0.1）≈ 560 次/秒，实测 batch p50≈51-64µs 可控；
链式品种（数百期权）上线时应调大 `slow_compute_interval` 至 0.5-1s 或评估 IV 反推增量化
（BS 全量 + IV 仅对 dirty 期权）。

---

## 回测与 Replay 验证

`replay/` 提供真实 tick 数据的端到端回测工具链（2026-11 验收通过）：

| 文件 | 职责 |
|------|------|
| `run_option_replay.sh` | 一键回放：取数（缺数据时）→ DSB 校验 → 注入 basefiles → WtBtRunner → **运行时 error 分类门禁** → 延迟指标提取 |
| `configbt_option_replay.yaml` | 默认回放配置（归因关闭） |
| `configbt_option_replay_attr.yaml` | 归因开启变体（`YAML=configbt_option_replay_attr.yaml` 选用） |
| `fetch_option_ticks.sh` | 服务器侧取数管道（KS/MinIO tar → 服务器环回拉取+转换 → 分块拉回），含外网数据面故障绕行 |
| `gen_basefiles.py` | 生成 replay basefiles：SHFE 期权挂 `{prod}_o` 独立品种（category=2）+ 标的期货 + 期权链合约条目 |
| `replay_common/` | 生成的 basefiles（commodities/contracts/fess/sessions/holidays/hots） |

**error 门禁**（2026-11-09 复核整改后加入）：`exit=0` 只证明进程没崩，不代表无运行时错误。
脚本在回放后统计 ` - error]/ - fatal]`（注意该格式无尾部空格），分两类：

- **HARD**（归因/异常完整性）：`order-not-found fill`、`OVERFILL`、`fill for REJECTED order`、
  `ORDER ANOMALY ESCALATION` —— 出现即 exit 1（可用 `STRICT=0` 覆盖）；
- **SOFT**（风险路径事件）：FillPriceChecker PANIC / Auto-panic / PositionGuard / market stale /
  Greeks 限额 / `session_end 非零持仓` —— 打印分类计数供人工 triage，不自动失败。

**回放要点**：

- replayer 只回放已订阅合约：commodities 中 SHFE 期权品种须 `category=2`（`CC_FutOption`），contracts 须含期权链全部代码
- tick DSB 为 raw V2（0x03）时文件头必须是 **12 字节 BlockHeader**（`BLK_FLAG(8含NUL)+type(u16)+version(u16)`），C++ `proc_block_data` 按 12 字节去头
- 回测双边报价依赖 `HftMocker::stra_quote`（buy+sell 模拟，第二腿失败回撤第一腿）
- 建议 `use_newpx: false`（按对手价撮合；KS 期权 last 价可能陈旧，用 last 撮合会产生虚假滑点）
- 取数方法已固化为 opencode skill：`ks-data-fetch`（数据布局、stat 探测、故障绕行、服务器侧转换）

**验收基线（SHFE cu2607 期权链 56 期权 + 标的 × 20260602-0605，共 318.5 万 tick，2026-11-09 整改后复测）**：

- 回放 exit=0，**error 门禁 HARD=0**（unknown fill / overfill / reject-fill 全为 0，
  20260604→05 session 边界的迟到成交全部正确归类并进入归因，修复前为 34 条 unknown fill）
- SOFT 类为风险路径预期噪声：FillPriceChecker PANIC（回放按对手价撮合，期权偏离 >0.5% 常见，
  触发 panic latch 与 auto-panic）、PositionGuard discrepancy（回放持仓快照口径）、
  末日非零持仓（换日不平仓设计）；以上见日志 triage，不视为代码缺陷
- 归因回放（attr yaml）：`fill_attribution.csv` 表头恰好一行、按 session 分组无重复行、
  31 行成交订单增量行；`fill_anomaly.csv` 无记录（0 unknown）
- IV 质量：拟合 IV 11-14%，与市场独立 BS 反推（12-15%）吻合；GVV solve failed 0 次
- 定价链路：平价合成远期 + Black76 + GVV 全链路符合设计（详见上文「IV 定价链路」）
- 延迟（Debug 构建，worker 批处理直方图）：batch p50≈56-63µs / 尾部受批内 tick 数影响
  （同量级复测见 `LatencyHistogram` 批日志；早期 11µs 口径为单 tick 计算路径，非批处理）

合成延迟基线：`tests/test_replay_benchmark.cpp`。
验收以**当前机器复跑 + 门禁输出**为准；上表数字随回放配置/机器波动，不作为绝对基线。

---

## 构建与测试

### 构建

```bash
cd src/build_all && cmake .. && make -j$(nproc) WtOptionCore     # 共享库
make -j$(nproc) WtBtRunner                                        # 回测 runner
```

发行构建使用 `-march=x86-64-v3`（避免 `-march=native` 换机 SIGILL），`-O3 -flto`。

### 部署

```
hft/libWtOptionCore.so        ← WtRunner/WtBtRunner 的 strategies.hft[] 加载
```

插件边界：公共头文件只依赖 `Includes/*`；`libWtOptionCore.so` 无 `Wt*` 未定义符号、不 link WtCore。

### 单元测试（CTest 7/7）

```bash
ctest --test-dir src/build_all/WtOptionCore --output-on-failure
```

| 测试 | 覆盖 |
|------|------|
| test_ctg_ranking | CTG 排序 + 风控 |
| test_enhancements | 过滤器/持仓/偏离/风控/OQM |
| test_scanners | 工厂/生命周期/9 类信号/参数/GARCH/协调器 |
| test_async_event_processor | 双队列/背压/去重/异常兜底 |
| test_hedge_coordinator | delta 对冲决策/冷却/在途 |
| test_stress | StressAnalyzer 情景压力 |
| test_command_handlers | STOP/PANIC/QMODE/ORDER/CANCEL |
| test_replay_benchmark | 合成延迟基线（手工运行，未注册 CTest） |

### 端到端回测

```bash
bash src/WtOptionCore/replay/run_option_replay.sh [YYYYMMDD,...]
```

---

## 文件清单

```
src/WtOptionCore/
├── HftOptionStrategy.h/.cpp            # 策略入口（Shell：装配/生命周期/回调路由）
├── HftOptionStraFact.cpp               # DLL 工厂
├── OptionAsyncEventProcessor.h/.cpp    # 异步事件处理（双队列/优先级/去重/背压）
├── SingleFlightTaskRunner.h            # 拟合单飞执行
├── LatencyHistogram.h                  # tick→quote 延迟直方图
├── OptionTimeService.h                 # 连续时钟/FAST-SLOW 调度
├── TradingState.h                      # 交易状态机
│
├── OptionGrid.h/.cpp                   # 3级网格/合约发现/平价合成ATM forward
├── OptionData.h/.cpp                   # 单合约数据（双缓冲计算值）
├── ExpiryData.h/.cpp                   # 到期月（日历/maturity/discount）
├── StrikeData.h/.cpp                   # 行权价层
├── OptionTradingGrid.h/.cpp            # OTD/UTD 容器
├── OptionTradingData.h/.cpp            # 期权交易域状态
├── UnderlyingTradingData.h/.cpp        # 标的交易域状态
│
├── BlackCalc.h/.cpp                    # BS/Black76 计算
├── BlackImpliedCalculator.h/.cpp       # IV Brent 求解
├── OptionPricer2.h/.cpp                # 定价内核（forward装配/理论价/IV反推）
├── CompositeOptionPricer.h/.cpp        # 做市定价器（alpha+风控调整）
├── GammaScalpOptionPricer.h/.cpp       # gamma scalping 策略 pricer
├── GvvVolCurve.h/.cpp                  # GVV 曲面（WLS3+Brent α）
├── LinearVolCurve / ConstantVolCurve / PowerVolCurve   # 备用曲线
├── PeriodicCurveFitter.h/.cpp          # 拟合调度
├── AsyncVolFitter.h                    # 异步拟合快照/发布
├── IVolCurve.h                         # 曲线接口
│
├── ControllableTradingGrid.h/.cpp      # 执行调度（排序/TPS/panic/drop重试）
├── OptionQuoteManager.h/.cpp           # 订单生命周期（diffing/节流/STP/看门狗）
├── OrderGateway.h                      # 下单统一出口
├── check_markets.h/.cpp                # desired vs current 比较
├── BaseOrder / OptionOrder             # 订单状态机/腿PnL
├── OptionOrderInfo / FutureOrderInfo   # 订单快照
├── ComboOrders.h/.cpp                  # SpreadCombo/SynCombo 多腿原子执行
│
├── OptionRisk.h/.cpp                   # 组合Greeks聚合
├── OptionRiskData.h/.cpp               # 单合约仓位Greeks
├── OptionExpiryGreeks.h/.cpp           # 到期月Greeks
├── RiskFilterChain.h/.cpp              # 5过滤器链
├── RiskLimitsEx.h/.cpp                 # 预交易风控/Greeks限额/涨跌停护栏
├── PositionGuard.h/.cpp                # 持仓不一致护栏
├── PositionOffsetMgr.h/.cpp            # 可平量跟踪
├── FillPriceChecker.h/.cpp             # 成交价偏离监控
├── IssuedOrderRegistry.h               # 单一 per-order 登记表（异常检测+归因数据源）
├── StressAnalyzer.h/.cpp               # 情景压力
│
├── OptionScanCoordinator.h/.cpp        # scanner 生命周期/派发
├── OptionHedgeCoordinator.h/.cpp       # delta 对冲决策
├── OptionExpiryLifecycle.h/.cpp        # 到期刷新/purge/session清场
├── OptionCommandHandlers.h/.cpp        # 热指令
├── CommandBus.h                        # 指令总线
├── ConfigValidator.h                   # 启动配置校验
│
├── Scanners/                           # 9 类扫描器
├── Signals/                            # Alpha + Risk 信号 + 工厂
├── config/                             # option_strategy_full/advanced.json
├── replay/                             # 端到端回测工具链（取数/basefiles/配置）
├── tests/                              # 8 个 GoogleTest 测试
└── analysis/                           # 诊断报告与分阶段修复记录
```

---

## 迁移历史

从 quantbox/optiontrader（longbeach 框架）迁移到 WonderTrader HFT 框架：

| 迁移项 | 原始 | 迁移后 |
|--------|------|--------|
| 框架 | longbeach + Lua | WonderTrader HFT + JSON |
| 命名空间 | longbeach::optiontrader | wt_option |
| 指针 | boost::shared_ptr | std::shared_ptr |
| 事件 | Subscription + DECL_EVENT | std::function + enqueue |
| 仓位 | IPositionListener push | setPosition pull / enqueue |
| 配置 | Lua autogen | WTSVariant JSON |
| 数学 | QuantLib + GSL | BlackCalc + WLS3 |
| 脚本 | luabind | 移除 |
| UFT | 双框架 | 仅保留 HFT |
| 诊断 | std::cout | WTSLogger（结构化日志） |

诊断与修复历程见 `analysis/`（diagnostic-report/fix-plan 2026-08-24，phase0-11 2026-10-30，code-review-and-optimization-plan 2026-10-30）。
