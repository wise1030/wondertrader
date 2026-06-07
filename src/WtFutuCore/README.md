# WtFutuCore — 期货高频做市引擎

基于 WonderTrader UFT 框架的期货高频做市 + 跨期价差套利引擎，采用 GLFT+Alpha 信号架构。

## 项目概览

| 项目 | 说明 |
|------|------|
| 语言 | C++17 |
| 框架 | WonderTrader UFT (Ultra-Fast Trading) |
| 编译产物 | `libWtFutuCore.so` (动态策略库) |
| 源文件 | 46 个 .h + 31 个 .cpp |
| 命名空间 | `futu` |
| 工厂名 | `FutuStraFact.FutuMM` |

## 架构总览

```
UftFutuMmStrategy (入口策略)
├── StrategyCoordinator (做市流水线)
│   ├── FutuPortfolio        (组合/持仓/Delta/敞口/对冲)
│   ├── FutuRiskMonitor      (风控状态机: 5级响应+自动恢复)
│   ├── ToxicFlowDetector    (毒性检测门面)
│   │   ├── PredictiveToxicity  (VPIN + OFI + Alpha)
│   │   ├── RealizedToxicity    (自成交校准)
│   │   └── SyntheticSignalFusion (内嵌3源融合)
│   ├── SignalAggregator     (per合约, 6源信号聚合)
│   │   ├── OFISignalSource         (订单流不平衡, w=0.35)
│   │   ├── TradeFlowSignalSource   (交易流, w=0.25)
│   │   ├── BookImbalanceSignalSource (簿不平衡, w=0.20)
│   │   ├── MomentumSignalSource    (动量EMA, w=0.15)
│   │   ├── LeadLagSignalSource     (跨合约领先滞后, w=0.05)
│   │   └── VolatilitySignalSource  (波动率, 辅助)
│   ├── SpreadOptimizer      (per合约, GLFT价差模型)
│   ├── FutuQuoter           (per合约, 多档双边报价)
│   ├── OrderRouter          (套利/对冲/平仓统一下单)
│   ├── UnifiedOrderTracker  (订单状态单一真相源)
│   ├── CorrelationManager   (跨合约相关性与beta)
│   ├── PerformanceMonitor   (无锁延迟/吞吐监控)
│   └── SelfTradeCalibrator  (自成交校准)
├── SpreadArbitrageManager (跨期套利协调器)
│   ├── SpreadCalculator / SpreadRiskManager
│   ├── MarketMakingEnhancer
│   └── 策略实例: MeanReversion / TrendFollowing / PairsTrading / StatisticalArb
├── AsyncArbitrageExecutor (独立线程, 无锁SPSC队列)
├── SelfTradePrevention
├── BilateralQuoteStats / PerformanceAnalyzer
└── FutuConfigValidator (启动时配置校验)
```

## 核心架构决策

### 双路径下单 (Dual-Path Order Routing)

```
┌───────────── 做市路径 (零延迟) ─────────────┐
│  FutuQuoter → ctx API (stra_buy/sell/quote)  │
│  无中间检查, 内联多档报价, 直接下单           │
└──────────────────────────────────────────────┘

┌───────────── 非做市路径 (限速+审计) ──────────┐
│  OrderRouter → ctx API (with guards)          │
│  · 自成交防护 (对抗做市挂单)                   │
│  · 按源限速: ARBITRAGE=30/s, HEDGING=10/s    │
│  · 优先级路由: CLOSEOUT > HEDGING > ARBITRAGE │
│  · 延迟预算: < 500ns/order                    │
└──────────────────────────────────────────────┘
```

### 统一交易状态 (TradingState)

替换原先分散的 5+ 个 bool 变量，集中管理所有交易状态：

```cpp
struct TradingState {
    bool trading_halted;    // 全暂停(风控触发)
    bool quoting_paused;    // 报价暂停(下单错误等)
    bool long_blocked;      // 禁止买入
    bool short_blocked;     // 禁止卖出
    bool toxicity_paused;   // 毒性熔断
    bool market_paused;     // 极端行情暂停
    bool closeout_mode;     // 收盘平仓模式
    PauseReason pause_reason; // 暂停原因追踪

    bool canQuote() const;  // 综合可报价判断
    bool canBuy()  const;   // 可买入判断
    bool canSell() const;   // 可卖出判断
};
```

### 信号架构 (SignalAggregator)

插件式信号源，配置驱动启用/禁用：

```
alpha = Σ(weight_i × signal_i) / Σ(weights)
confidence = consistency × strength × warmup_factor
```

| 信号源 | 权重 | 说明 |
|--------|------|------|
| OFI | 0.35 | 订单流不平衡 (Order Flow Imbalance) |
| TradeFlow | 0.25 | 净交易流方向 |
| BookImbalance | 0.20 | 订单簿买卖压力 |
| Momentum | 0.15 | 价格动量 EMA |
| LeadLag | 0.05 | 跨合约领先滞后预测 |
| Volatility | 辅助 | 已实现波动率 + 分层 |

### LeadLag 跨合约数据流

```
Anchor合约Tick到达
  → UftFutuMmStrategy::on_tick()
  → 遍历所有非Anchor合约的SignalAggregator
  → SignalAggregator::updateLeadContract(code, mid, ts)
  → LeadLagSignalSource::updateLeadContract()
  → calculateSignal(): signal = tanh(Σ(correlation × mid_change × 100) / Σ(correlation))
```

## 做市流水线 (StrategyCoordinator)

每个 tick 的处理流程：

```
processTick()
  1. preCheck()          → 会话/市场状态/毒性/风控预检
  2. updateMarketData()  → 更新 MarketDataContext
  3. updateSignals()     → SignalAggregator + ToxicFlowDetector
  4. checkRisk()         → FutuRiskMonitor 评估, 执行风控动作
  5. processQuoting()    → SpreadOptimizer → FutuQuoter.refreshQuotes()
  6. processAutoCancel() → 过时/偏价挂单清理
  7. checkAndHedge()     → Delta超限自动对冲 (via OrderRouter)
  8. updateAdaptiveParams() → 周期性参数微调
```

## 风控体系 (FutuRiskMonitor)

### 5级风险响应

| 级别 | 动作 |
|------|------|
| NORMAL | 正常报价 |
| WARNING | 日志警告 |
| ELEVATED | 扩大价差 |
| HIGH | 缩量报价 |
| CRITICAL | 暂停报价 |
| (超过阈值) | 暂停交易 + 撤全单 |

### 自动恢复机制
- **可逆风险** (Delta偏离、频率超限): 冷却后自动恢复
- **不可逆风险** (日亏损超限): 需人工干预

### 收盘平仓状态机
```
IDLE → PENDING → EXECUTING → COMPLETED
                  ↘ FAILED → RETRYING → ...
```

## 毒性检测 (ToxicFlowDetector)

门面模式组合三个子模块：

| 子模块 | 触发条件 | 输出 |
|--------|----------|------|
| PredictiveToxicity | 每tick | VPIN/OFI/Alpha预测毒性 |
| RealizedToxicity | 成交事件 | 自成交校准后的已实现毒性 |
| SyntheticSignalFusion | 每tick(内嵌) | 3源融合合成交易数据 |

融合权重: TickTransactionInferer=0.4, DepthImbalance=0.4, SelfTradeCalibration=0.2

## 套利子系统 (SpreadArbitrageManager)

### 支持的策略类型
- **MeanReversion**: 均值回归 (Z-score入场, 加仓安全间距0.75)
- **PairsTrading**: 配对交易 (协整p-value, MacKinnon近似)
- **TrendFollowing**: 趋势跟踪 (止损pct=2%, 最大趋势bar=50)
- **StatisticalArb**: 统计套利 (M-spread特征, volume imbalance)

### 异步执行
- `AsyncArbitrageExecutor`: 独立线程 + 无锁SPSC队列
- 两腿原子提交: req_id fetch_add(2), 连续ID分配
- 自成交检查: 对抗做市挂单快照

## 配置说明

### 配置文件结构

```
dist/WtRunnerFutu/
├── config.yaml              # 主策略配置
├── coordinator.yaml         # 模块参数配置
├── spread_arbitrage.yaml    # 跨期套利配置
├── hotparams.yaml           # 热更新参数(运行时可改)
├── actpolicy.yaml           # 买卖策略
├── logcfg.yaml              # 日志配置
├── mdparsers.yaml           # 行情解析模块
├── tdtraders.yaml           # 交易模块
├── common/                  # 基础数据(合约/品种/节假)
├── uft/                     # UFT框架数据
│   └── libWtFutuCore.so     # 策略动态库
├── Logs/                    # 运行日志
└── generated/outputs/       # 策略输出
```

### config.yaml 关键配置项

```yaml
# 锚定合约(LeadLag信号的领先合约)
anchorCode: "CFFEX.IF"

# 合约列表
contracts:
  - code: "CFFEX.IF"
    maxPosition: 20
    maxDelta: 10
    targetPosition: 0
  - code: "CFFEX.IC"
    maxPosition: 10
    maxDelta: 5

# 报价参数
quoting:
  numLevels: 3          # 报价档位
  baseSpread: 2.0       # 基础价差(tick)
  baseQty: 1            # 基础手数
  qtyDecay: 0.7         # 每档衰减
  useBilateralQuote: true

# 组合参数
portfolio:
  maxDelta: 30
  hedgeRatio: 1.0
  hedgeDeltaThreshold: 0.8   # Delta利用率触发对冲
  hedgeCooldownMs: 5000      # 对冲冷却时间

# 风控参数
risk:
  maxExposure: 500000
  maxDailyLoss: 50000
  maxOrdersPerSec: 30
  maxCancelsPerSec: 60

# 收盘平仓
closeout:
  minutesBefore: 5
  flattenPosition: true
  closeTime: 151000

# 模块开关
modules:
  useMarketMaking: true
  useSpreadArbitrage: true
  usePerformanceAnalyzer: true
```

### coordinator.yaml 关键配置项

```yaml
# 策略模式开关
useMarketMaking: true
useSpreadArbitrage: true
useSignalAggregator: true
useHedging: false           # 对冲默认关闭
hedgeDeltaThreshold: 0.8
hedgeCooldownMs: 5000

# 信号聚合器
signalAggregator:
  useOfi: true
  useTradeFlow: true
  useBookImbalance: true
  useMomentum: true
  useLeadLag: true
  ofiWeight: 0.35
  tradeWeight: 0.25
  bookImbalanceWeight: 0.20
  momentumWeight: 0.15
  leadLagWeight: 0.05
  warmupTicks: 50

# 毒性检测
toxicityDetector:
  vpinBucketSize: 50
  toxicityThreshold: 0.6
  toxicitySpreadFactor: 1.0

# GLFT价差优化
spreadOptimizer:
  baseSpread: 2.0
  phi: 0.20
  deltaSkewThreshold: 0.3
  deltaSkewFactor: 1.5
  deltaSkewPower: 1.5
  maxSpreadMult: 3.0
  minSpreadMult: 1.0

# 自成交校准
selfTradeCalibrator:
  toxicityWindowMs: 5000
  adverseThreshold: 0.6
```

### spread_arbitrage.yaml 关键配置项

```yaml
pairs:
  - leg1: "CFFEX.IF"
    leg2: "CFFEX.IC"
    ratio: 1.0
    entryZThreshold: 2.0
    exitZThreshold: 0.5
    stopLossPct: 0.02
    maxTrendBars: 50
    addSafetyRatio: 0.75

riskLimits:
  maxSpreadRisk: 100000
  maxDailyLoss: 20000
  maxDrawdown: 30000

# 统计套利子策略参数
statistical:
  meanReversion:
    entryZThreshold: 2.0
    stopLossZ: 3.0
    addSafetyRatio: 0.75
  pairsTrading:
    lookbackWindow: 100
    entryZThreshold: 2.0
  trendFollowing:
    stopLossPct: 0.02
    maxTrendBars: 50
```

### hotparams.yaml (运行时热更新)

```yaml
# 基础报价参数
baseSpread: 2.0
baseQty: 1
qtyDecay: 0.7
maxDelta: 30

# Alpha信号权重
ofiWeight: 0.35
tradeWeight: 0.25
bookImbalanceWeight: 0.20
momentumWeight: 0.15
leadLagWeight: 0.05

# GLFT参数
phi: 0.20
alphaSensitivity: 2.0
deltaSkewThreshold: 0.3
deltaSkewFactor: 1.5

# 价差乘子
maxSpreadMult: 3.0
minSpreadMult: 1.0
toxicitySpreadFactor: 1.0
ewmaDecay: 0.3
```

热更新通过共享内存同步，`on_params_updated()` 回调生效，无需重启策略。

## 编译与部署

### 编译环境

- OS: Linux (WSL Ubuntu 22.04 已验证)
- 编译器: g++ 11.4+ (需支持 C++17)
- CMake: 3.22+
- 依赖: WtUftCore, WTSTools, Share, boost_filesystem, pthread, atomic

### 编译命令

```bash
# 从项目根目录
cd /mnt/d/gf_pc/WonderTrader/wondertrader/src/build_all

# 首次或新增/删除源文件后需要重新cmake
cmake .

# 编译
make -j4 WtFutuCore
```

### 编译产物

```
build_all/build_x64/Debug/bin/WtUftRunner/futu/libWtFutuCore.so
```

### 部署

将编译产物复制到运行目录的 `uft/` 下：

```bash
cp build_all/build_x64/Debug/bin/WtUftRunner/futu/libWtFutuCore.so \
   dist/WtRunnerFutu/uft/
```

## 运行启动

### 启动命令

```bash
cd dist/WtRunnerFutu
./WtUftRunner ./config.yaml
```

### 启动流程

```
1. WtUftRunner 加载 config.yaml
2. 动态加载 libWtFutuCore.so (FutuStraFact.FutuMM)
3. UftFutuMmStrategy::init() 读取配置
4. initBusinessModules() 创建并连接所有组件:
   - FutuPortfolio (组合管理)
   - FutuRiskMonitor (风控)
   - SpreadOptimizer (per合约)
   - SignalAggregator (per合约, 含LeadLag配置)
   - FutuQuoter (per合约)
   - ToxicFlowDetector (含内嵌SyntheticSignalFusion)
   - OrderRouter (套利/对冲/平仓)
   - StrategyCoordinator (流水线)
   - SpreadArbitrageManager (套利)
   - AsyncArbitrageExecutor (异步执行)
   - PerformanceAnalyzer/Monitor
5. FutuConfigValidator 校验配置参数
6. 注册热更新参数
7. 订阅合约行情
8. 进入tick驱动循环
```

### 启动日志关键信息

```
SignalAggregator: N aggregators initialized (ofi=0.35, trade=0.25, book=0.20, mom=0.15, lead_lag=0.05)
Config validation passed (0 errors, N warnings)
UftFutuMmStrategy[X] session begin: YYYYMMDD
```

### 运行时监控

- **日志**: `dist/WtRunnerFutu/Logs/` 下按日期滚动
- **报价日志**: `[QUOTE]` 前缀, 每tick输出mid/alpha/skew/spread/bid/ask
- **风控日志**: `[RISK]` 前缀, 风险等级变化/动作执行
- **套利日志**: `[SPREAD_ARB]` 前缀, 信号触发/成交
- **绩效日志**: `[PERF]` 前缀, session_end时输出完整绩效报告
- **双边统计**: `[BILATERAL_STATS]` 前缀, session_end时输出

## 热更新参数

运行时可通过修改 `hotparams.yaml` + 共享内存同步更新以下参数，无需重启：

| 参数 | 说明 |
|------|------|
| baseSpread / baseQty / qtyDecay | 基础报价参数 |
| maxDelta | 最大Delta |
| ofiWeight / tradeWeight / ... | Alpha信号权重 |
| phi / alphaSensitivity | GLFT模型参数 |
| deltaSkewThreshold / deltaSkewFactor | Delta偏斜参数 |
| maxSpreadMult / minSpreadMult | 价差乘子范围 |
| toxicitySpreadFactor | 毒性价差扩大因子 |
| ewmaDecay | Alpha EWMA衰减因子 |

## 模块清单

### 做市核心
| 模块 | 文件 | 说明 |
|------|------|------|
| UftFutuMmStrategy | .h/.cpp | 入口策略, GLFT+Alpha做市框架 |
| StrategyCoordinator | .h/.cpp | 做市流水线编排 |
| FutuQuoter | .h/.cpp | 多档双边报价引擎 |
| SpreadOptimizer | .h/.cpp | GLFT价差优化(公允价+偏斜) |
| SignalAggregator | .h | 6源信号聚合 |
| OrderRouter | .h/.cpp | 非做市统一下单路由 |
| TradingState | .h | 统一交易状态管理 |

### 信号源
| 模块 | 文件 | 说明 |
|------|------|------|
| ISignalSource | .h | 信号源插件接口 |
| OFISignalSource | .h | 订单流不平衡 |
| TradeFlowSignalSource | .h | 交易流分析 |
| BookImbalanceSignalSource | .h | 订单簿不平衡 |
| MomentumSignalSource | .h | 价格动量 |
| LeadLagSignalSource | .h | 跨合约领先滞后 |
| VolatilitySignalSource | .h | 已实现波动率 |

### 风控与毒性
| 模块 | 文件 | 说明 |
|------|------|------|
| FutuRiskMonitor | .h/.cpp | 5级风控+自动恢复+收盘平仓 |
| ToxicFlowDetector | .h/.cpp | 毒性检测门面 |
| PredictiveToxicity | .h/.cpp | VPIN+OFI+Alpha预测毒性 |
| RealizedToxicity | .h/.cpp | 已实现毒性 |
| SyntheticSignalFusion | .h/.cpp | 3源信号融合 |
| TickTransactionInferer | .h | Tick级交易推断 |
| SelfTradeCalibrator | .h/.cpp | 自成交校准 |
| SelfTradePrevention | .h/.cpp | 自成交防护 |

### 组合与持仓
| 模块 | 文件 | 说明 |
|------|------|------|
| FutuPortfolio | .h/.cpp | 组合管理(Delta/敞口/对冲) |
| UnifiedOrderTracker | .h/.cpp | 订单状态单一真相源 |
| CorrelationManager | .h/.cpp | 跨合约相关性与beta |

### 套利
| 模块 | 文件 | 说明 |
|------|------|------|
| SpreadArbitrageManager | .h/.cpp | 跨期套利协调器 |
| SpreadCalculator | .h/.cpp | 价差计算 |
| SpreadRiskManager | .h/.cpp | 套利风控 |
| AsyncArbitrageExecutor | .h/.cpp | 异步套利执行 |
| MeanReversionStrategy | .h/.cpp | 均值回归 |
| TrendFollowingStrategy | .h/.cpp | 趋势跟踪 |
| PairsTradingStrategy | .h/.cpp | 配对交易 |
| StatisticalArbStrategy | .h/.cpp | 统计套利 |
| MarketMakingEnhancer | .h/.cpp | 套利信号增强做市 |

### 基础设施
| 模块 | 文件 | 说明 |
|------|------|------|
| MarketDataContext | .h/.cpp | 行情深度+交易流门面 |
| FutuConfig | .h/.cpp | 配置读取工具 |
| FutuConfigValidator | .h | 配置校验 |
| FutuComponentFactory | .h/.cpp | 依赖注入工厂 |
| PerformanceMonitor | .h/.cpp | 无锁延迟/吞吐监控 |
| PerformanceAnalyzer | .h/.cpp | 绩效分析 |
| BilateralQuoteStats | .h/.cpp | 双边报价统计 |
| AlphaTypes | .h | Alpha类型定义 |
| FutureTypes | .h | 期货类型定义 |
| SpinLockGuard | .h | 自旋锁RAII |

## 设计原则

1. **单一真相源**: TradingState管交易状态, UnifiedOrderTracker管订单状态, SignalContext管信号状态, FutuPortfolio管持仓状态
2. **双路径下单**: 做市零延迟直调ctx API; 非做市走OrderRouter限速+防自成交+审计
3. **插件信号架构**: ISignalSource接口, 配置驱动启用/禁用, 统一SignalContext输出
4. **门面模式**: ToxicFlowDetector(预测+已实现+融合), MarketDataContext(簿+流), SpreadArbitrageManager(计算+风控+策略)
5. **无锁热路径**: 原子计数器限速, 预分配向量, 内联价格计算
6. **状态机安全**: CloseoutState验证转换, TradingState集中变更
7. **分级风控响应**: NORMAL→WARNING→ELEVATED→HIGH→CRITICAL, 渐进动作(警告→扩差→缩量→单边→暂停→平仓→停止)
8. **可恢复机制**: 可逆风险冷却后自动恢复; 不可逆风险(日亏)需人工干预
9. **热参数更新**: 共享内存同步, on_params_updated()回调, 22个参数无需重启
10. **异步套利**: 独立线程+无锁SPSC队列, ~50ns tick推送, 自成交检查对抗做市快照
