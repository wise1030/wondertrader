# WtOptionCore — 期权做市策略引擎

## 目录
1. [项目概述](#1-项目概述)
2. [架构总览](#2-架构总览)
3. [核心模块详解](#3-核心模块详解)
4. [数据模型](#4-数据模型)
5. [业务流程](#5-业务流程)
6. [风控体系](#6-风控体系)
7. [Scanner模块](#7-scanner模块)
8. [配置说明](#8-配置说明)
9. [线程安全](#9-线程安全)
10. [扩展指南](#10-扩展指南)
11. [已知限制](#11-已知限制)

---

## 1. 项目概述

WtOptionCore 是 WonderTrader 框架下的 **期权做市策略引擎**，移植自 longbeach/quantbox 系统。

**核心能力**：
- 实时波动率曲面拟合（GVV/Linear/Constant模型）
- FAST/SLOW 双路径定价（~50μs / ~5ms）
- **原生双边报价能力 (Native Quotes) 与做市策略深度集成**
- 多维风控（组合级/到期日级/单笔级/防Ping击穿）
- 11种 Scanner 策略信号模块
- 异步行情处理（lock-free SPSC Queue）
- 线程池订单执行

**技术栈**：
- C++14（MSVC/GCC）
- Boost（threadpool, lockfree）
- WonderTrader Core（IBaseDataMgr, WTSSessionInfo, TraderAdapter 支持双边报价API）

---

## 2. 架构总览

### 2.1 三层架构

```
┌─────────────────────────────────────────────────┐
│           WonderTrader HFT Engine               │
│        (行情/交易回调, 定时器驱动)                  │
└────────────────────┬────────────────────────────┘
                     │
┌────────────────────▼────────────────────────────┐
│  WtOptEngine (引擎层)                            │
│  • 线程池管理 (boost::threadpool::pool, 4线程)    │
│  • 行情异步分发 (SPSC Queue → Context)           │
│  • 交易通道管理 (TraderAdapter)                   │
│  • 定时器驱动 (WtOptRtTicker)                     │
│  • 多Context实例管理                              │
└────────────────────┬────────────────────────────┘
                     │
┌────────────────────▼────────────────────────────┐
│  WtOptContext (上下文层)                          │
│  • 异步 Worker 线程 + SPSC Queue (lock-free)     │
│  • 组件生命周期: OptionGrid / OptionRisk /        │
│    GridOrderManager                              │
│  • 订单/撤单执行器配置                             │
│  • WonderTrader 接口适配 (IOptStraCtx)           │
└────────────────────┬────────────────────────────┘
                     │
┌────────────────────▼────────────────────────────┐
│  WtOptionStrategy (策略层)                       │
│  • 状态机: Idle → Running → Paused → Panicked   │
│  • 拥有: CurveFitter / CompositeOptionPricer     │
│  • 管理: 11种Scanner模块                          │
│  • 风控: Delta/Gamma/Vega限额 + 日亏限额         │
│  • 行为控制: Trade Shock Tracker (防止频繁被击穿) │
│  • PnL: 实时已实现+未实现盈亏追踪                  │
└─────────────────────────────────────────────────┘
```

### 2.2 组件所有权关系

| 组件 | 创建者 | 所有者 | 使用者 |
|------|--------|--------|--------|
| OptionGrid | WtOptContext | WtOptContext | Strategy, Scanners, Pricer |
| OptionRisk | WtOptContext | WtOptContext | Strategy, CompositeOptionPricer |
| GridOrderManager | WtOptContext | WtOptContext | Strategy, Scanners |
| CurveFitter | WtOptionStrategy | WtOptionStrategy | 拟合定时触发 |
| CompositeOptionPricer | WtOptionStrategy | WtOptionStrategy | Grid |
| StandardOptionPricer | WtOptionStrategy | CompositeOptionPricer | 理论定价 |
| Scanners[] | WtOptionStrategy | WtOptionStrategy | 信号生成 |

### 2.3 文件结构

```
WtOptionCore/
├── 引擎层
│   ├── WtOptEngine.h/cpp          # 引擎主类
│   ├── WtOptRtTicker.h/cpp        # 定时器驱动
│   └── WtOptContext.h/cpp         # 策略运行上下文
│
├── 策略层
│   ├── WtOptionStrategy.h/cpp     # 做市策略主类
│   ├── OptStrategy.h              # 策略基类接口
│   └── IOptStraCtx.h              # 上下文接口
│
├── 数据模型
│   ├── OptionTypes.h              # 枚举/常量定义
│   ├── OptionData.h/cpp           # 期权数据容器
│   ├── OptionGrid.h/cpp           # 期权链网格管理
│   ├── OptionGreeks.h             # Greeks容器
│   ├── ExpiryData.h/cpp           # 到期日数据
│   ├── UnderlyingTradingData.h/cpp # 标的交易数据
│   └── OptionOrder.h/cpp          # 订单数据结构
│
├── 定价引擎
│   ├── IOptionPricer.h            # 统一定价接口
│   ├── BlackScholes.h/cpp         # BS/Black76核心算法
│   ├── StandardOptionPricer.h/cpp # 标准理论定价
│   └── CompositeOptionPricer.h/cpp # 复合做市定价 (FAST/SLOW)
│
├── 波动率
│   ├── VolCurve.h/cpp             # 波动率曲线接口+实现
│   ├── CurveFitter.h/cpp          # 曲面拟合器
│   └── IAlphaSignal.h             # Alpha信号接口
│
├── 风控
│   ├── OptionRisk.h/cpp           # 组合风险管理
│   └── QuoteStatistics.h          # 报价统计
│
├── 订单管理
│   └── OrderManager.h/cpp         # 多层订单管理
│
├── Scanner模块
│   ├── IScanModule.h              # Scanner基类
│   ├── ScannerFactory.h/cpp       # 工厂注册
│   ├── MMScanner.h/cpp            # 做市Scanner
│   ├── SpreadScanner.h/cpp        # 价差Scanner
│   ├── ButterflyScanner.h/cpp     # 蝶式Scanner
│   ├── StrikeSpreadScanner.h/cpp  # 行权价价差
│   ├── GarchScanner.h/cpp         # GARCH信号
│   ├── VolSpreadScanner.h/cpp     # 波动率期限结构
│   ├── SyntheticFutureScanner.h/cpp # 合成期货
│   ├── LowBidsScanner.h/cpp       # 低价期权
│   ├── OpenScanner.h/cpp          # OI/量比异动
│   └── SimplexScanner.h/cpp       # 线性规划
│
├── 配置
│   ├── option_config_demo.json    # 完整配置示例
│   ├── option_strategy.json       # 基础配置
│   └── option_strategy_advanced.json # 高级配置
│
└── tests/
    ├── test_blackscholes.cpp      # BS定价测试
    └── test_optiongrid.cpp        # Grid结构测试
```

---

## 3. 核心模块详解

### 3.1 OptionGrid — 期权数据网格

**职责**：管理完整的期权链数据，按到期日→行权价→Call/Put三级组织。

**关键功能**：
- `addOption(OptionInfo)` — 自动建立到期日、行权价、Call/Put关联
- `onTick(code, price, bid, ask, ...)` — 标的/期权行情分流更新
- `computeValues()` — 委托 IOptionPricer 计算理论价和Greeks
- `getAggregatedGreeks()` — 聚合全组合的Greeks

**交易日历集成**：
- 通过 `setBaseDataMgr()` + `setProductId()` + `setSessionInfo()` 获取交易日历
- `ExpiryData::updateTimeToExpiry()` 根据交易日历计算精确到期时间

### 3.2 CompositeOptionPricer — FAST/SLOW 双路径定价

**FAST路径**（每tick触发，~50μs）：
1. 使用缓存的波动率和Greeks
2. 直接根据标的价格变化更新理论价
3. 不求解IV，不重拟合曲面

**SLOW路径**（定期触发，默认100ms，~5ms）：
1. 求解每个期权的隐含波动率（Newton-Raphson）
2. 重新拟合波动率曲面
3. 重算ATM Forward（合成期货平价）
4. Greeks时间衰减调整
5. 完整的OurMarket报价计算

**报价生成** (`computeOurMarkets`)：
```
TheoPrice = StandardOptionPricer 理论价
± risk_adjustment (基于当前Delta/Vega仓位偏移)
± alpha_adjustment (基于价格/波动率信号)
= OurBid / OurAsk (考虑spreadVol, spreadFut, minSpread)
```

### 3.3 CurveFitter — 波动率曲面拟合

**拟合流程**：
1. 遍历每个到期日
2. `collectFitPoints()` — 收集 (strikeDiff, normalizedVol) 数据点
3. 过滤异常点 (`validatePoint()`)
4. 计算 ATMVol（线性插值法） 和 ATMForward（合成期货平价）
5. 调用 `GvvVolCurve.fit()` 执行GVV模型拟合

**GVV模型拟合**：
- 参数：spotvol, rho, volvol, alpha
- alpha 搜索：线性扫描 [alphaLow, alphaHigh]
- 固定 alpha 后加权线性回归求 (atmvol², skew, kurt)
- 最小化 chi² 确定最优 alpha
- 反推物理参数 spotvol, rho, volvol

**支持的曲线模型**：
| 模型 | 类名 | 用途 |
|------|------|------|
| 常数 | `ConstantVolCurve` | 测试/默认 |
| 线性 | `LinearVolCurve` | 简单skew |
| GVV | `GvvVolCurve` | 生产环境 |

### 3.4 OrderManager — 订单管理

**三层结构**：
```
GridOrderManager (全网格)
  ├── OptionOrderManager (每个期权)
  │     ├── MultiLevelQuote (多层报价 - 支持原生Quote API)
  │     ├── Active Orders (活跃订单列表)
  │     ├── TradingStats (交易统计)
  │     └── OrderExecutor / CancelExecutor / QuoteExecutor (原生接口回调)
  └── UnderlyingOrderManager (标的对冲)
        ├── 独立状态跟踪与撤单逻辑管理对冲委托
```

**报价更新逻辑** (`updateOrders`)：
1. 分析最新 `MultiLevelQuote` 与当前活跃订单的差异。
2. 调用底层 `QuoteExecutor` 提交原生双边报单(`stra_quote`)，或对单侧报单使用 `OrderExecutor`。
3. 撤销不在目标价位的订单。
4. 补充缺少的报价量。
5. 支持 cancelOnly 模式（Panic时），统一清理所有挂单记录并发送撤单要求。

**状态机**：`New → Sent → Acked → PartialFill → Filled | Cancelled | Rejected`

### 3.5 OptionRisk — 组合风险管理

**数据结构**：
- `OptionRiskData` — 单个期权的仓位+Greeks+PnL
- `ExpiryGreeks` — 到期日级聚合Greeks
- `HedgeData` — 对冲工具仓位
- 全组合 `m_positionGreeks` / `m_optionGreeks` / `m_underlierDelta`

**更新流程**：
1. 遍历所有非零仓位→更新每个 `OptionRiskData`
2. 聚合至到期日级 `ExpiryGreeks`
3. 包含 UnderlyingTradingData 的 delta
4. 包含 HedgeData 的 delta
5. 计算总组合 Delta

**线程安全**：所有公共读写方法使用 `std::mutex` 保护。

---

## 4. 数据模型

### 4.1 期权信息 (OptionInfo)

| 字段 | 类型 | 说明 |
|------|------|------|
| code | string | 合约代码 (e.g., "cu2502C50000") |
| underlying | string | 标的代码 |
| strike | double | 行权价 |
| expiry | uint32 | 到期日 (YYYYMMDD) |
| right | OptionRight | Call / Put |
| multiplier | double | 合约乘数 |

### 4.2 行情数据 (OptionMarket)

| 字段 | 说明 |
|------|------|
| bid/ask/last | 买一/卖一/最新价 |
| bidSize/askSize | 买一量/卖一量 |
| underlyingPrice | 标的最新价 |
| volume/openInterest | 成交量/持仓量 |

### 4.3 计算值 (OptionValues)

| 字段 | 说明 |
|------|------|
| theoPrice | 理论价格 |
| impliedVol | 隐含波动率 |
| Greeks | OptionGreeks (delta, gamma, vega, theta, rho, vanna, volga) |
| ourBid/ourAsk | 策略报价 |

---

## 5. 业务流程

### 5.1 初始化流程

```
WtOptEngine::init()
  └── 创建线程池(4), 初始化TraderAdapter
      └── for each strategy config:
          └── WtOptContext::init()
              ├── 创建 OptionGrid (设置日历, Session)
              ├── 创建 OptionRisk (注册Grid监听)
              ├── 创建 GridOrderManager (设置执行器回调)
              ├── 启动 Worker 线程 (SPSC Queue消费者)
              └── WtOptionStrategy::on_init()
                  ├── 获取 Grid/Risk/OrderManager 引用
                  ├── 创建 CurveFitter
                  ├── 创建 CompositeOptionPricer
                  │     ├── 设置 StandardOptionPricer
                  │     └── 设置 OptionRisk 引用
                  ├── 创建 Scanners (from config)
                  ├── 设置状态 → Running
                  └── 启动所有 Scanners
```

### 5.2 Tick处理流程

```
HFT Engine ──tick──▶ WtOptEngine.on_tick(code, tick)
                         │
                    ┌────▼────┐  lock-free
                    │  SPSC   │
                    │  Queue  │
                    └────┬────┘
                    Worker Thread
                         │
                    WtOptContext ──▶ OptionGrid.onTick()
                         │              └── 更新行情数据
                         │
                    WtOptionStrategy.onTick()
                         │
                    ┌────┴─────────────────────┐
                    │                          │
              标的更新?                    期权更新?
                    │                          │
           通知Scanners                 通知Scanners
           checkPanic()                      │
                    │                          │
                    └──────────┬───────────────┘
                               │
                    定期(5s) checkRiskLimits()
                              updatePnL()
```

### 5.3 定时器流程

```
WtOptRtTicker ──timer──▶ WtOptionStrategy.onTimer()
                              │
                    ┌─────────┼──────────────┐
                    │         │              │
            Scanner.onTick  CurveFitter   processOrders
            (生成信号)      .onTimer()    (执行报价)
                    │      (定期拟合)         │
                    ▼                         ▼
           onScannerHit              updateAllOrders
           → onOptionHit              (diff报价)
```

---

## 6. 风控体系

### 6.1 多层风控框架

| 层级 | 检查项 | 触发条件 | 动作 |
|------|--------|----------|------|
| **Panic** | 标的价格急变 | `|ΔS/S| > panicThreshold` | 撤全部订单, 状态→Panicked |
| **组合级** | Delta/Gamma/Vega | `|Greek| > maxGreek` | `onRiskLimitBreached()` → Pause |
| **日损级** | 日PnL | `dayPnL < -maxLossPerDay` | 暂停策略 |
| **到期日级** | deltaMin/Max, maxPosOpt | per-expiry配置 | 拒绝超限订单 |
| **单笔级** | maxOrderSize, maxPositionPerOption | 预检查 | 拒绝订单 |

### 6.2 风控参数 (RiskLimits)

```cpp
struct RiskLimits {
    double maxDelta = 1000;             // 最大Delta暴露
    double maxGamma = 100;              // 最大Gamma暴露
    double maxVega = 10000;             // 最大Vega暴露
    double maxPositionPerOption = 100;  // 单期权最大仓位
    double maxTotalPosition = 1000;     // 总仓位上限
    double maxLossPerDay = 100000;      // 日亏上限
    double panicThreshold = 0.05;       // Panic阈值 (5%)
};
```

### 6.3 PnL追踪

| 字段 | 计算方式 |
|------|----------|
| realizedPnL | 从 OrderManager 统计 |
| unrealizedPnL | 各仓位 netPnl 汇总 |
| optionPnL | realized + unrealized |
| hedgePnL | 对冲工具 delta * 标的价格 |
| dayPnL | totalPnL - dayStartPnL |

---

## 7. Scanner模块

### 7.1 Scanner接口

所有Scanner继承 `IScanModule`：
```cpp
class IScanModule {
    virtual void onStart() = 0;
    virtual void onStop() = 0;
    virtual void onTick(OptionGrid* grid) = 0;
    virtual void onOptionUpdate(OptionData* option) = 0;
    virtual void onUnderlyingUpdate(double price) = 0;
    virtual void onPanic() = 0;
    void setEnabled(bool);
    bool isEnabled() const;
};
```

### 7.2 Scanner清单

| Scanner | 信号来源 | 策略类型 | 典型参数 |
|---------|----------|----------|----------|
| **MMScanner** | TheoPrice vs Market | 做市 | spreadVol, minSpread |
| **SpreadScanner** | Call-Put价差异常 | 套利 | maxSpread, minProfit |
| **ButterflyScanner** | 蝶式组合价差 | 套利 | wingWidth, threshold |
| **StrikeSpreadScanner** | 行权价价差/Guts/合成对 | 套利 | strikeGap, minEdge |
| **SyntheticFutureScanner** | 合成期货 vs 实际 | 套利 | maxDeviation |
| **GarchScanner** | GARCH波动率预测 | 方向 | lookback, lambda |
| **VolSpreadScanner** | 波动率期限结构 | 波动率 | maxTermSpread |
| **LowBidsScanner** | 低价期权 | 收益增强 | maxPrice, minOI |
| **OpenScanner** | OI/量比异动 | 信息型 | oiThreshold, volumeRatio |
| **SimplexScanner** | 线性规划最优组合 | 优化型 | constraints[] |

### 7.3 Scanner配置

每个Scanner支持全局参数 + 到期日覆盖：
```json
{
    "name": "MMScanner",
    "enabled": true,
    "spreadVol": 0.005,
    "expiryOverrides": {
        "20250228": { "spreadVol": 0.003 },
        "20250328": { "spreadVol": 0.008 }
    }
}
```

---

## 8. 配置说明

### 8.1 策略配置结构

```json
{
    "strategy": {
        "name": "OptionMM",
        "underlyingCode": "FUT_SHFE_cu",
        "exchangeCode": "SHFE",
        "hedgeCode": "cu2502",
        "defaultVolatility": 0.2,
        "riskFreeRate": 0.03,
        "traceLevel": 1,
        "useCompositePricer": true,
        
        "riskLimits": {
            "maxDelta": 1000,
            "maxGamma": 100,
            "maxVega": 10000,
            "maxPositionPerOption": 100,
            "maxTotalPosition": 1000,
            "maxLossPerDay": 100000,
            "panicThreshold": 0.05
        },
        
        "curveFit": {
            "enable": true,
            "periodUs": 60000000,
            "model": "GVV",
            "alphaLow": 0.01,
            "alphaHigh": 5.0,
            "alphaSteps": 100
        }
    },
    
    "threading": {
        "orderPoolSize": 4,
        "riskIntervalMs": 5000,
        "pnlIntervalMs": 5000
    },
    
    "scanners": [
        {
            "name": "MMScanner",
            "enabled": true,
            "spreadVol": 0.005,
            "minSpread": 0.0005,
            "expiryOverrides": {}
        }
    ],
    
    "expiryConfigs": [
        {
            "expiry": 20250228,
            "enable": true,
            "maxPosOpt": 500,
            "deltaMin": 0.1,
            "deltaMax": 0.9,
            "spreadVol": 0.005,
            "quoteUnderlying": false
        }
    ]
}
```

### 8.2 关键参数说明

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `traceLevel` | 0 | 日志级别 (0=quiet, 1=normal, 2=verbose) |
| `slowComputePeriodMs` | 100 | SLOW定价路径间隔(ms) |
| `spreadVol` | 0.005 | 报价波动率spread (0.5%) |
| `spreadFut` | 0.0002 | 报价期货price spread |
| `minSpread` | 0.0005 | 最小报价宽度 |
| `panicThreshold` | 0.05 | Panic触发阈值 (5%) |
| `cancelRetryIntervalMs` | 400 | 撤单重试间隔 |

---

## 9. 线程安全

### 9.1 线程模型

| 线程 | 职责 | 访问的共享数据 |
|------|------|----------------|
| HFT回调线程 | tick/order事件 | SPSC Queue (producer) |
| Context Worker线程 | 处理tick, 驱动策略 | OptionGrid, Strategy |
| Engine线程池(4) | 订单执行 | TraderAdapter, OrderManager |
| Timer线程 | 定时事件 | Strategy (间接) |

### 9.2 同步机制

| 组件 | 同步方式 | 说明 |
|------|----------|------|
| SPSC Queue | lock-free | Engine→Context 行情传递 |
| OptionRisk | `std::mutex` | 全部公共方法加锁 |
| OrderManager | `std::mutex` | 各Manager独立锁 |
| GridOrderManager | `std::mutex` | 管理器注册/遍历 |
| OptionGrid | 无 | 单线程访问(Worker线程) |

---

## 10. 扩展指南

### 10.1 添加新的 Scanner

1. 在 `Scanners/` 目录创建 `MyScanner.h/cpp`
2. 继承 `IScanModule`，实现所有虚函数
3. 在 `ScannerFactory.cpp` 中注册：
   ```cpp
   REGISTER_SCANNER("MyScanner", MyScanner);
   ```
4. 在配置 JSON 中添加 scanner 配置
5. Scanner 通过 `onScannerHit(ScannerHitEvent)` 发出信号

### 10.2 添加新的波动率模型

1. 在 `VolCurve.h` 中继承 `IVolCurve`
2. 实现 `getVol(strikeDiff)` 和 `fit(points, weights)` 
3. 在 `CurveFitter` 中添加创建逻辑

### 10.3 自定义定价器

1. 继承 `IOptionPricer` 
2. 实现 `computeValues(OptionGrid*)` 等方法
3. 在 `WtOptionStrategy::on_init()` 中替换默认定价器

### 10.4 自定义策略逻辑

继承 `WtOptionStrategy` 并覆盖以下方法：
```cpp
virtual void onStrategyInit() {}        // 初始化自定义逻辑
virtual void onStrategyStart() {}       // 策略启动
virtual void onOptionHit(OptionData*, double signal, const std::string& reason);
virtual void onPanicTriggered() {}      // Panic事件处理
virtual void onRiskLimitBreached(const std::string& limitName, double value, double limit);
virtual void onDayStart(uint32_t date) {}
virtual void onDayEnd(uint32_t date) {}
```

---

## 11. 已知限制与最新优化建议

当前版本（支持WonderTrader底层原生Quote之后）的进一步优化建议：

### 11.1 性能与算法优化 (已于最新版本完成)
| 方向 | 说明 | 当前已实现状态 |
|------|------|----------|
| **GVV Alpha搜索** | 原先拟合时对Alpha进行线性扫描开销过高 | 已验证并确认在 `VolCurve.cpp` 中的 `BrentMinimize` (Brent's Method/Golden Section Search) 已经应用到 `GvvVolCurve::fit` 中实现快速收敛。 |
| **内存池化(Memory Pool)** | 原先订单和多档行情结构在堆内存频繁分配 | 已通过引入 **Boost.Pool** (`boost::object_pool`) 重载了 `OptionOrder` 和 `MultiLevelQuote` 的 `new`/`delete` 运算符，提升了内存管理的局部性并降低了内存碎片。 |
| **异步并发能力** | 原先天生预设 `OptionGrid` 针对单工作线程 | 已针对 `OptionGrid` 的读写逻辑增设了细粒度的 **Read-Write Locks (`std::shared_mutex`)**。保证了核心容器在多策略多线程并发运行时的结构安全性。 |
| **报价更新流量控制** | 原生的频繁撤盖原生Quote常遇交易所限流问题 | 已在 `OrderManager` 层 (`updateOrders` 方法) 追加了基于 `min_quote_update_interval_ms` 字段的时间戳差值测算的 "**微距过滤 (Micro-tick filtering)**" 以有效抑制重复冗余的 Native Quote。 |

### 11.2 工程架构完善
| 方向 | 说明 | 建议改进 |
|------|------|----------|
| **灾备与持久化 (Recovery)** | 系统进程重启后，未完成的报单状态和累积PnL/Risk数据丢失 | 接入 WonderTrader 的缓存或轻量持久化组件 (如 `wtkeeper` 或 mmap)，实现订单和持仓快照的 **Checkpoint/Recovery** 机制 |
| **订单ID贯通** | 内部产生的伪 `orderId` 未与交易所 SysID 完全绑定双向索引 | 通过适配 TraderAdapter 底层的 `ActionID/OrderRef` 联动，在 `WtOptContext` 中维持映射关系，以应对断线重连时的孤儿单 |
| **SPSC Queue 瓶颈监控** | Context 工作线程慢于前端行情流入时会发生队列丢弃且无自反馈 | 增加背压(back-pressure)计数，或接入时序追踪埋点反馈给策略执行应急降频措施 |

---

## 12. 构建与运行指南

### 12.1 构建

项目提供了 Windows 下的构建脚本 `build_wtoptioncore.bat`。

**前置依赖**：
- CMake 3.10+
- Visual Studio (MSVC) With C++17 support
- Boost (Thread, System)
- WonderTrader Core Libraries (WtCore, WTSTools, Share)

**构建步骤**：
1. 打开命令行
2. 进入 `src` 目录
3. 运行 `build_wtoptioncore.bat`

构建产物将生成在 `build/Release` 目录下。

### 12.2 运行

1. 确保 `WtOptionCore.lib` (或 .dll 如果编译为动态库) 放置在 WonderTrader 运行目录。
2. 准备配置文件 `option_strategy.json` (参考 `config/option_strategy.json`)。
3. 在 WonderTrader 加载器 (Loader) 配置中添加 WtOptionCore 策略工厂。

### 12.3 单元测试

构建脚本会自动构建单元测试：
- `build/tests/Release/test_blackscholes.exe`: 验证 BS/Black76 定价模型的准确性。
- `build/tests/Release/test_optiongrid.exe`: 验证 OptionGrid 数据结构的正确性。

建议在修改核心算法后运行这些测试。
