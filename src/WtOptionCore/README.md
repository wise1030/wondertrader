# WtOptionCore - WonderTrader 期权做市引擎

## 概述

WtOptionCore 是 WonderTrader 量化交易平台下的**期权做市引擎**，从 quantbox/optiontrader 项目迁移而来。作为一个 C++17 共享库 (`libWtOptionCore.so`)，它被 WtRunner 以 HFT 策略工厂插件方式动态加载，实现了完整的期权做市管线：合约数据网格、Black-Scholes/Black76 定价、GVV 波动率曲面、组合风险/Greeks 聚合、订单管理、PnL 跟踪，以及一套套利/做市扫描器。

支持三种期权业务场景：
- **商品期货期权** (SHFE/DCE/INE)：定价标的=期货合约，对冲标的=同一期货合约
- **郑州商品期权** (CZCE)：C/P 双品种自动发现 (如 SRC+SRP)
- **股指期权** (CFFEX IO/MO/HO)：定价标的=现货指数（不可交易），对冲标的=股指期货（不同月份分别对冲）

## 架构

```
Market Tick (CTP/XQP)
    │
    ▼
OptionAsyncEventProcessor (异步事件队列 + worker线程)
    │  事件按优先级桶排序: Session > Channel > Position > Trade > Order > Timer > Tick
    │  Tick 去重 (unordered_map<const char*>)
    │  on_batch_complete 始终执行 (即使无 tick, 也能处理 trade/position 事件)
    │
    ▼
OptionGrid.onTick() -> 合约自动发现 -> 市场快照更新
    │
    ▼ (on_batch_complete, 防抖 debounce)
CompositeOptionPricer.computeValues() -> FAST/SLOW path
    │                                    ├── initValuesCompute (forward + maturity + risk greeks)
    │                                    ├── BlackCalc / Black76 / BlackScholes (定价)
    │                                    ├── GvvVolCurve / LinearVolCurve (波动率曲面)
    │                                    ├── computeOurMarkets -> desired markets
    │                                    └── OptionGrid.__notifyComputeCompleted
    │                                         └── CTG listener -> refresh() (自动)
    │
    ▼
ControllableTradingGrid.refresh() -> check_markets -> rank -> TPS限流
    │  排序: isBest(+5/+1) + crossing(+10) + delta + spread + expiry + type权重
    │  TPS: 正常限流 + Panic增强 + Drop重试 (retainedDrops 保留)
    │
    ▼
OptionQuoteManager.updateOrders() -> stra_quote / stra_buy / stra_sell
    │  ├── RiskFilterChain (MaxOrderSize/MinSellPrice/MaxPosition/MaxCancel/MaxNewOrders)
    │  ├── PositionGuard 检查
    │  └── 增量 diffing + 撤单节流
    │
    ▼
OptionRisk -> 组合Greeks聚合 -> 风控限制 -> panic/widen
    │
    ▼
PnlTracker -> mark-to-mid PnL -> PnlLimitSignal -> Auto-panic
```

## 核心组件

### 引擎层
| 组件 | 文件 | 职责 |
|------|------|------|
| HftOptionStrategy | HftOptionStrategy.h/.cpp | HFT 策略入口，协调所有组件 |
| HftOptionStraFact | HftOptionStraFact.cpp | DLL 工厂，供 WtRunner 动态加载 |
| OptionAsyncEventProcessor | OptionAsyncEventProcessor.h/.cpp | 异步事件队列 + worker 线程，桶排序 + tick 去重 + 异常保护 |
| WtOptEngine | WtOptEngine.h/.cpp | 期权交易引擎 (legacy 路径) |
| WtOptContext | WtOptContext.h/.cpp | 策略上下文 (legacy 路径) |

### 数据网格 (3级: Expiry -> Strike -> Call/Put)
| 组件 | 文件 | 职责 |
|------|------|------|
| OptionGrid | OptionGrid.h/.cpp | 3级期权数据网格，tick 驱动合约发现 |
| OptionData | OptionData.h/.cpp | 单合约数据: 10档行情 + 计算值 + 监听器 |
| ExpiryData | ExpiryData.h/.cpp | 到期月数据: 日历、forward、maturity、利率 |
| StrikeData | StrikeData.h/.cpp | 行权价层: call/put 对 |

### 定价层
| 组件 | 文件 | 职责 |
|------|------|------|
| CompositeOptionPricer | CompositeOptionPricer.h/.cpp | 做市定价器 (FAST/SLOW, alpha/risk调整) |
| OptionPricer2 | OptionPricer2.h/.cpp | Black76/GVV 定价器 (支持 OpenMP 并行) |
| BlackCalc | BlackCalc.h/.cpp | 统一 BS/Black76 计算 |
| GvvVolCurve | GvvVolCurve.h/.cpp | GVV 波动率曲面 (WLS3 替代 GSL) |
| PeriodicCurveFitter | PeriodicCurveFitter.h/.cpp | 定时波动率曲面拟合 (period 间隔控制) |

### 风险层
| 组件 | 文件 | 职责 |
|------|------|------|
| OptionRisk | OptionRisk.h/.cpp | 组合 Greeks 聚合 + 对冲 delta |
| OptionRiskData | OptionRiskData.h/.cpp | 单合约仓位 Greeks (dirty flag 增量更新) |
| OptionExpiryGreeks | OptionExpiryGreeks.h/.cpp | 到期月 Greeks 聚合 |

### 执行层
| 组件 | 文件 | 职责 |
|------|------|------|
| ControllableTradingGrid | ControllableTradingGrid.h/.cpp | 执行调度: 排序 + TPS限流 + Panic模式 + Drop重试 |
| OptionQuoteManager | OptionQuoteManager.h/.cpp | 单合约订单生命周期 + STP + late fill + 增量diffing |
| check_markets | check_markets.h/.cpp | desired vs current 市场比较 |

### 信号层
| 组件 | 文件 | 职责 |
|------|------|------|
| ToxicitySignal | Signals/RiskSignals.h/.cpp | 毒性流检测 (adverse fills + 自动 widen/panic) |
| PnlLimitSignal | Signals/RiskSignals.h/.cpp | 日内 PnL 限制 (超限 -> auto panic) |
| VegaFlowSignal | Signals/AlphaSignals.h/.cpp | Vega 流信号 |
| DeltaFlowSignal | Signals/AlphaSignals.h/.cpp | Delta 流信号 |
| ForwardSpreadSignal | Signals/AlphaSignals.h/.cpp | Forward spread EMA 信号 |

### 风控增强模块
| 组件 | 文件 | 职责 |
|------|------|------|
| RiskFilterChain | RiskFilterChain.h/.cpp | 可组合订单过滤器链 (5个过滤器, 配置驱动) |
| PositionOffsetMgr | PositionOffsetMgr.h/.cpp | 策略侧可平量跟踪 (平今/平昨/开仓) |
| PositionGuard | PositionGuard.h/.cpp | 持仓不一致检测 (安全护栏) |
| FillPriceChecker | FillPriceChecker.h/.cpp | 成交价偏离监控 (Warning/Panic) |
| RiskLimitsEx | RiskLimitsEx.h/.cpp | 扩展预交易风控检查 |

### 辅助组件
| 组件 | 文件 | 职责 |
|------|------|------|
| PnlTracker | PnlTracker.h/.cpp | 单合约 tick-by-tick mark-to-mid PnL |
| AttributePublisher | AttributePublisher.h/.cpp | 结构化属性发布 (WTSLogger) |
| QuoteStatistics | QuoteStatistics.h/.cpp | 报单统计 (回调驱动, 借鉴 BilateralQuoteStats) |
| ExpirationSimulator | ExpirationSimulator.h/.cpp | 到期 PnL 模拟 |
| OptionValueWriter | OptionValueWriter.h/.cpp | 定时 CSV 记录理论值/Greeks |
| Predictor | Predictor.h | 预测器基础设施 (IPredictor + TriggerEngine) |

## 线程模型

```
TraderAdapter 回调线程          ShareManager 监控线程        Async Worker 线程
    │                               │                          │
    ├─ on_tick (enqueue)            ├─ on_params_updated       ├─ 事件桶排序
    ├─ on_trade (enqueue)           │  (读写 atomic params)     ├─ Session/Channel/Position
    ├─ on_order (enqueue)                                      ├─ Trade/Order/Timer
    ├─ on_position (enqueue)                                   ├─ Tick 去重 + batch
    ├─ on_channel_ready/lost (enqueue)                        ├─ computeValues (防抖)
    └─ on_session_begin/end (enqueue)                          ├─ risk update + panic
                                                                ├─ scanner dispatch
                                                                ├─ CTG refresh + drain
                                                                └─ attribute publish
```

**线程安全保证**:
- `_positions` / `_pnlPendingInit`: 仅 worker 线程访问 (on_position 已改为 enqueue)
- `_traderCtx->enabled` / `panicked`: `std::atomic<bool>`
- `_channelReady` / `_initialized` / `_tickCount`: `std::atomic`
- `OptionGrid` 容器: `std::shared_mutex`
- `worker_loop`: `try/catch` 包裹，异常不崩溃

## 配置文件

完整配置参见 `config/option_strategy_full.json`。主要配置项:

| 配置项 | 类型 | 默认值 | 说明 |
|--------|------|--------|------|
| exchange | string | - | 交易所 (如 "SHFE", "CFFEX", "CZCE") |
| futuresProduct | string | - | 期货品种 (如 "ag", "IF", "SR") |
| optionProduct | string | 自动推导 | 期权品种 (不配则按交易所规则自动推导) |
| underlyingCode | string | - | 全局定价标的 (期货合约或指数) |
| underlyingType | string | "future" | 标的类型: "future" 或 "index" |
| riskFreeRate | double | 0.03 | 无风险利率 (flat) |
| riskFreeRateCurve | array | [] | 利率期限结构 [{days, rate}] |
| maxTPS | int | 50 | 每秒最大交易数 |
| minComputeInterval | double | 0.02 | 最小计算间隔 (秒) |
| use_parallel | bool | false | 启用 OpenMP 并行定价 |
| sessionSchedule | object | - | 盘中停止调度 |
| pricer | object | - | 定价器参数 (含 volCurve.fitter) |
| orderManager | object | - | OQM 参数 + riskFilters 风控过滤器 |
| expiries | array | - | 到期月配置 (含 per-expiry underlyingCode/hedgeCode) |
| riskLimits | object | - | 扩展风控参数 (RiskLimitsEx) |
| signals | object | - | Alpha + Risk 信号 |
| scanners | array | - | 扫描器列表 |
| optionValueWriter | object | - | 值记录器配置 |

### Vol Curve Fitter 配置 (`pricer.blackCalc.volCurve.fitter`)

控制波动率曲面拟合的时机和参数：

```json
"blackCalc": {
    "volCurve": {
        "fitter": {
            "start_time": 0,
            "end_time": 86400,
            "period": 60,
            "decay_window": 300,
            "threshold": 0,
            "fit_all_expiries": false,
            "good_points_thresh": [
                {"days": 0, "thresh": 3}
            ]
        }
    }
}
```

| 字段 | 默认值 | 说明 |
|------|--------|------|
| start_time | 0 | 拟合窗口起始 (秒, 0=全天开始, 支持夜盘) |
| end_time | 86400 | 拟合窗口结束 (秒, 86400=全天结束) |
| period | 60 | 拟合周期 (秒) |
| decay_window | 300 | 衰减窗口 (秒, 旧数据权重衰减) |
| threshold | 0 | 最低期权中间价阈值 (低于此价不参与拟合) |
| fit_all_expiries | false | 是否拟合所有到期月 (false=仅可拟合的) |
| good_points_thresh | [{days:0, thresh:3}] | 有效点数阈值 (按天数插值) |

### OrderManager 配置 (`orderManager`)

```json
"orderManager": {
    "riskFilters": {
        "option": { ... },
        "future": { ... }
    },
    "option": { ... },
    "future": { ... }
}
```

#### RiskFilterChain 配置 (`orderManager.riskFilters`)

按品种 (option/future) 独立配置 5 个可组合过滤器。`enabled=false` 时该品种过滤器不激活。

```json
"riskFilters": {
    "option": {
        "enabled": true,
        "max_order_size": 100,
        "max_order_size_reject": false,
        "min_sell_price": 0.0001,
        "max_position": 50,
        "max_position_mode": 0,
        "max_cancel_soft": 100,
        "max_cancel_hard": 200,
        "max_new_orders_hard_flat": 100,
        "max_new_orders_reject": 200
    },
    "future": {
        "enabled": true,
        "max_order_size": 100,
        "max_order_size_reject": false,
        "min_sell_price": 0.0001,
        "max_position": 10,
        "max_position_mode": 0,
        "max_cancel_soft": 100,
        "max_cancel_hard": 200,
        "max_new_orders_hard_flat": 100,
        "max_new_orders_reject": 200
    }
}
```

| 字段 | 默认值 | 说明 |
|------|--------|------|
| enabled | false | 是否启用该品种的过滤器链 |
| max_order_size | 100 | 单笔最大下单量 |
| max_order_size_reject | false | 超限时 true=拒绝, false=截断到限制 |
| min_sell_price | 0.0001 | 最低卖价 (防止零价或负价) |
| max_position | 0 | 最大持仓限制 (0=不启用此过滤器, 用 OQM 自身的 check_potential_position) |
| max_position_mode | 0 | 0=REJECT(拒绝), 1=ALLOW(允许), 2=MODIFY(截断到最大) |
| max_cancel_soft | 100 | 撤单软限制 (超过时告警但不阻止) |
| max_cancel_hard | 200 | 撤单硬限制 (超过时阻止撤单) |
| max_new_orders_hard_flat | 100 | 新单数达此限后进入硬平仓模式 |
| max_new_orders_reject | 200 | 新单数达此限后拒绝新单 |

**过滤器执行顺序**: MaxOrderSize -> MinSellPrice -> MaxPosition -> MaxCancel -> MaxNewOrders

#### OQM 参数 (`orderManager.option` / `orderManager.future`)

| 字段 | 默认值 | 说明 |
|------|--------|------|
| enable_quote_api | true | 使用双边报价 API (false=拆分为买单+卖单) |
| avoid_trade | true | 避免成交 (desired=current 时跳过) |
| check_potential_position | false | 检查潜在持仓 (OQM 自身的仓位限制) |
| leave_outer_orders | true | 保留外层订单 (不撤非最优价位) |
| hard_flat_after_n_fills | -1 | N 次成交后硬平仓 (-1=不启用) |
| reject_max_new_orders | -1 | 新单拒绝阈值 (-1=不启用) |
| cancel_timeout_ms | 3000 | 撤单超时 (毫秒) |
| max_orders_per_code | 10 | 每合约最大订单数 |
| stp_enabled | true | 自成交保护 (STP) |
| ttl_ms | 5000 | 订单存活时间 (毫秒, 超时自动撤单) |

### 到期月配置 (`expiries`)

每个到期月可独立配置定价标的和对冲标的：

```json
"expiries": [
    {
        "expiry": 202608,
        "enable": true,
        "underlyingCode": "SHFE.ag.2608",
        "hedgeCode": "SHFE.ag.2608",
        "secondaryHedgeCodes": [],
        "delta_min": 0.05,
        "delta_max": 0.95,
        "sprd_fwd": 0.01,
        "sprd_atmvol": 0.1,
        "sprd_corr": 0.0,
        "max_pos_fut": 10,
        "max_pos_stk": 50,
        "max_pos_opt": 50,
        "max_qsize": 5,
        "enable_auto_close": false,
        "close_pos_thresh": 0
    }
]
```

| 字段 | 说明 |
|------|------|
| expiry | 到期月 YYYYMM |
| enable | 是否启用该到期月 |
| underlyingCode | 该到期月的定价标的 (空=用全局 underlyingCode) |
| hedgeCode | 该到期月的对冲标的 (空=同 underlyingCode) |
| secondaryHedgeCodes | 次要对冲标的列表 |
| delta_min/max | delta 报价范围 (QM_AUTO 模式下生效) |
| sprd_fwd | forward 价差参数 |
| sprd_atmvol | ATM vol 价差参数 |
| sprd_corr | 相关系数 |
| max_pos_fut/stk/opt | 期货/行权价/期权持仓限制 |
| max_qsize | 最大报价手数 |
| enable_auto_close | 是否启用自动平仓 |
| close_pos_thresh | 平仓阈值 |

### Hot-param 运行时控制

通过 ShareManager 共享内存热更新:

| 参数 | 类型 | 说明 |
|------|------|------|
| wgt_vegaflow | double | Vega 流权重 |
| wgt_frontfut_skew | double | 前月偏度权重 |
| wgt_deltaflow | double | Delta 流权重 |
| wgt_atmsig | double | ATM 信号权重 |
| wgt_rollema | double | 展仓 EMA 权重 |
| sticky_base | double | 粘性基准 |
| improve_retreat_ratio | double | 改进/撤退比 |
| max_tps | int32 | 最大 TPS |
| command | int32 | 0=正常, 1=停止, 2=恐慌, 3=恢复 |
| qmode_override | int32 | 0=无, 1=ON, -1=OFF, 2=CLOSE |
| manual_order | string | 手动下单: "B,code,price,qty" / "S,..." / "C,code" |

### 期权品种自动推导

根据交易所和期货品种自动推导期权品种，无需手动配置 `optionProduct`：

| 交易所 | 期货品种 | 期权品种 | 规则 |
|--------|---------|---------|------|
| SHFE/INE/DCE | ag, cu, m | ag_o, cu_o, m_o | 期货品种 + "_o" |
| CZCE | SR | SRC, SRP | 期货品种 + "C"/"P" (双品种发现) |
| CFFEX | IF, IC, IH | IO, MO, HO | 固定映射表 |

### 三种业务场景配置示例

**商品期权 (SHFE.ag, 1:1)**:
```json
{
    "exchange": "SHFE",
    "futuresProduct": "ag",
    "underlyingCode": "SHFE.ag.2608",
    "underlyingType": "future",
    "expiries": [{"expiry": 202608, "enable": true}]
}
```

**商品期权 (CZCE.SR, 多系列)**:
```json
{
    "exchange": "CZCE",
    "futuresProduct": "SR",
    "underlyingCode": "",
    "underlyingType": "future",
    "expiries": [
        {"expiry": 202609, "underlyingCode": "CZCE.SR309", "hedgeCode": "CZCE.SR309"},
        {"expiry": 202701, "underlyingCode": "CZCE.SR701", "hedgeCode": "CZCE.SR701"}
    ]
}
```

**股指期权 (CFFEX.IO, 指数定价+期货对冲)**:
```json
{
    "exchange": "CFFEX",
    "futuresProduct": "IF",
    "underlyingCode": "CFFEX.000300",
    "underlyingType": "index",
    "expiries": [
        {"expiry": 202607, "hedgeCode": "CFFEX.IF2607"},
        {"expiry": 202608, "hedgeCode": "CFFEX.IF2608"}
    ]
}
```

## 事件处理流程

### Worker 线程批处理顺序

每个 batch 内事件按固定顺序处理：

```
1. on_session (session begin/end)
2. on_channel (channel ready/lost)
3. on_position (broker position update)
4. on_trade (fill)
5. on_order (order status)
6. on_timer (triggerDoFit + session scheduling + timeout recompute)
7. on_tick_batch (set pricer time)
   per-tick: on_tick (market snapshot update)
8. on_batch_complete (always runs, even without ticks)
   ├─ position sync (OptionData + OQM setPosition)
   ├─ PnlTracker update
   ├─ computeValues (debounced: _underlyingChanged || _needsRefresh)
   │   ├─ OptionRisk::update() (refresh position greeks)
   │   ├─ initValuesCompute (forward + maturity)
   │   ├─ computeValue per strike (BlackCalc)
   │   ├─ computeOurMarkets per option (desired bid/ask)
   │   └─ __notifyComputeCompleted -> CTG::refresh() (auto via listener)
   ├─ drainPendingQuotes (send orders)
   └─ attribute publish
```

### 关键设计决策

1. **on_batch_complete 始终执行** - 即使无 tick 也能处理 trade/position/order 事件
2. **eager computeValues 已移除** - 不在 per-tick 中计算，统一由 on_batch_complete 的 debounce 控制
3. **onFitCompleted 经 OptionGrid::computeValues** - 确保 listener refresh 自动触发，ourMarket 被收集
4. **tradingStopMidDay 无条件 drain** - CANCEL quotes 立即发送，不受 enabled 门控
5. **dropped-quote 重试** - TPS 限流丢弃的报价保留到下一周期，不被 refresh clear 销毁
6. **channel_ready 延迟启用** - 无标的价格时不启用交易，首个 underlying tick 到达时启用
7. **_needsRefresh 标志** - on_trade/on_position/on_order 标记后，下一 on_batch_complete 强制重新计算

## 标的/对冲模型

### 设计原则
- **定价标的必须是该系列期权对应的标的**，不能用 HOT 或其他月份替代
- **股指期权的定价标的是指数**（不可交易），对冲只能用股指期货
- **不同到期月的股指期权对应不同月份的股指期货对冲**

### Per-expiry 标的价格路由
```
on_tick(code):
    if code in m_expiryUnderlyingMap:
        -> 更新对应 ExpiryData 的 per-expiry 标的价
        -> 更新全局标的价格
        -> _underlyingChanged = true
    elif code == m_underlyingCode (全局标的, 如指数):
        -> 更新全局标的价格
        -> _underlyingChanged = true
    elif code is option:
        -> 更新期权市场快照
```

### Forward 计算 (Put-Call Parity 优先)

1. **Put-call parity 合成** (PRIMARY) - 需要 >=5 个 strike 的 call bid/ask + put bid/ask 全部 > 0
2. 不足时 `forwardReady=false`，定价跳过，不回退到标的价格
3. `ema_sprd_vs_atmfwd` 在合成 forward 时按 strike 更新 spread (而非绝对值)
4. `ForwardSpreadSignal` 基于 forward spread - EMA(forward spread) 生成 alpha 调整

### 防崩保护 (P0)
- `GvvVolCurve::eval()`: maturity<=0 时返回 1.0，防止除零
- `GvvVolCurve::fitWithAlpha()`: maturity<=0 时用 1.0 替代
- `ExpiryData::getMaturity()`: 最小值 1/252（1个交易日）
- `ExpiryData::updateDaysToExpiration()`: bdays=0 且 calendarDays>0 时 fallback 为 1
- `OptionPricer2::computeImpliedValues()`: forward/maturity/discount 为 NAN 或 <=0 时跳过 IV 计算
- `PeriodicCurveFitter::doFit()`: 无 forward-ready expiry 时返回 false
- `PeriodicCurveFitter::fitToExpiry()`: forward not ready / 无 vol points / 不足 upside+downside 时返回 false
- `refreshExpiryDays()`: 每个 session begin 刷新所有 expiry 的 days-to-expiration (防止冻结)

### Maturity 每日刷新

`ExpiryData::updateDaysToExpiration()` 仅在合约创建时调用。`OptionGrid::refreshExpiryDays()` 在每个 session begin 时为所有 expiry 重新计算，防止近月 maturity 冻结导致定价偏差。

## 构建与部署

### 构建
```bash
cd /path/to/wondertrader/src
mkdir -p build && cd build
cmake -G "Unix Makefiles" ..
make WtOptionCore -j4
```

### 部署
```bash
cp build/WtOptionCore/libWtOptionCore.so /path/to/run/strategies/
cp src/WtOptionCore/config/option_strategy_full.json /path/to/run/config/option_strategy.json
```

### 单元测试
```bash
cd build
make test_ctg_ranking test_enhancements
./WtOptionCore/test_ctg_ranking      # CTG 排序 + 风控测试
./WtOptionCore/test_enhancements    # 增强模块测试 (过滤器/持仓/偏离/风控/OQM)
```

## 迁移历史

从 quantbox/optiontrader (基于 longbeach 框架) 迁移到 WonderTrader (HFT 框架):

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
| 诊断 | std::cout | WTSLogger (结构化日志) |

## 文件清单

```
src/WtOptionCore/
├── CMakeLists.txt
├── HftOptionStrategy.h/.cpp          # HFT 策略入口
├── HftOptionStraFact.cpp              # DLL 工厂
├── OptionAsyncEventProcessor.h/.cpp  # 异步事件处理器 (lock-free SPSC队列)
├── OptionGrid.h/.cpp                  # 3级数据网格 + per-expiry标的路由
├── OptionData.h/.cpp                  # 单合约数据
├── ExpiryData.h/.cpp                 # 到期月数据 (per-expiry标的价 + maturity防护)
├── StrikeData.h/.cpp                 # 行权价数据
├── CompositeOptionPricer.h/.cpp      # 做市定价器 (FAST/SLOW + onFitCompleted)
├── OptionPricer.h/.cpp               # 理论定价器 v1
├── OptionPricer2.h/.cpp              # Black76/GVV 定价器
├── BlackCalc.h/.cpp                  # BS/Black76 计算
├── BlackImpliedCalculator.h/.cpp     # 隐含波动率求解
├── GvvVolCurve.h/.cpp                # GVV 波动率曲面 (maturity除零保护)
├── LinearVolCurve.h/.cpp            # 线性波动率曲面
├── PeriodicCurveFitter.h/.cpp       # 定时曲面拟合 (period间隔 + WTSLogger诊断)
├── VolCurve.h/.cpp                   # 波动率曲面基类 + GVV实现
├── OptionRisk.h/.cpp                 # 组合风险
├── OptionRiskData.h/.cpp             # 单合约仓位 Greeks
├── OptionExpiryGreeks.h/.cpp        # 到期月 Greeks
├── ControllableTradingGrid.h/.cpp    # 执行调度 (Drop重试 + 无条件drain)
├── OptionQuoteManager.h/.cpp        # 订单管理 (增量diffing + 撤单节流 + 谨慎翻面)
├── check_markets.h/.cpp              # 市场比较
├── PnlTracker.h/.cpp                 # PnL 跟踪
├── QuoteStatistics.h/.cpp            # 报单统计 (回调驱动, 借鉴BilateralQuoteStats)
├── AttributePublisher.h/.cpp         # 属性发布
├── ExpirationSimulator.h/.cpp       # 到期模拟
├── OptionValueWriter.h/.cpp         # 值记录器
├── Predictor.h                       # 预测器
├── RiskFilterChain.h/.cpp            # 可组合订单过滤器链 (5个过滤器, 配置驱动)
├── PositionOffsetMgr.h/.cpp          # 策略侧可平量跟踪 (平今/平昨/开仓)
├── PositionGuard.h/.cpp              # 持仓不一致检测 (安全护栏)
├── FillPriceChecker.h/.cpp           # 成交价偏离监控 (Warning/Panic)
├── RiskLimitsEx.h/.cpp               # 扩展预交易风控检查
├── ComboOrders.h/.cpp                # 多腿组合订单 (Spread/SynFuture)
├── OptionOrder.h/.cpp               # 期权订单 (ValuesAtIssue + PnL)
├── BaseOrder.h/.cpp                  # 订单基类 (状态机)
├── OptionOrderInfo.h/.cpp           # 期权订单快照
├── FutureOrderInfo.h/.cpp           # 期货订单快照
├── ScannerInfo.h                      # 扫描器信息
├── WtOptEngine.h/.cpp                # 引擎 (legacy)
├── WtOptContext.h/.cpp               # 上下文 (legacy)
├── WtOptTicker.h/.cpp                # 定时器
├── Scanners/                         # 扫描器 (10种)
├── Signals/                          # 信号 (Alpha + Risk, 含 ForwardSpreadSignal)
├── config/                           # 配置文件
└── tests/                            # 单元测试 (test_ctg_ranking + test_enhancements)
```

## 增强模块详情

### RiskFilterChain - 可组合订单过滤器链 (配置驱动)

通过 `orderManager.riskFilters.{option,future}` 配置，按品种独立激活：

- **MaxOrderSizeFilter**: 单笔最大下单量，超限时 reject 或 modify
- **MinSellPriceFilter**: 最低卖价保护，防止零价或负价
- **MaxPositionFilter**: 持仓限制，3 种模式 (REJECT / ALLOW / MODIFY_TO_MAX)
- **MaxCancelFilter**: 撤单限制，软限制告警 + 硬限制阻止
- **MaxNewOrdersFilter**: 新单数限制，达 hard_flat 后进入硬平仓，达 reject 后拒绝

**执行顺序**: MaxOrderSize -> MinSellPrice -> MaxPosition -> MaxCancel -> MaxNewOrders
**短路**: 遇 REJECT 立即返回，后续过滤器不执行

### PositionOffsetMgr - 策略侧可平量跟踪
- 从 WT `on_position` 回调提取 today/prev 可平量
- 跟踪挂单冻结量（平仓单挂出但未成交时冻结可平量）
- `getOrderBreakdown(isBuy, qty)` 将订单拆分为 (平今, 平昨, 开仓) 三段
- 持仓不一致检测（内部 fill 累计 vs broker 回调）

### PositionGuard - 持仓不一致检测
- 内部 fill 累计 vs broker `on_position` 回调对比
- 偏差超阈值 -> 禁用交易 + 告警
- 支持冷却时间防止告警刷屏
- `reconcile()` 手动解决不一致

### FillPriceChecker - 成交价偏离监控
- 记录发单价，成交时检查偏离百分比
- Warning (0.25%) -> 告警回调
- Panic (0.5%) -> 可触发 Panic 模式
- 与 ToxicitySignal 互补（价格维度 vs 方向维度）

### RiskLimitsEx - 扩展预交易风控检查
- 单笔手数/金额限制
- 价格偏离参考价检查 (clearly_erroneous)
- 最低卖价保护
- Greeks 限制检查 (Delta/Gamma/Vega/DailyLoss)
- 事后持仓复查

### OptionQuoteManager 增强 (借鉴 DefaultOrderManager)
- **增量订单 diffing**：`getMissingPriceLevelSize()` 按价位比较 desired vs current，避免全量撤单重发
- **撤单节流（三层）**：hard limit + buffer + soft warn
- **谨慎翻面**：`cautious_flipping` 阻止头寸翻面方向订单
- **Scale factor**：运行时动态缩放订单大小
- **最小更新间隔**：`min_intra_update_period_ms` 限制 updateOrders 频率
- **Wait-for-cancels**：撤单未确认时不发新单
- **PositionGuard 集成**：交易前检查持仓一致性
- **PositionOffsetMgr 集成**：平仓方向路由到 `stra_exit_long/short(isToday)`
- **拒绝重试**：全量拒绝后延迟重试，成交后重置计数器
- **OQM 仓位同步**：on_batch_complete 中从 broker 仓位同步 OQM m_position

### QuoteStatistics - 报单统计 (借鉴 BilateralQuoteStats)
- **回调驱动**：所有统计在 `onOrderStatusChange`/`onFill` 回调中完成，不影响热路径
- **双边有效报价时长**：基于 SessionInfo 的 timeToMinutes 计算，非交易时段不统计
- **做市义务判断**：min_valid_qty 深度 + max_obligation_spread 双重检查
- **状态切换计数**：记录进入/退出双边状态的次数
- **加权价差统计**：以 tick 为单位
- **报价延迟**：tick -> 订单确认的微秒级延迟
- **成交/撤单/拒绝率**：fills/cancels/rejects / ordersSent
- **会话级报告**：onSessionEnd 自动输出汇总日志
