# WtFutuCore — 期货高频做市引擎

基于 WonderTrader UFT 框架的期货高频做市 + 跨期价差套利引擎，采用 GLFT+Alpha 信号架构。

## 项目概览

| 项目 | 说明 |
|------|------|
| 语言 | C++17 |
| 框架 | WonderTrader UFT (Ultra-Fast Trading) |
| 编译产物 | `libWtFutuCore.so` (动态策略库) |
| 源文件 | 90 个 .h/.cpp/.hpp (含拆分组件), 约 3.0 万行 |
| 命名空间 | `futu` |
| 工厂名 | `FutuStraFact.FutuMM` |

## 架构总览 (v7.0 - 含 V6/V7 深度分析修复)

```
UftFutuMmStrategy (入口策略, ~2100行, 较 v4 减 800行)
├── FutuConfigLoader        (配置解析+校验, 拆分组件)
├── FutuHotParamManager     (26热参数注册+分发, 拆分组件)
├── CloseoutOrchestrator    (收盘平仓编排, 拆分组件)
├── ArbExecutionBridge      (套利执行桥+残腿防护, 拆分组件)
├── StrategyCoordinator (做市流水线)
│   ├── FutuPortfolio        (组合/持仓/Delta/敞口/对冲)
│   ├── FutuRiskMonitor      (风控状态机: 7级响应[soft+hard]+EventNotifier告警+自动恢复+滑窗读侧剔除)
│   ├── ToxicFlowDetector    (毒性检测门面)
│   │   ├── PredictiveToxicity  (VPIN + OFI + Alpha, warmup 期 alpha 通道保留)
│   │   ├── RealizedToxicity    (自成交校准)
│   │   └── SyntheticSignalFusion (hasAnySource 门控, 无源时跳过)
│   ├── SignalAggregator     (per合约, 6源信号聚合, SignalSlot 表驱动)
│   │   ├── ICWeightTracker        (三层权重: 基础×市场状态×IC, cap 归一化后施加)
│   │   ├── OFISignalSource         (订单流不平衡)
│   │   ├── TradeFlowSignalSource   (交易流, 滑窗衰减)
│   │   ├── BookImbalanceSignalSource (簿不平衡)
│   │   ├── MomentumSignalSource    (动量 O(1) 增量 log 收益)
│   │   ├── LeadLagSignalSource     (跨合约领先滞后)
│   │   └── VolatilitySignalSource  (波动率, 辅助)
│   ├── SpreadOptimizer      (per合约, GLFT价差模型)
│   ├── FutuQuoter           (per合约, 多档双边报价)
│   ├── OrderRouter          (套利/对冲/平仓统一下单, <500ns/order)
│   ├── UnifiedOrderTracker  (订单状态单一真相源, original_qty 部分成交跟踪)
│   ├── CorrelationManager   (跨合约相关性与beta)
│   ├── PerformanceMonitor   (无锁延迟/吞吐监控)
│   └── SelfTradeCalibrator  (自成交校准)
├── SpreadArbitrageManager (跨期套利协调器)
│   ├── ISpreadStrategy 注册表 (插件化, 新增策略 1 行注册)
│   ├── SpreadCalculator / SpreadRiskManager
│   ├── MarketMakingEnhancer
│   └── 策略实例: MeanReversion / TrendFollowing / PairsTrading / StatisticalArb
├── AsyncArbitrageExecutor (独立线程, 无锁SPSC队列, 跨线程安全)
│   配置: useAsyncArbThread=true(实盘异步) / false(回测同步)
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

分层状态机 (HSM)，集中管理做市阶段 + 报价子状态，单线程写契约 + DEBUG 线程断言：

```cpp
struct TradingState {
    MmPhase      phase;         // 做市阶段: QUOTING / CLOSEOUT
    QuotingPhase qphase;        // 报价子状态: NORMAL / RISK_HALTED / TOXICITY / MARKET / ERROR
    bool long_blocked;          // 禁止买入
    bool short_blocked;         // 禁止卖出

    // RISK_HALTED -> NORMAL 唯一合法出口 (防误恢复)
    void resumeFromRisk();
    bool setQuotingPhase(QuotingPhase q);  // canTransitionQuoting 校验
    bool tryResumeFrom(QuotingPhase expected); // 条件恢复 (防跨态闪烁)
};
```

> **V7 修复 (A1)**: 外部恢复路径 (on_trade / channel_ready) 现通过 `Coordinator::onExternalResumeFromRisk()` 同步重置软风控倍数 `_risk_spread_mult`，避免恢复后报价宽度被永久放大。

### 信号架构 (SignalAggregator + 三层权重框架)

插件式信号源 + 自适应权重框架，配置驱动启用/禁用：

```
alpha = Σ(dynamic_weight_i × normalized_signal_i) / Σ(dynamic_weights)
```

**三层权重模型** (ICWeightTracker.h):

```
最终权重 = 基础逻辑权重 × 市场状态调节 × 在线可信度调节

第一层: 基础逻辑权重 (静态, 按交易逻辑设定)
第二层: 市场状态调节 (波动率/趋势/流动性 regime)
  - OFI: 薄流动性 ×0.5, 深 ×1.5
  - TradeFlow: 高波动 ×1.3, 低 ×0.7
  - Momentum: 趋势 ×1.5, 震荡 ×0.5
  - LeadLag: 跨期品种 ×1.5
第三层: 在线可信度 (滚动 IC + 信号一致性)
  - floor=0.05 / cap=0.50, 不归零不独占
  - IC 低 ≠ 信号无效 (可能是参数不适配)
```

**信号幅度归一化** (RollingScaleTracker):
所有信号在加权前通过滚动 p95 归一化到可比范围，确保权重真正有效。

| 信号源 | 基础权重 | 说明 |
|--------|---------|------|
| OFI | 0.25 | 订单流不平衡 (Order Flow Imbalance) |
| TradeFlow | 0.20 | 净交易流方向 (统计显著性归一化) |
| BookImbalance | 0.20 | 订单簿买卖压力 |
| Momentum | 0.15 | 价格动量 EMA |
| LeadLag | 0.20 | 跨合约领先滞后预测 (scale_factor=3000, bps 缩放) |
| Volatility | 辅助 | 已实现波动率 + 分层 |

### LeadLag 跨合约数据流

```
Anchor合约Tick到达
  → UftFutuMmStrategy::on_tick()
  → 遍历所有非Anchor合约的SignalAggregator
  → SignalAggregator::updateLeadContract(code, mid, ts)
  → LeadLagSignalSource::updateLeadContract()
  → calculateSignal(): signal = tanh(Σ(correlation × mid_change × scale_factor) / Σ(correlation))
```

## 做市流水线 (StrategyCoordinator)

每个 tick 的处理流程：

```
processTick()
  1. preCheck()          → 会话/市场状态/毒性/风控预检
  2. updateMarketData()  → 更新 MarketDataContext
  3. updateSignals()     → SignalAggregator + ToxicFlowDetector
  4. checkRisk()         → FutuRiskMonitor 评估, 执行风控动作
     └ PAUSE_QUOTING 时仍执行 checkTakerReduce (减仓才能恢复; v7.2 已删除 checkAndHedge)
  5. processQuoting()    → SpreadOptimizer → FutuQuoter.refreshQuotes() (B1条件式重挂, v7.2黏性生效)
  6. processAutoCancel() → 过时/偏价挂单清理
  7. checkTakerReduce()  → 合约util≥takerReduceThreshold(1.1)时FAK对手价减仓
  8. updateAdaptiveParams() → 周期性参数微调
```

成交回调 (on_trade): 组合记账 → arb桥 → scout识别(v7.2: 自由层成交撤同侧义务层)
→ 条件式重挂(回测cancelAll仅深度破坏时 / 生产requoteAfterFill)

## 风控体系 (FutuRiskMonitor)

### v7.1 连续控制重设计 (2026-07-23)

**核心原则**: 仓位风险由**无状态连续控制**处理, 报价永在线(做市义务); 离散硬动作只保留给
真正的极端情况(日亏损/组合敞口)。仓位 breach **不再触发** BLOCK_SIDE/PAUSE_QUOTING/RISK_HALTED。

**统一利用率口径**: `util = (|pos| + 同向pending) / maxPos` — skew/qty衰减/义务/taker 共用

| 层 | 触发条件 | 动作 | 类型 |
|----|---------|------|------|
| NORMAL | util < 0.8 | 正常报价 (skew + qty衰减连续调节) | 连续 |
| **WIDEN_SPREAD** | 组合delta util ≥ 0.8/0.9 | `spread_mult = 1.2/1.5` (每tick无状态重算) | 策略(soft) |
| **skew 穿越授权** | util ≥ 1.0 | 减仓侧允许穿越 mid 最多 `skewCrossMaxTicks`(3), 主动减仓 | 连续 |
| **obligation reduce** | util ≥ 1.0 | 加仓侧=带宽极限价+min qty; 减仓侧=skew攻击性(clamp不覆写) | 连续 |
| **TAKER_REDUCE** | util ≥ 1.3 | FAK对手价平到 0.8×maxPos, 每合约30s限频 | 离散(主动吃单) |
| FLATTEN_POSITION | 非仓位类 breachCount ≥ 2 | 撤单 + 停 arb + anchor 强平 | 硬风控 |
| HALT_TRADING | DAILY_LOSS CRITICAL 或任意 CRITICAL | IRREVERSIBLE 强平 + 全停 | 终极 |

**设计要点**:
- **无状态 = 无死锁**: 仓位调节是 util 的纯函数, 仓位降了 skew 自动缓和, 无需恢复状态机
  (旧设计: PAUSE→撤单→无成交→仓位永不降→永不恢复, 覆盖率1-9% 的结构性根因)
- **T4 clamp 修复**: obligation 减仓侧 `askPrice=min(skew价, 带宽上限)`, 保留 skew 攻击性;
  旧覆写把减仓侧钉死在 mid+10ticks(被动), 最需要减仓时最不积极
- **穿越权限**: GLFT skew 旧 clamp(±half_spread) 使减仓能力在 util=1.0 封顶;
  v7.1 扩展到 half_spread+3ticks, 配合归一化 skew (`util^1.5×gain`, 1.0=贴mid)
- BLOCK_SIDE 保留给 DELTA/EXPOSURE 组合级 breach (罕见); POSITION_NET 仅 alert 上报
- 配置: coordinator.yaml `takerReduceThreshold/TargetUtil/CooldownMs`,
  spreadOptimizer `inventorySkewGain/skewCrossMaxTicks`

### v7.1 配套修复 (2026-07-24, 回测验证通过)

| 修复 | 位置 | 说明 |
|---|---|---|
| **分向成本簿记账** | FutuPortfolio::onTradeFill + ContractState long/short_qty/avg | offset 标志驱动, 替代净额推断。旧净额均价在 MM+arb 共享净头寸时被 arb 腿污染 → daily_pnl 假阳性日亏(引擎真账+34k vs 内部账-3.7M) → IRREVERSIBLE halt 误杀。markToMarket/setReferencePrice/resetDailyPnl 同步分向化 |
| **closeout 窗口禁 arb** | UftFutuMmStrategy::on_tick step 6 | `phase != CLOSEOUT` 才喂 arb tick。旧逻辑 14:45 平仓后 arb 继续开新价差仓到 15:00, 重建 ~50 手 delta 过夜 |
| **成交后立即重挂** | StrategyCoordinator::requoteAfterFill | 单边成交侵蚀深度 < min_valid_qty → 立即撤剩余单按最近 tick 参数重挂, 恢复双边义务; 200ms 限频. 配置 `requoteAfterFillMinIntervalMs` |
| **日界自动清除 IRREVERSIBLE** | FutuRiskMonitor::resetDaily | `autoClearIrreversibleOnReset`(默认 false, 回测开), 模拟隔夜人工复核 |

**5日 EC 回测验证 (6/08-6/12)**:
- 仓位死锁消除: util 达 1.06 (proj 53/50) 仅触发 obligation reduce, 报价不停; TAKER_REDUCE 未触发(仓位被连续控制压在 1.3 以内)
- 日亏假阳性消除: 全 5 天无 LOSS_CRITICAL 误报, 5 次 QUOTING_PAUSED 均为 closeout 正常流程
- 引擎资金曲线: +444,940 (修复前同窗口 +34k 且 Day2-5 被误 halt)
- requoteAfterFill 触发 279 次

### v7.1 第二轮修复 (2026-07-24, 覆盖率根因治理)

覆盖率仍低(<1%) → 二轮深挖定位三个根因, 均在 WtFutuCore 内修复:

| 修复 | 位置 | 说明 |
|---|---|---|
| **双边统计以引擎确认为准** | FutuQuoter::onEntrustAck/onOrder; 删 refreshQuotes末/placement时/onTrade 的统计 | 报单"在场时间"从引擎确认(on_entrust)起算, 不含发出→确认的网络延迟 (建模延迟)。mocker 新单只有 on_entrust 异步(postTask), on_order 仅在成交/撤单/每tick匹配心跳触发; 每笔 on_trade 必伴随 on_order(leftQty) → onTrade 统计冗余已删 |
| **价格保护可配置化** | config `protectTicks`/`priceProtection` | protectTicks 语义=允许比盘口好的最大 tick 数("最大可以穿多少")。远月宽盘口(11-34t)合约须 ≥ obligationMaxSpreadTicks, 否则报价被钳在盘口边缘→双边价差超带宽(ec2609 实测 8%)。priceProtection:false 完全关闭 |
| **session 休息段暂停** | StrategyCoordinator::processSectionBreak + on_tick step6 门控 | 每节收盘前 N 分钟(`sectionBreakMinutesBefore`,默认1)撤全部报价+arb在途单+停报价/套利, 下一节首tick自动恢复。每日最后一节跳过(归 closeout)。注意 stra_get_time 返回 HHMM(4位) |
| **节流统一 replay 时钟** | 全策略决策路径(coordinator/risk/router/orchestrator/bridge/manager) | 回测中墙钟节流随机器速度漂移→订单序列不可复现。统一改 tick actiondate/actiontime 推出的 replay ms, 墙钟仅用于纯日志限频 |
| **诊断计数器** | BilateralQuoteStats 非双边原因分类 | formatString 输出 inv[both/bid/ask/cross/wide] 计数, 定位覆盖率失真根因 |

**5日 EC 回测验证 (6/08-6/12, protectTicks=10)**:
- ec2607/08 双边覆盖率: **75-84%** (vs 修复前 <1%)
- ec2609 双边覆盖率: **8% → 51-56%** (远月盘口宽, protectTicks=10 允许深入盘口)
- SECTION_BREAK 触发 20 次 (2 break/天 × 5天 × 2 合约档), 正常
- 仓位连续控制工作: util 1.06 → obligation reduce, 报价不停
- **Day3 真实日亏 halt** (-953k): 50手 maxDelta × 50乘数 = 2500元/点, 200k 日亏限 ≈ 80点反向; EC 日内常波动 80+点 → **日亏限对 50手 EC 结构性偏紧**, 属业务风控参数决策(非 bug)

**已知外部限制**: 回测仍有轻微不可复现 (WtBtCore/HftMocker.cpp `splitVolume()` 用 srand(time) 随机拆分成交 → 同配置两次运行成交序列有噪声), 评估策略表现需多次运行取均值。详见 `AGENTS.md`。

### 告警外发通道 (R1 接线)

风控告警通过 **EventNotifier**(nanomsg PUB/SUB)外发,供运维侧(钉钉/webhook 等)订阅:

```
FutuRiskMonitor.broadcastAlert (22 处) ──直达──→ EventNotifier.notify("RISK_TYPE", msg)
SpreadArbitrageManager._alert_callback    ──回调──→ UftFutuMmStrategy.handleRiskAlert
                                                         ↓
                                                  EventNotifier.notify("ARB_RISK", msg)
```

**注入链路**(方案 B 策略层转发,保持模块边界):
- `WtUftRunner.initUftStrategies` → `UftStraContext.setEventNotifier(&_notifier)`
- 策略 `on_init` → `dynamic_cast<UftStraContext*>` → `setEventNotifier(getEventNotifier())`
- `setEventNotifier` 内:RiskMonitor 直达 + ArbManager 回调转发(解耦)

**告警类型**:
- RiskMonitor: `EXPOSURE_BREACH` / `LOSS_CRITICAL` / `POSITION_BREACH` / `TRADING_HALTED` / `QUOTING_PAUSED` / `CLOSEOUT_*` / `DELTA_RATE_BREACH` 等 16 类
- SpreadArbitrageManager: `ARB_RISK` topic(含 CORRELATION_BREAK / DIVERGENCE / POSITION_LIMIT / STOP_LOSS / B5 OVERSHOOT)

**yaml 配置**(notifier 节点,已由 WtUftRunner 读取):
```yaml
notifier:
  active: true
  url: "ipc://risk_events.ipc"   # nanomsg PUB 端点 (运维侧 SUB connect)
```

### 自动恢复机制
- **仓位 breach** (v7.1): 无状态连续控制, 无需恢复 — 仓位回落 skew/qty 自动缓和
- **可逆风险** (Delta偏离、EXPOSURE breach、频率超限): 冷却后自动恢复
- **不可逆风险** (日亏损超限): 需人工干预(`clearIrreversible`)
- **BLOCK_SIDE 恢复**(R2.7, 仅 DELTA/EXPOSURE 触发): BLOCK_SIDE_LONG/SHORT 设 `qphase=RISK_HALTED`,走统一恢复路径(`canRecover` + `resumeFromRisk` + `unblockLong/Short`)

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
src/WtFutuCore/config/         # 配置示例 (参考部署用)
├── config.yaml              # 主策略配置 (身份+业务参数, 不含模块开关)
├── coordinator.yaml         # 模块开关(唯一权威)+模块参数
├── spread_arbitrage.yaml    # 跨期套利配置
└── hotparams.yaml           # 热更新参数(运行时可改)

dist/WtRunnerFutu/             # 实盘部署目录
├── config.yaml              # (同上, 含行情/交易通道配置)
├── coordinator.yaml
├── spread_arbitrage.yaml
├── hotparams.yaml
├── actpolicy.yaml             # 买卖策略
├── logcfg.yaml                # 日志配置
├── mdparsers.yaml             # 行情解析模块
├── tdtraders.yaml             # 交易模块
├── common/                    # 基础数据(合约/品种/节假)
├── uft/                       # UFT框架数据
│   └── libWtFutuCore.so       # 策略动态库
├── Logs/                      # 运行日志
└── generated/outputs/         # 策略输出
```

### 配置分层说明 (v5 模块化统一)

### 配置分层说明 (v5+ — 单一权威位置)

| 文件 | 职责 | 开关承载 |
|------|------|---------|
| config.yaml | 策略身份 + 业务参数 | **不承载任何开关** |
| coordinator.yaml | 模块开关 + 模块参数 | 策略级7开关(根级) + 模块级4开关(modules.&lt;name&gt;.enabled) |
| spread_arbitrage.yaml | 套利子系统 | pair配置/风控/子策略参数 |
| hotparams.yaml | 热更新参数 | 26个参数运行时生效(共享内存同步) |

> **单一权威原则**: 每个开关有且只有一个书写位置, 多处书写不会被合并/覆盖, 而是直接报错或被忽略。代码内 fallback 仅表示"键缺失时使用编译期默认值", 不构成第二处配置。

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
  numLevels: 2          # 报价档位 (2 = L0自由探测层 + L1义务层)
  obligationLevel: 1    # 义务层档位 (0=L0义务无自由层; 1=scout结构, v7.2)
  scoutQty: 1.0         # 自由探测层手数 (<义务层; 成交即撤同侧义务层)
  baseSpread: 2.0       # 基础价差(tick)
  baseQty: 1            # 基础手数
  levelQtyMultiplier: 0.7         # 每档衰减 (义务层不适用, 按baseQty计)
  useBilateralQuote: true

# 组合参数
portfolio:
  maxDelta: 30
  hedgeRatio: 1.0              # 对冲比率 (CloseoutExecutor 用)
  # hedgeDeltaThreshold/hedgeCooldownMs 已随做市阶段Hedge删除 (v7.2):
  # 仓位风险由连续控制链处理 (WIDEN→skew→takerReduce→PAUSE→HALT)

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
# useHedging/hedgeDeltaThreshold/hedgeCooldownMs 已删除 (v7.2):
# 做市阶段Hedge移除, 仓位风险由连续控制链 + checkTakerReduce(takerReduceThreshold: 1.1)处理

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

# 套利风控参数 (H4: 已接线, 数据源=全组合 PnL)
riskLimits:
  portfolioStopLoss: 50000.0       # 组合峰值回撤止损 (drawdown>此值 → EMERGENCY)
  maxTotalPosition: 50.0           # 最大总价差持仓
  maxSinglePair: 20.0              # 单 pair 最大持仓

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
baseQty: 2.0
levelQtyMultiplier: 0.7
levelStep: 1.0
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

# 报价粘性/保护
stickyThreshold: 1.0
improveRetreatRatio: 2.0
protectTicks: 1.0
maxPriceDeviation: 20.0
```

热更新通过共享内存同步，`on_params_updated()` 回调生效，无需重启策略。26 个参数穿透到 FutuQuoter（报价参数+重算预计算表）、Coordinator（maxDelta+alphaSensitivity）、AdaptiveWeightFramework（信号权重 Layer1）。

### useAsyncArbThread 配置

```yaml
# coordinator.yaml 根级 (唯一权威位置)
coordinator:
  useAsyncArbThread: true   # 实盘: 独立arb线程(低延迟), 回测: false(主线程同步)
```

异步模式: 主线程 pushTick(~50ns SPSC) → arb 线程 processTick+generateSignals → 订单请求 SPSC → 主线程 processPendingOrders 经 OrderRouter 下单。下单始终在主线程执行（ctx 无线程安全问题）。

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
| baseSpread / baseQty / levelQtyMultiplier | 基础报价参数 |
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
| UftFutuMmStrategy | .h/.cpp | 入口策略, 回调分发+轻量委托 |
| FutuConfigLoader | .h/.cpp | 配置解析+边界校验 (拆分组件) |
| FutuHotParamManager | .h/.cpp | 26热参数注册+分发 (拆分组件) |
| CloseoutOrchestrator | .h/.cpp | 收盘平仓全生命周期编排 (拆分组件) |
| ArbExecutionBridge | .h/.cpp | 套利执行桥+残腿防护 (拆分组件) |
| StrategyCoordinator | .h/.cpp | 做市流水线编排 |
| FutuQuoter | .h/.cpp | 多档双边报价引擎 |
| SpreadOptimizer | .h/.cpp | GLFT价差优化(公允价+偏斜) |
| SignalAggregator | .h | 6源信号聚合(SignalSlot表驱动) |
| ICWeightTracker | .h | 三层权重框架 + RollingScaleTracker + IC追踪 |
| OrderRouter | .h/.cpp | 非做市统一下单路由 |
| TradingState | .h | 统一交易状态管理 (分层状态机) |
| ISpreadStrategy | .h | 套利策略插件接口 + 注册表 |

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
| LockFreeQueue | .hpp | SPSC无锁队列(cache line 对齐) |

## 设计原则

1. **单一真相源**: TradingState管交易状态, UnifiedOrderTracker管订单状态(含original_qty部分成交跟踪), SignalContext管信号状态, FutuPortfolio管持仓状态
2. **双路径下单**: 做市零延迟直调ctx API; 非做市走OrderRouter限速+防自成交+审计
3. **插件架构**: ISignalSource接口(信号源)+ISpreadStrategy接口(套利策略), 注册表驱动, 配置启用/禁用
4. **门面模式**: ToxicFlowDetector(预测+已实现+融合), MarketDataContext(簿+流), SpreadArbitrageManager(计算+风控+策略)
5. **无锁热路径**: 原子计数器限速, 预分配向量, TickContext指针预解析, 内联价格计算, spinlock alignas(64) 隔离 cacheline
6. **状态机安全**: CloseoutState验证转换, TradingState HSM分层, RISK_HALTED唯一恢复出口, 外部恢复路径同步重置软风控倍数
7. **分级风控响应**: NORMAL→WARNING→ELEVATED→HIGH→CRITICAL, 渐进动作+滑窗读侧剔除+自动恢复+delta-rate检测与状态分离
8. **可恢复机制**: 可逆风险冷却后自动恢复(恢复上限per-session); 不可逆风险(日亏)需人工干预; 撤单ack超时强制untrack
9. **热参数更新**: 共享内存同步, on_params_updated()回调, 26个参数穿透到Quoter/Coordinator/权重框架
10. **异步套利**: 独立线程+无锁SPSC队列, ~50ns tick推送, 配置开关useAsyncArbThread(实盘true/回测false), 异常兜底保线程存活
11. **分层配置(单一权威)**: 每个开关/参数只有唯一位置 — config.yaml=身份+业务参数(无开关), coordinator.yaml=模块开关+模块参数, spread_arbitrage.yaml=套利子系统, hotparams.yaml=热更新参数。代码内 fallback 仅用于"键缺失时使用编译期默认", 不构成第二处配置。
12. **O(1)自成交检查**: MM 订单快照预计算 min_sell/max_buy 标量, executeSignal 自检从 O(n) 线性扫描降为 O(1) 比较

## 优化历程 (ROADMAP V2)

| Phase | 内容 | 状态 |
|-------|------|------|
| Phase 0 | 基础设施 (12项 P0) | ✅ 完成 |
| Phase 1 | 架构重构 (triple-state-machine, 3-path quotes) | ✅ 完成 |
| Phase 2 | 状态机统一 (tryResumeFrom, ERROR 修复, 线程契约) | ✅ 完成 |
| Phase 3 | 代码质量 (on_tick 拆分, FIX 标记审计) | ✅ 完成 |
| Phase 4 | 信号系统改造 + 评估指标修正 | ✅ 完成 |
| **Phase 5** | **深度分析 v5: 48 项 Bug 修复 + 性能优化 + 架构重构** | **✅ 完成** |
| **Phase 6** | **深度分析 v6: 37 项诊断, 15 项 P0/P1/P2 修复** | **✅ 完成** |
| **Phase 7** | **深度分析 v7: 44 项诊断, 21 项真实修复 + 5 项误判避免** | **✅ 完成** |

### Phase 5 关键改造 (详见 docs/DEEP_ANALYSIS_V5.md)

**正确性修复 (35 Bug, P0 全清零)**:
- 时间戳 4 种单位混用统一为 epoch-ms (closeout 重试/套利冷却/毒性校准 全链路)
- halt 后自动恢复路径接线 (checkAndRecover 从死代码复活)
- 日亏损跨日累计修复 (resetDailyPnl)
- 部分成交 original_qty 跟踪 (不再提前 untrack)
- 反手成交 PnL 四情形正确计算
- 套利仓位回填 (updatePosition 从死代码复活) + 残腿防护机制
- 异步套利线程恢复 (5 处跨线程 data race 修复)
- 风控频率滑窗读侧剔除 + 恢复上限修复

**性能优化 (12 项)**:
- RollingScaleTracker 节流 + nth_element (每 tick 省 4 次 500 元素 sort)
- Momentum O(1) 增量 log 收益 (替代每 tick 127 次 std::log)
- TickContext 组件指针预解析 (每 tick 省 ~7 次字符串哈希查找)
- checkRiskLimits out-param 零堆分配
- updateMMOrders 世代号门控 (订单集未变时跳过快照深拷贝)
- 每 tick 墙钟单次读取 + LockFreeQueue cache line 对齐
- CMake 去 -ffast-math (NaN 风控静默失效隐患)

**架构重构 (3 方案)**:
- ISpreadStrategy 公共基类 + 注册表 (新增策略 7 处改动→1 行注册)
- SignalSlot 表驱动 (新增信号源 ~8 处改动→一段注册)
- 上帝类拆分: UftFutuMmStrategy 2924→2112 行, 拆出 ConfigLoader/HotParamManager/CloseoutOrchestrator/ArbExecutionBridge
- yaml 模块化统一 (coordinator.yaml 根级开关解析修复)

### Phase 6 关键改造 (详见 docs/DEEP_ANALYSIS_V6.md + V6_REVIEW.md)

**资金安全/数据正确性 (P0, 4 项)**:
- `flatten_threshold` 默认 3->2 (FLATTEN_POSITION 从不可达变可达)
- `getPositionReductionToLimit` int32_t->double 截断修复
- `timestampToMs` 删除 (fill_time 已是 epoch ms)
- TrendFollowing `entry_price` 赋值 (止损从死代码复活)

**逻辑正确性 (P1, 9 项)**:
- Welford->EWMA 衰减方差 (SpreadCalculator 适应 regime 切换)
- orphan 队列满兜底 (onArbSignalDropped)
- PnL 快照 atomic<double> (arb 线程 data race 修复)
- close 信号 in_flight 释放 / 自成交检查全扫描
- 自适应权重 tanh 归一化 / suppress 衰减修正
- SignalAggregator reset() 补全 / delta-rate 恢复路径统一

**清理 (P2, 2 项)**: hedge map 超时清理 / pushOverwrite 死代码删除

### Phase 7 关键改造 (详见 docs/DEEP_ANALYSIS_V7.md)

**资金安全/数学正确性 (P0, 10 项真实修复 + 1 项误报)**:
- STP 过滤 pending_cancel (checkSelfTrade + getConflictingMMOrders) -- 消除高频刷新 ARB 误拒
- `getActiveCountBySource` 过滤 pending_cancel -- 防 closeout inflight guard 卡死
- 撤单 ack 超时强制 untrack (5s) -- 防状态永久残留
- closeout 期间暂停 ARB (isCloseoutTriggered 门控)
- channel_lost 停 arb 线程 (新增 `setEnabled`)
- arb 线程 try/catch 兜底 (异常禁用套利保线程存活)
- `net_exposure` 符号错误修复 (`-` -> `+`, 全对冲价差从虚高 2 倍修正为 ≈0)
- `resetDailyPnl` 重置 avg_cost=0 (触发 pre_close 重设, 修复隔夜浮盈重复计入)
- Sharpe 年化因子修正 (per-trade 用 `sqrt(250)` -> `sqrt(250×日均笔数)`)
- inventory_pnl 填充 (recordTrade 按合约累计真实成交 PnL)
- ~~on_entrust HALT 期间误增 error_count~~ -- **误报** (已有 RISK_HALTED 早退守卫)

**代码质量/性能 (P1, 8 项真实修复 + 3 项误判避免)**:
- pending_adverse 内存泄漏 (30s 墙钟超时 + remove_if 全清理)
- cancelByPair 防御性补撤死代码 (`orders_it==end` -> `if(!sent)`)
- cancelOrder 补 pending_cancel 标记 (一致性)
- beta 截断 [0.7,1.5] 硬编码 -> config 可配 + CorrelationManager 按 expectedBeta 设带宽
- on_trade/channel_ready 漏清 `_risk_spread_mult` (新增 `onExternalResumeFromRisk()`)
- checkAutoCancel 改成员缓冲 (消除每 tick 3 次堆分配)
- getPairsForContract 返回 const ref (消除 6 处 vector 按值拷贝)
- 5 个 spinlock `alignas(64)` + 64B 填充 (消除 false sharing)
- ~~VaR 缺乘数~~ -- **误判** (WEIGHTED 模式 spread_std 已含乘数)
- ~~MarketMakingEnhancer 死代码~~ -- **有意观测模式** (推迟到 C2 阶段)
- ~~TickContext.code 堆分配~~ -- **高估** (合约码 14 字符命中 SSO)

**性能优化 (P2, 3 项)**:
- correlation+beta 合并扫描 (`computeCorrelationAndBeta` 单次 log-return, 消除重复 std::log)
- generateSignal spinlock 合并 (4->2 次/pair)
- executeSignal 自检改预计算标量 (O(n) 扫描 -> O(1), `_mm_buy/sell_orders` vector -> `min_sell/max_buy` 标量)

**避免的有害改动**: B7(误报)、B16(误判)、C1(有意设计)、P1(SSO覆盖)、A1-atomic(当前设计正确)

### Phase 8 (v7.2) 报价黏性 + scout 多层结构 (2026-07-31, 回测验证通过)

**背景**: 复核发现两个机制在 `alwaysObligation=true + numLevels=1` 下"空转"——
黏性(checkStickyUpdate)仅路径 A/B2 调用, 义务路径 B1 每 tick 无脑全撤重挂;
价格保护仅约束非义务单, 全义务配置下从不生效。

**Stage 1 — B1 条件式重挂 + 硬约束防穿**:
- B1 (handleObligationQuote) 双侧独立条件式: need = 无单 || 深度<min_valid_qty || 价格超黏性阈值;
  仅 need 侧撤+重挂, 双侧都不 need 零信息流 (回测实测 STICKY skip 274k 次/5日)
- 新增硬约束防穿盘 (所有层含义务, 不受 priceProtection 开关影响):
  bid ≤ best_ask-tick, ask ≥ best_bid+tick — 修复义务单完全豁免保护、skew 穿 mid 时可穿盘/自成交的缺陷
- 回测成交后处理对齐生产语义: 条件式撤单 (仅义务深度破坏才 cancelAll, 仅 stra_cancel
  避开回调内 stra_buy/sell 的 _orders 迭代风险), 深度满足则保留 (黏性受益)

**Stage 2 — scout 探测层架构** (自由层价格优于义务层):
- L0=自由探测层(scout): 最优价(l0_bid/l0_ask 直用), 小 qty(scoutQty=1), B2 路径
- L1=义务层: 次优价(l0∓levelStep), baseQty 大单(档位衰减不适用), B1 路径
- scout 成交 = 逆向信号 → `onScoutFillCancelObligation` 立即撤同侧义务层 →
  跳过通用重挂(不以旧价立即重挂义务单, 规避逆向成交) → 下一 tick 按新价重挂
- 新配置: `obligationLevel`(义务层档位, 默认0向后兼容), `scoutQty`(探测手数, 默认1.0);
  校验: obligationLevel<numLevels(报错), levelStep>0(报错, 堵价格阶梯倒挂入口), scoutQty≤baseQty(警告)
- 关键时序: UftMocker on_trade 先于 on_order, scout 成交时订单条目尚在, 识别可靠

**复核结论 (Q&A)**:
- **Q1 缺陷(已修)**: force 义务(util≥1.0)时旧 needObligation 对全层返回 true →
  scout 层被拖入 B1, qty 抬到 baseQty 且加仓侧不被 band 钳制 → 打满时 L0 挂 5 手
  最优价激进加仓单(逆向敞口)。修复: 非义务层永不转义务 + 自由分支 force 期间 qty 置零
  (force 时只留义务层, 恢复 v7.2 前语义)。本数据集 force 触发 0 次, 属生产防御
- **Q2 配置矩阵**: numLevels=1 → 仅 obligationLevel=0, 无自由层;
  numLevels=2 + obligationLevel=1 → scout 结构; numLevels=2 + obligationLevel=0 →
  经典"L0义务+L1外层自由单"(外层不享 scoutQty/不触发撤义务, 走 qty衰减+软保护)
- **Q3 l0价归属**: SpreadOptimizer 的 l0_bid/l0_ask(含alpha/skew/spread)报给 **scout 层(L0)**,
  义务层 = l0∓1tick。后果: 义务层较 v7.2 前被动 1 tick, skewCrossMid 激进减仓由
  1手 scout 先行; 减仓不足应调 skewCrossMaxTicks/checkTakerReduce, 而非把义务层拉回 L0
- **Q4 sub-tick 义务空窗评估** (实测): SCOUT 触发 14,800 次 / ~17.6万 tick(5日,500ms/tick)
  → 8.4% 的 tick 出现单侧义务空窗, 单侧空窗占比 ≈4.2%, 单次 ≤1 tick。
  对常规在场率考核(70-90%)可接受; 趋势行情有聚集效应(scout 连续成交 → 义务层连续缺席,
  正是设计意图的逆向避让)。可选改进: 生产端撤义务后立即外移1tick重挂 / scout触发冷却 /
  仅 scout 全成才撤义务层

**回测验证** (_ec_5d.yaml, exit=0): Config validation 0错0警(含新校验), 0 segfault,
0 穿盘报单, SCOUT 14,800 次(日志确认 1手scout成交→撤义务层lvl1),
报单分布 义务5手×17.9k / scout1手×15.4k, 净收益 ¥442,561 (arb ON + srand 噪声, 不与基线直接比)

## 待定项

### Trade-through 毒性检测 (暂缓)

**状态**: 评估完成, 暂缓实现

**设计**: 作为 ISignalSource 实现, 基于 tick 快照 volume 增量检测脉冲毒性 (连续同方向大单).

**价值**: 补充 VPIN 的持续性毒性检测, 提供脉冲毒性 + 方向性检测.

**暂缓原因**:
- 国内期货只有行情切片 (tick snapshot), 无逐笔成交
- volume 增量方向推断有 bid-ask bounce 问题 (TradeFlow IC=-0.83 的同根因)
- tick 快照无法精确区分"大单扫盘" vs "多笔小单累积"
- 当前 VPIN toxicity 实际影响仅 1% 交易时间, 改善空间有限

**重启条件**: 接入有逐笔成交数据的市场 (如加密货币), 或解决 tick 快照方向推断准确性.

### P1-8 跨期同步报价组 (部分覆盖)

Phase 5 的 LeadLag + 权重框架已部分覆盖跨期协调。独立 sync_group 实现需要改 Quoter 架构, 留作后续.

## 深度分析报告

完整的逐行源码审查、Bug 清单与修复记录详见:

- **[docs/DEEP_ANALYSIS_V7.md](docs/DEEP_ANALYSIS_V7.md)** - v7 深度分析 (44 项诊断: 架构/业务逻辑/代码质量/性能四维; 21 项真实修复 + 5 项误判避免; 含复核结论表与分阶段重构方案)
- **[docs/DEEP_ANALYSIS_V6.md](docs/DEEP_ANALYSIS_V6.md)** - v6 深度分析 (37 项诊断 + 修复方案)
- **[docs/DEEP_ANALYSIS_V6_REVIEW.md](docs/DEEP_ANALYSIS_V6_REVIEW.md)** - v6 复核报告 (2 误报 + 3 降级 + 35 确认)
- **[docs/DEEP_ANALYSIS_V5.md](docs/DEEP_ANALYSIS_V5.md)** — v5 深度分析 (35 Bug + 48 Fix + 3 架构方案 + 复核修正)
- **[docs/ARB_SELF_CLOSE_DESIGN.md](docs/ARB_SELF_CLOSE_DESIGN.md)** — 套利分级平仓设计方案 v2.0 (分级执行: CLOSE 保持 B-3 / STOP_LOSS taker 立即 / TIMEOUT maker 挂单; 含成本模型 fee+spread+slippage; Phase A1-A10 + Phase B/C/D; 状态: 已确认,待实施)
- **[OPTIMIZATION_REPORT.md](OPTIMIZATION_REPORT.md)** — v4 优化报告 (19 项已修)
