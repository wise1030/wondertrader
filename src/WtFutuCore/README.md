# WtFutuCore - 期货高频做市引擎

## 项目简介

WtFutuCore 是 WonderTrader 框架下的期货高频做市引擎模块，实现 **GLFT+Alpha** 做市框架与 **跨期价差套利** 系统。作为标准 UFT 策略运行，通过 `WtUftRunner` 启动，支持多合约、多档位双边报价，内置组合级库存管理、跨合约相关性分析、风险监控和异步套利执行。

**核心特性**：
- GLFT+Alpha 做市框架：动态价差 + Alpha预测驱动
- 跨期价差套利：5种策略逻辑（均值回归/趋势跟踪/配对交易/统计套利/做市增强）
- 异步执行架构：做市零延迟，套利独立线程
- 自成交防护：自动检测并避免自成交风险
- 高性能设计：Tick-to-Quote P99 < 100μs

---

## 架构设计

### 整体架构

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                            WtUftRunner                                       │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │                          WtUftEngine                                   │  │
│  │  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐                │  │
│  │  │ ParserAdapter │  │TraderAdapter │  │ WtUftDtMgr   │                │  │
│  │  └──────────────┘  └──────────────┘  └──────────────┘                │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
│                              ▼                                               │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │                      IUftStraCtx (UFT框架接口)                         │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
│                              ▼                                               │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │                    UftFutuMmStrategy (做市策略)                        │  │
│  │                                                                         │  │
│  │  ┌──────────────────── 主线程 (行情线程) ─────────────────────────┐   │  │
│  │  │                                                                 │   │  │
│  │  │  ┌───────────────── GLFT+Alpha 核心 ────────────────────────┐ │   │  │
│  │  │  │  SpreadOptimizer ◄── MicroAlphaEngine ◄── CorrelationMgr │ │   │  │
│  │  │  │  • GLFT价差计算      • OFI/Trade Imbalance  • Beta对冲   │ │   │  │
│  │  │  │  • 组合级库存偏移    • Lead-Lag信号        • 相关性追踪  │ │   │  │
│  │  │  └─────────────────────────────────────────────────────────┘ │   │  │
│  │  │                                                                 │   │  │
│  │  │  ┌───────────────── 风控与执行 ─────────────────────────────┐ │   │  │
│  │  │  │  FutuPortfolio    FutuQuoter      FutuRiskMonitor        │ │   │  │
│  │  │  │  • 持仓/Delta     • 多档位报价    • 频率限制             │ │   │  │
│  │  │  │  • 风险限制       • 订单跟踪      • 日内止损             │ │   │  │
│  │  │  └─────────────────────────────────────────────────────────┘ │   │  │
│  │  │                                                                 │   │  │
│  │  │  ┌───────────────── 辅助模块 ───────────────────────────────┐ │   │  │
│  │  │  │  ToxicFlowDetector  MarketStateDetector  AutoCancelPolicy│ │   │  │
│  │  │  │  SelfTradePrevention (自成交防护)                        │ │   │  │
│  │  │  └─────────────────────────────────────────────────────────┘ │   │  │
│  │  │                                                                 │   │  │
│  │  │  ┌───────────────── 异步接口 ───────────────────────────────┐ │   │  │
│  │  │  │  pushTick() ~50ns    processPendingOrders() ~5μs         │ │   │  │
│  │  │  └─────────────────────────────────────────────────────────┘ │   │  │
│  │  └─────────────────────────────────────────────────────────────────┘   │  │
│  │                              │                                          │  │
│  │                    LockFreeQueue (无锁队列)                             │  │
│  │                              │                                          │  │
│  │                              ▼                                          │  │
│  │  ┌──────────────────── 套利线程 (独立线程) ────────────────────────┐   │  │
│  │  │                                                                   │   │  │
│  │  │  ┌───────────────── SpreadArbitrageManager ────────────────────┐ │   │  │
│  │  │  │                                                               │ │   │  │
│  │  │  │  SpreadCalculator      SpreadRiskManager                     │ │   │  │
│  │  │  │  • 多类型价差计算      • 仓位限制/VaR                        │ │   │  │
│  │  │  │  • Z-Score/相关性      • 收敛风险评估                        │ │   │  │
│  │  │  │                                                               │ │   │  │
│  │  │  │  ┌─────────── 策略模块 ───────────────────────────────┐     │ │   │  │
│  │  │  │  │  MeanReversion  TrendFollowing  PairsTrading      │     │ │   │  │
│  │  │  │  │  StatisticalArb  MarketMakingEnhancer             │     │ │   │  │
│  │  │  │  └──────────────────────────────────────────────────┘     │ │   │  │
│  │  │  └───────────────────────────────────────────────────────────┘ │   │  │
│  │  │                                                                   │   │  │
│  │  │  条件变量唤醒 | 信号检查: 5ms / 每5tick | 延迟: <1ms             │   │  │
│  │  └───────────────────────────────────────────────────────────────────┘   │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 线程模型

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                           线程模型与数据流                                   │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ┌────────────────────────────┐         ┌────────────────────────────┐     │
│  │  主线程 (行情线程)          │         │  套利线程                   │     │
│  │  ────────────────────────  │         │  ────────────────────────  │     │
│  │                            │         │                            │     │
│  │  on_tick() {               │         │  arbThreadFunc() {         │     │
│  │    1. 数据更新      ~30μs  │         │    1. wait_for(cv)         │     │
│  │    2. 做市报价      ~20μs  │         │    2. popAll(ticks)        │     │
│  │    3. pushTick()    ~50ns ─┼────────►│    3. updateSpread()       │     │
│  │    4. 执行订单      ~5μs ◄─┼─────────│    4. generateSignals()    │     │
│  │  }                         │         │    5. pushOrders() ───────►│     │
│  │                            │         │  }                          │     │
│  │  延迟: ~55μs (做市无影响)   │         │  延迟: <1ms (条件变量唤醒) │     │
│  └────────────────────────────┘         └────────────────────────────┘     │
│                                                                             │
│  主线程优先级：做市报价 > 数据更新 > 套利订单执行                            │
│  套利线程：独立运行，条件变量唤醒，不影响做市延迟                            │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## 文件清单

### 核心模块

| 文件 | 模块 | 功能 |
|------|------|------|
| UftFutuMmStrategy.h/cpp | 主策略 | 做市策略入口、模块协调、异步执行 |
| FutuPortfolio.h/cpp | 组合管理 | 持仓跟踪、Delta聚合、风险限制、对冲决策 |
| FutuQuoter.h/cpp | 报价引擎 | 多档位报价、订单跟踪、一键撤单 |
| SpreadOptimizer.h/cpp | GLFT价差 | 动态价差计算、组合级库存偏移 |
| MicroAlphaEngine.h/cpp | Alpha预测 | OFI/Trade/LeadLag因子、复合Alpha |
| CorrelationManager.h/cpp | 相关性管理 | 跨合约相关性、Beta对冲比率 |
| FutuRiskMonitor.h/cpp | 风险监控 | 频率限制、日内止损 |
| AutoCancelPolicy.h/cpp | 自动撤单 | 超时撤单、价格偏离撤单 |
| MarketStateDetector.h/cpp | 市场状态 | 趋势/震荡识别、高波动检测 |
| ToxicFlowDetector.h/cpp | 毒性检测 | VPIN计算、熔断保护 |

### 监控调优模块

| 文件 | 模块 | 功能 |
|------|------|------|
| AdaptiveParamManager.h/cpp | 参数调优 | 动态参数调整、绩效反馈闭环 |
| PerformanceAnalyzer.h/cpp | 绩效分析 | PnL归因、胜率统计、不利选择成本 |
| PerformanceMonitor.h/cpp | 性能监控 | 延迟追踪、吞吐统计 |

### 跨期价差套利模块

| 文件 | 模块 | 功能 |
|------|------|------|
| SpreadArbitrageTypes.h | 类型定义 | 价差类型、策略类型、信号类型 |
| SpreadCalculator.h/cpp | 价差计算 | 多类型价差、Z-Score、相关性/Beta、半衰期 |
| MeanReversionStrategy.h/cpp | 均值回归 | Z-Score入场/离场、止损/超时 |
| TrendFollowingStrategy.h/cpp | 趋势跟踪 | MA交叉、趋势确认 |
| PairsTradingStrategy.h/cpp | 配对交易 | 协整检验、Beta对冲 |
| StatisticalArbStrategy.h/cpp | 统计套利 | 多因子信号、自适应权重 |
| MarketMakingEnhancer.h/cpp | 做市增强 | 报价调整、单边暂停 |
| SpreadRiskManager.h/cpp | 风险管理 | 仓位限制、VaR、相关性监控 |
| SpreadArbitrageManager.h/cpp | 核心管理器 | 策略协调、信号分发 |

### 异步执行与防护模块

| 文件 | 模块 | 功能 |
|------|------|------|
| LockFreeQueue.hpp | 无锁队列 | SPSC无锁队列、wait-free操作 |
| AsyncArbitrageExecutor.h/cpp | 异步执行器 | 独立线程、条件变量唤醒、订单分发 |
| SelfTradePrevention.h/cpp | 自成交防护 | 订单跟踪、风险检测、防护策略 |

---

## 模块详解

### 一、GLFT+Alpha 核心框架

#### 1. SpreadOptimizer - GLFT价差优化器

基于 GLFT (Gueant-Lehalle-Tapia Fernandez) 模型计算最优报价：

**核心公式**：
```
Fair Value:  ŝ = s + η × α      (中间价 + Alpha调整)
Bid Price:   P_bid = ŝ - δ/2 - φ×q
Ask Price:   P_ask = ŝ + δ/2 - φ×q

组合级库存偏移:
Skew_total = φ × (q_single + Σ ρ_i × q_related × hedge_ratio_i)
```

**主要接口**：
```cpp
// 单合约报价计算
GLFTResult computeOptimalQuote(double midPrice, double inventory, 
                               double alpha, double alphaSensitivity) const;

// 组合级报价计算
GLFTResult computeOptimalQuoteWithPortfolio(double midPrice, double singleInventory,
                                            const PortfolioContext& ctx,
                                            double alpha, double alphaSensitivity) const;

// 设置统一 skew 参数 (从策略顶层配置传入)
void setSkewEnhancement(double sensitivity, double aggressiveThreshold);
```

**统一参数说明**：
| 参数 | 说明 | 来源 |
|------|------|------|
| _skew_sensitivity | Skew 非线性增强系数 | 策略顶层配置 skewSensitivity |
| _aggressive_threshold | 激进 Skew 触发阈值 | 策略顶层配置 aggressiveSkewThreshold |

**配置参数**：
| 参数 | 说明 | 默认值 |
|------|------|--------|
| phi | 库存惩罚系数 φ | 0.01 |
| volSensitivity | 波动率敏感度 | 1.0 |
| depthSensitivity | 深度敏感度 | 0.5 |
| portfolioSkewWeight | 组合倾斜权重 | 0.5 |

#### 2. MicroAlphaEngine - Alpha预测引擎

预测未来100ms~1s的微观价格漂移：

**因子构成**：
| 因子 | 权重 | 计算方法 |
|------|------|----------|
| OFI (订单流失衡) | 40% | e_bid - e_ask |
| Trade Imbalance | 30% | 主动买量 - 主动卖量 |
| Lead-Lag | 30% | 跨品种领滞后 |

**输出**：复合Alpha α ∈ [-1, 1]

**配置参数**：
| 参数 | 说明 | 默认值 |
|------|------|--------|
| sensitivity | Alpha敏感度 η | 0.5 |
| ofiWeight | OFI权重 | 0.4 |
| tradeWeight | 成交失衡权重 | 0.3 |
| leadlagWeight | 跨品种权重 | 0.3 |
| emaFactor | EMA平滑因子 | 0.3 |

#### 3. CorrelationManager - 跨合约相关性管理

**功能**：
- 实时相关性计算（滚动窗口）
- 线性回归Beta计算
- 跨期/跨品种价差Z-Score

**主要接口**：
```cpp
CorrelationStats getCorrelation(const std::string& code1, const std::string& code2) const;
double getAggregateDelta(const std::map<std::string, double>& positions) const;
```

**配置参数**：
| 参数 | 说明 | 默认值 |
|------|------|--------|
| windowSize | 统计窗口 | 100 |
| minCorrelation | 最小相关系数 | 0.5 |
| spreadZThreshold | 价差Z阈值 | 2.0 |

---

### 二、风险与执行模块

#### 4. FutuPortfolio - 组合管理

**主要功能**：
- 多合约持仓跟踪（O(1)查找）
- 组合Delta/Exposure计算
- 单合约限制检查
- 对冲信号生成

#### 5. FutuQuoter - 报价引擎

**主要功能**：
- 多档位买卖报价管理
- 订单ID快速查找（O(1)哈希表）
- 一键撤单接口

**主要接口**：
```cpp
uint32_t refreshQuotes(IUftStraCtx* ctx, double mid, double skew, 
                       double spread_mult, bool allow_bid, bool allow_ask);
void cancelAll(IUftStraCtx* ctx);
```

**配置参数**：
| 参数 | 说明 | 默认值 |
|------|------|--------|
| numLevels | 档位数量 | 3 |
| baseSpread | 基础价差(tick) | 2.0 |
| baseQty | 基础量 | 1.0 |
| qtyDecay | 外层衰减 | 0.7 |

#### 6. FutuRiskMonitor - 风险监控

**功能**：
- 下单/撤单/成交频率限制
- 日内止损熔断
- 收盘前平仓管理
- 实时风险状态

**收盘前平仓管理**：
```
┌─────────────────────────────────────────────────────────────────┐
│                   收盘前平仓流程                                 │
├─────────────────────────────────────────────────────────────────┤
│  1. 收盘时间从基础数据自动获取 (WTSSessionInfo::getCloseTime)   │
│  2. 收盘前 N 分钟触发平仓 (可配置)                              │
│  3. 暂停报价 → 撤销所有挂单 → 对冲敞口                         │
│  4. 状态管理: triggered → completed                            │
│  5. 新交易日自动重置状态                                        │
└─────────────────────────────────────────────────────────────────┘
```

**主要接口**：
```cpp
// 检查是否触发收盘前平仓
bool checkCloseout(uint32_t currentTime, uint32_t closeTime);

// 状态查询
bool isCloseoutTriggered() const;
bool isCloseoutCompleted() const;

// 状态管理
void markCloseoutCompleted();
void resetCloseout();  // 新交易日重置
```

**配置参数**：
| 参数 | 说明 | 默认值 |
|------|------|--------|
| maxOrdersPerSec | 每秒最大下单数 | 50 |
| maxCancelsPerSec | 每秒最大撤单数 | 30 |
| maxTradesPerSec | 每秒最大成交数 | 20 |
| closeoutMinutesBefore | 收盘前多少分钟停止报价 | 5 |
| closeoutFlattenPosition | 收盘前是否平掉敞口 | true |

#### 7. AutoCancelPolicy - 自动撤单策略

**撤单条件**：
- 订单超时（maxAgeMs）
- 价格偏离过大
- 市场状态变化
- 库存限制触发

**配置参数**：
| 参数 | 说明 | 默认值 |
|------|------|--------|
| maxAgeMs | 最大存活时间 | 5000 |
| priceDeviation | 价格偏离阈值 | 3.0 |

#### 8. MarketStateDetector - 市场状态检测

**检测维度**：
- 波动率阈值
- 价格变动阈值
- 价差异常
- 成交量异常

**配置参数**：
| 参数 | 说明 | 默认值 |
|------|------|--------|
| volThreshold | 波动率阈值 | 0.003 |
| moveThreshold | 价格变动阈值 | 0.005 |
| lookbackTicks | 回看tick数 | 50 |

#### 9. ToxicFlowDetector - VPIN毒性检测

**功能**：
- VPIN计算
- 熔断保护
- 综合毒性检测（结合Alpha、订单簿、自身成交）

**配置参数**：
| 参数 | 说明 | 默认值 |
|------|------|--------|
| vpinThreshold | VPIN阈值 | 0.7 |
| window | 检测窗口 | 50 |
| cooloffMs | 冷却时间 | 5000 |
| alphaWeight | Alpha毒性权重 | 0.3 |
| bookWeight | 订单簿毒性权重 | 0.3 |
| selfTradeWeight | 自身成交毒性权重 | 0.4 |

**重要说明**：
- 自身成交记录由 `SelfTradeCalibrator` 统一管理
- 使用 `setSelfTradeCalibrator()` 连接校准器

---

### 五、综合信号组件（针对无逐笔成交市场）

国内期货市场不推送逐笔成交明细（Level-2 Transaction Data），需要通过综合推断获取交易流信号。

#### 10. TickTransactionInferer - Tick级交易推断器

**功能**：从Tick快照推断交易方向和成交量

**推断方法**：
| 方法 | 说明 | 置信度 |
|------|------|--------|
| BID_DEPLETION | 买盘量减少 → 卖方主动 | 中等 |
| ASK_DEPLETION | 卖盘量减少 → 买方主动 | 中等 |
| PRICE_UP | 价格上跳 → 买方主动 | 高 |
| PRICE_DOWN | 价格下跳 → 卖方主动 | 高 |
| SPREAD_CROSS | 穿价成交 | 高 |

**主要接口**：
```cpp
// 从Tick推断交易
InferredTransaction inferFromTick(
    double bid_px, double ask_px,
    double bid_vol, double ask_vol,
    double last_price, double last_vol,
    uint64_t timestamp
);

// 获取推断的Trade Imbalance
InferredTradeImbalance getInferredImbalance() const;
```

**配置参数**：
| 参数 | 说明 | 默认值 |
|------|------|--------|
| imbalanceWindowMs | Imbalance计算窗口 | 5000 |
| minConfidence | 最小置信度阈值 | 0.3 |
| largeTradeThreshold | 大单判定阈值 | 50.0 |

#### 11. SelfTradeCalibrator - 自身成交校准器

**功能**：
- 记录自身成交作为 Ground Truth
- 计算实现的逆向选择比例
- 为综合信号提供校准

**主要接口**：
```cpp
// 记录成交
void recordFill(const std::string& code, double price, double qty, 
                bool is_buy, double mid_price, double spread, uint64_t timestamp);

// 获取校准结果
CalibrationResult getCalibration(const std::string& code) const;

// 更新市场状态（计算成交后价格变动）
void onTick(const std::string& code, double mid_price, uint64_t timestamp);
```

**输出结果**：
| 字段 | 说明 |
|------|------|
| direction_bias | 方向偏差 [-1, 1] |
| toxicity_level | 毒性水平 [0, 1] |
| buy_adverse_ratio | 买入成交逆向比例 |
| sell_adverse_ratio | 卖出成交逆向比例 |
| confidence | 校准置信度 |

#### 12. SyntheticSignalFusion - 综合信号融合器

**功能**：融合多源信号生成综合交易推断

**信号源**：
| 源 | 权重 | 说明 |
|------|------|------|
| TickInference | 0.4 | Tick推断信号 |
| BookAnalysis | 0.4 | 订单簿不平衡信号 |
| SelfTradeCalibration | 0.2 | 自身成交校准 |

**自适应权重调整**：
- 高波动时降低Tick推断权重
- 低流动性时提高订单簿权重
- 样本不足时降低自身成交权重

**主要接口**：
```cpp
// 添加信号源
void addTickInference(const InferredTransaction& tick_inf);
void addBookSignal(const DepthImbalanceSignal& book_sig);
void addSelfTradeCalibration(const CalibrationResult& calib);

// 执行融合
SyntheticTransactionData fuse();
```

**输出结果**：
| 字段 | 说明 |
|------|------|
| direction_signal | 方向信号 [-1, 1] |
| confidence | 综合置信度 [0, 1] |
| is_strong_signal | 强信号标志 |
| has_calibration | 是否有校准数据 |

---

### 综合信号数据流

```
┌─────────────────────────────────────────────────────────────────┐
│                   综合信号处理流程                               │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  on_tick()                                                      │
│      │                                                          │
│      ├──► TickTransactionInferer::inferFromTick()              │
│      │         │                                                │
│      │         ▼                                                │
│      │    InferredTransaction (推断的交易)                       │
│      │         │                                                │
│      ├──► OrderBookAnalyzer::analyze()                          │
│      │         │                                                │
│      │         ▼                                                │
│      │    BookAnalysisResult (订单簿分析)                        │
│      │         │                                                │
│      └──► SelfTradeCalibrator::getCalibration()                 │
│                │                                                │
│                ▼                                                │
│          CalibrationResult (自身成交校准)                        │
│                │                                                │
│                ▼                                                │
│      SyntheticSignalFusion::fuse()                              │
│                │                                                │
│                ▼                                                │
│      SyntheticTransactionData (综合信号)                         │
│                │                                                │
│                ▼                                                │
│      MicroAlphaEngine::onSyntheticTransaction()                 │
│                │                                                │
│                ▼                                                │
│      ToxicFlowDetector::onSyntheticAlpha()                      │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘

  on_trade()
      │
      └──► SelfTradeCalibrator::recordFill()  (统一入口)
                │
                └──► ToxicFlowDetector::onSelfTradeCalibration()
```

---

### 三、自成交防护模块

#### SelfTradePrevention - 自成交防护

**职责**：检测并防止做市订单与套利订单之间的自成交

**检测逻辑**：
```
套利买入 → 检查是否有做市卖单在买入价或以下 → 风险
套利卖出 → 检查是否有做市买单在卖出价或以上 → 风险
```

**防护策略**：
| 策略 | 说明 |
|------|------|
| CANCEL_MM | 先撤销冲突的做市订单，再执行套利订单 |
| ADJUST_PRICE | 调整套利订单价格，避开做市挂单 |
| REJECT | 拒绝套利订单 |

**主要接口**：
```cpp
// 跟踪做市订单
void trackMMOrder(const std::string& code, uint32_t order_id, 
                  double price, double qty, bool is_buy, uint64_t timestamp);

// 检查套利订单
SelfTradeCheckResult checkOrder(const std::string& code, bool is_buy, 
                                 double price, bool is_market_order) const;
```

---

### 四、跨期价差套利模块

#### SpreadCalculator - 价差计算引擎

**支持价差类型**：
| 类型 | 公式 | 说明 |
|------|------|------|
| SIMPLE_DIFF | P1 - P2 | 简单价差 |
| RATIO | P1 / P2 | 价格比率 |
| LOG_DIFF | log(P1) - log(P2) | 对数价差 |
| WEIGHTED | w1*P1 - w2*P2 | 加权价差 |
| BASIS | Spot - Future | 基差 |

**统计计算**：
- 滚动均值/标准差
- Z-Score
- 相关系数
- Beta (OLS回归)
- 半衰期 (Ornstein-Uhlenbeck)

#### 策略详解

**1. MeanReversionStrategy (均值回归)**
```
入场: |Z-Score| > entry_threshold
离场: |Z-Score| < exit_threshold
止损: |Z-Score| > stop_loss_threshold
超时: 持仓时间 > convergence_timeout
```

**2. TrendFollowingStrategy (趋势跟踪)**
```
入场: Fast MA 上穿 Slow MA 且趋势确认
离场: MA 反向交叉 或 趋势耗尽
```

**3. PairsTradingStrategy (配对交易)**
```
入场: 残差超过阈值 (基于协整)
对冲: 动态Beta调整
离场: 残差回归
```

**4. StatisticalArbStrategy (统计套利)**
```
多因子: Z-Score(30%) + Momentum(20%) + Volatility(15%) + Correlation(20%) + MSpread(15%)
权重: 自适应调整
```

**5. MarketMakingEnhancer (做市增强)**
```
Z-Score高 → 扩大价差、暂停买报价
Z-Score低 → 扩大价差、暂停卖报价
中等偏离 → 调整报价倾斜
```

#### SpreadRiskManager - 风险管理

| 风险类型 | 监控指标 | 处理方式 |
|----------|----------|----------|
| 仓位风险 | 总仓位/单对仓位 | 限制新开仓 |
| 市场风险 | VaR (99%) | 警告/熔断 |
| 相关性风险 | 跨腿相关性 | 相关性破裂警告 |
| 收敛风险 | Z-Score偏离度 | 强制平仓评估 |

---

### 五、异步执行模块

#### LockFreeQueue - 无锁队列

**特性**：
- Single-Producer Single-Consumer (SPSC)
- Wait-free 操作（无 CAS 循环）
- Cache-friendly 设计

**主要接口**：
```cpp
bool tryPush(const T& item);      // 非阻塞推送
bool tryPop(T& item);              // 非阻塞弹出
bool empty() const;                // 队列空检查
```

#### AsyncArbitrageExecutor - 异步套利执行器

**架构**：
```
主线程:
  pushTick() → 非阻塞推送 (~50ns)
  processPendingOrders() → 执行订单 (~5μs)

套利线程:
  条件变量等待 → popAll(ticks) → 更新价差 → 生成信号 → 推送订单
```

**配置参数**：
| 参数 | 说明 | 默认值 |
|------|------|--------|
| signal_interval_us | 信号检查间隔(μs) | 5000 |
| max_wait_us | 最大等待时间(μs) | 10000 |
| ticks_per_signal | 每N个tick检查信号 | 5 |

**延迟特性**：
| 场景 | 延迟 |
|------|------|
| 高频行情(连续tick) | < 100μs |
| 中频行情(每5tick) | < 1ms |
| 低频行情(无tick) | < 10ms |

---

## 统一参数配置设计

### 设计原则

为了避免相似参数在多个配置节点中重复配置，采用**统一参数配置**模式：

```
┌─────────────────────────────────────────────────────────────────┐
│                   统一参数配置流程                               │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  config.yaml (顶层统一配置)                                     │
│       │                                                         │
│       │  skewSensitivity: 2.0                                   │
│       │  aggressiveSkewThreshold: 0.5                           │
│       │  stickyThreshold: 1.0                                   │
│       ▼                                                         │
│  UftFutuMmStrategy::init()                                      │
│       │                                                         │
│       │  _spread_optimizer->setSkewEnhancement(...)             │
│       │  _quoter->setStickyThreshold(...)                       │
│       ▼                                                         │
│  各模块内部使用统一参数                                         │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

### 参数对照表

| 统一参数 | 使用模块 | 说明 |
|----------|----------|------|
| stickyThreshold | FutuQuoter | 价格粘性阈值 |
| skewSensitivity | SpreadOptimizer | Skew 非线性增强系数 |
| aggressiveSkewThreshold | SpreadOptimizer | 激进 Skew 触发阈值 |
| oneSidedThreshold | UftFutuMmStrategy | 单向报价阈值 |
| marginCooldownMs | FutuRiskMonitor | 保证金不足冷却时间 |
| marginErrorThreshold | FutuRiskMonitor | 触发暂停的错误次数 |
| closeoutMinutesBefore | FutuRiskMonitor | 收盘前平仓时间 |

### 优势

1. **配置简洁**：避免重复配置，降低维护成本
2. **一致性**：确保各模块使用相同参数值
3. **可追溯**：参数来源清晰，便于调试

---

## 配置文件

### 完整配置示例

```yaml
# UFT 做市策略配置
strategies:
  uft:
    - id: futu_mm_if
      name: FutuMM
      params:
        # ================== 合约配置 ==================
        anchorCode: CFFEX.IF.2503        # 主力合约(对冲用)
        contracts:
          - code: CFFEX.IF.2503
            multiplier: 300              # 合约乘数(可选，自动获取)
            tickSize: 0.2                # 最小变动价位(可选)
            maxPosition: 50              # 单合约最大持仓
            maxDelta: 30                 # 单合约最大Delta
        
        # ============================================================
        # 统一参数配置 (全局生效，各模块共享)
        # ============================================================
        # Sticky 策略: 减少频繁撤单重报
        stickyThreshold: 1.0            # 价格粘性阈值(tick) - 价格偏离不超过此值不撤单
        
        # 增强 Skew: 更激进的库存回归
        skewSensitivity: 2.0            # Skew 非线性增强系数
        aggressiveSkewThreshold: 0.5    # 激进 Skew 触发阈值 (delta利用率)
        oneSidedThreshold: 0.8          # 单向报价阈值 (delta利用率)
        
        # 保证金不足处理
        marginCooldownMs: 30000         # 冷却时间 (毫秒)
        marginErrorThreshold: 3         # 触发暂停的连续错误次数
        
        # ============================================================
        # 收盘前平仓 (收盘时间从基础数据自动获取)
        # ============================================================
        closeoutMinutesBefore: 5        # 收盘前多少分钟停止报价 (0=不启用)
        closeoutFlattenPosition: true   # 收盘前是否平掉所有敞口
        
        # ================== 组合风险 ==================
        deltaLimit: 50.0                 # Delta阈值
        maxDelta: 100.0                  # 最大组合Delta
        maxExposure: 300.0               # 最大暴露
        hedgeThreshold: 30.0             # 对冲触发阈值
        hedgeRatio: 0.5                  # 对冲比例
        maxDailyLoss: -50000.0           # 最大日内亏损
        
        # ================== 报价参数 ==================
        numLevels: 3                     # 档位数量
        baseSpread: 2.0                  # 基础价差(tick)
        baseQty: 1.0                     # 基础量
        qtyDecay: 0.7                    # 外层衰减
        levelStep: 1.0                   # 档位间距(tick)
        
        # ================== 库存管理 ==================
        maxInventory: 100.0              # 最大库存
        skewFactor: 0.01                 # 库存倾斜因子 φ
        maxSkew: 5.0                     # 最大倾斜值
        targetInventory: 0               # 目标库存
        
        # ================== 高级开关 ==================
        useSpreadOptimizer: true         # 使用GLFT价差优化
        useAutoCancel: true              # 使用自动撤单
        useMarketState: true             # 使用市场状态检测
        useOrderBook: false              # 使用订单簿分析
        useAlphaEngine: true             # 使用Alpha预测
        useToxicityDetector: true        # 使用毒性检测
        useAdaptiveParam: false          # 自适应参数(默认关闭)
        usePerformanceMonitor: false     # 性能监控(默认关闭)
        usePerformanceAnalyzer: false    # 绩效分析(默认关闭)
        
        # ================== GLFT价差优化器 ==================
        spreadOptimizer:
          phi: 0.01                      # 库存惩罚系数
          volSensitivity: 1.0            # 波动率敏感度
          depthSensitivity: 0.5          # 深度敏感度
          volWindow: 100                 # 波动率窗口
          minSpreadMult: 0.5             # 最小价差倍数
          portfolioSkewWeight: 0.5       # 组合倾斜权重
          minCorrelation: 0.5            # 最小相关系数
        
        # ================== Alpha预测引擎 ==================
        alphaEngine:
          sensitivity: 0.5              # Alpha敏感度 η
          ofiWeight: 0.4                # OFI权重
          tradeWeight: 0.3              # 成交失衡权重
          leadlagWeight: 0.3            # 跨品种权重
          emaFactor: 0.3                # EMA平滑因子
          strongThreshold: 0.7          # 强信号阈值
        
        # ================== 毒性检测 ==================
        toxicityDetector:
          vpinThreshold: 0.7            # VPIN阈值
          window: 50                     # 检测窗口
          cooloffMs: 5000               # 冷却时间(ms)
        
        # ================== 市场状态检测 ==================
        marketState:
          volThreshold: 0.003           # 波动率阈值
          moveThreshold: 0.005          # 价格变动阈值
          spreadThreshold: 5.0          # 价差异常阈值
          volumeThreshold: 10.0         # 成交量异常阈值
          lookbackTicks: 50             # 回看tick数
          cooldownTicks: 20             # 冷却tick数
        
        # ================== 自动撤单 ==================
        autoCancel:
          maxAgeMs: 5000                # 最大存活时间(ms)
          priceDeviation: 3.0           # 价格偏离阈值(tick)
          cancelOnStateChange: true     # 状态变化时撤单
          cancelOnInventoryLimit: true  # 库存限制时撤单
        
        # ================== 风险监控 ==================
        riskMonitor:
          maxOrdersPerSec: 50           # 每秒最大下单数
          maxCancelsPerSec: 30           # 每秒最大撤单数
          maxTradesPerSec: 20            # 每秒最大成交数
        
        # ================== 相关性管理 ==================
        correlationManager:
          windowSize: 100               # 统计窗口
          minCorrelation: 0.5           # 最小相关系数
          spreadZThreshold: 2.0         # 价差Z阈值
        
        # ================== 自适应参数 ==================
        adaptiveParam:
          updateInterval: 100           # 更新间隔(tick)
          learningRate: 0.01            # 学习率
          minPhi: 0.001                 # 最小φ
          maxPhi: 0.1                   # 最大φ
        
        # ================== 绩效分析 ==================
        performanceAnalyzer:
          windowSize: 100               # 分析窗口
          sharpeRiskFree: 0.0           # 无风险利率
        
        # ================== 性能监控 ==================
        performanceMonitor:
          latencyThreshold: 100000      # 延迟阈值(ns)
        
        # ================== 跨期价差套利 ==================
        spreadArbitrage:
          enabled: true                 # 启用套利
          enhanceMarketMaking: true     # 做市增强
          maxPosition: 20.0             # 最大套利仓位
          entryZScore: 2.0              # 入场Z-Score
          exitZScore: 0.5               # 离场Z-Score
          stopLossZ: 4.0                # 止损Z-Score
          windowSize: 200               # 统计窗口
          convergenceTimeout: 3600      # 收敛超时(秒)
        
        # ================== 异步执行器 ==================
        asyncExecutor:
          signalIntervalUs: 5000        # 信号检查间隔(μs)
          maxWaitUs: 10000              # 最大等待(μs)
          ticksPerSignal: 5             # 每N个tick检查
        
        # ================== 自成交防护 ==================
        selfTradePrevention:
          enabled: true                 # 启用防护
          strategy: cancelMM            # 策略: cancelMM/adjustPrice/reject
          minPriceGap: 1.0              # 最小价格间隔(tick)
```

### 跨期价差对配置

价差对通过API动态添加：

```cpp
SpreadPairConfig pair_cfg;
pair_cfg.pair_id = "IF_2503_2506";
pair_cfg.leg1_code = "CFFEX.IF.2503";      // 近月
pair_cfg.leg2_code = "CFFEX.IF.2506";      // 远月
pair_cfg.spread_type = SpreadType::WEIGHTED;
pair_cfg.leg1_ratio = 1.0;
pair_cfg.leg2_ratio = -1.0;
pair_cfg.entry_z_threshold = 2.0;
pair_cfg.exit_z_threshold = 0.5;
pair_cfg.stop_loss_z = 4.0;
pair_cfg.max_spread_position = 20.0;
pair_cfg.primary_strategy = ArbitrageStrategy::MEAN_REVERSION;
pair_cfg.enhance_quoting = true;

_spread_arb_manager->addSpreadPair(pair_cfg);
```

---

## 编译与运行

### 编译

```bash
# 进入源码目录
cd wondertrader/src

# 创建构建目录
mkdir -p build && cd build

# 配置
cmake .. -DCMAKE_BUILD_TYPE=Release

# 编译
make WtFutuCore -j$(nproc)
```

### 运行

```bash
# 运行UFT策略
./WtUftRunner config.yaml
```

### 依赖

| 组件 | 说明 |
|------|------|
| WtUftCore | UFT框架核心 |
| WTSTools | 工具库 |
| Share | 公共工具 (RingBuffer, TimeUtils) |

---

## 性能指标

| 指标 | 目标值 | 实现方式 |
|------|--------|----------|
| Tick-to-Quote P99 | < 100μs | RingBuffer + 内联计算 |
| Alpha计算延迟 | < 50μs | O(1)滑动窗口 |
| 订单查找 | O(1) | 哈希表 |
| 热路径内存分配 | 0次 | 预分配缓冲区 |
| 套利信号延迟 | < 1ms | 条件变量唤醒 |
| 主线程做市延迟 | 无影响 | 异步执行 |

---

## 数据需求

| 数据类型 | 来源 | 用途 | 必须 |
|----------|------|------|------|
| Tick行情 | CTP | 做市报价 | 是 |
| 逐笔成交 | Level2 | Trade Imbalance | 否 |
| 委托队列 | Level2 | OFI增强 | 否 |
| 跨品种行情 | CTP | Lead-Lag | 否 |

---

## API 使用示例

### 基本使用

```cpp
// 创建策略
auto strategy = new UftFutuMmStrategy("futu_mm_001");

// 初始化配置 (通过配置文件)
strategy->init(config);

// 在 on_init 中自动订阅行情
// 在 on_tick 中自动处理报价逻辑
```

### 添加价差对

```cpp
// 在策略初始化后添加价差对
SpreadPairConfig pair;
pair.pair_id = "IC_2503_2506";
pair.leg1_code = "CFFEX.IC.2503";
pair.leg2_code = "CFFEX.IC.2506";
pair.entry_z_threshold = 2.0;
pair.exit_z_threshold = 0.5;

_spread_arb_manager->addSpreadPair(pair);
```

### 自定义信号回调

```cpp
_spread_arb_manager->setSignalCallback([](const SpreadSignal& signal) {
    if (signal.confidence > 0.7) {
        // 高置信度信号，执行交易
    }
});
```

---

## 常见问题

### Q: 做市延迟会受套利逻辑影响吗？

A: 不会。套利逻辑在独立线程中运行，主线程仅执行非阻塞的队列操作（~50ns），对做市延迟无影响。

### Q: 如何避免自成交？

A: SelfTradePrevention 模块会自动检测并处理。默认策略是先撤销冲突的做市订单，再执行套利订单。

### Q: 套利信号延迟是多少？

A: 使用条件变量唤醒，高频行情下延迟 < 100μs，低频行情下最大 10ms（超时保护）。

### Q: 如何选择套利策略？

A: 建议根据价差特性选择：
- 均值回归型价差 → MeanReversionStrategy
- 趋势型价差 → TrendFollowingStrategy
- 协整关系稳定 → PairsTradingStrategy
- 多因子驱动 → StatisticalArbStrategy

### Q: 收盘前平仓逻辑如何工作？

A: 系统自动从基础数据获取收盘时间，无需手动配置：

```
流程：
1. 策略启动时从 WTSSessionInfo::getCloseTime() 获取收盘时间
2. 收盘前 N 分钟 (closeoutMinutesBefore) 自动触发
3. 暂停报价 → 撤销所有挂单 → 对冲敞口
4. 新交易日自动重置状态
```

**支持不同品种的收盘时间**：
- 股指期货 IF/IC/IH: 15:15
- 商品期货: 15:00
- 夜盘品种: 根据各自交易时段

### Q: 如何配置统一参数？

A: 在策略顶层配置统一参数，系统自动分发给各模块：

```yaml
params:
  # 统一参数 (全局生效)
  stickyThreshold: 1.0
  skewSensitivity: 2.0
  aggressiveSkewThreshold: 0.5
```

无需在各子模块配置中重复设置。
