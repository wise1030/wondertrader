# WtOptionCore — 期权做市系统

> 从 quantbox (longbeach 框架) 完整迁移至 WonderTrader UFT 框架。
> 12636 行 C++17，85 文件，零外部依赖 (GSL→WLS3, TBB→OpenMP, QuantLib→std C++)。

## 1. 架构概览

```
┌─────────────────────────────────────────────────────────┐
│                    UftOptionStrategy                     │
│            (UFT 入口, on_tick/on_init/callback)           │
├──────────────┬──────────────────────────┬───────────────┤
│  Async Layer │     Pricing Layer        │  Exec Layer   │
│              │                          │               │
│ OptionAsync  │  CompositeOptionPricer   │ Controllable  │
│ EventProc    │    ├── OptionPricer2     │ TradingGrid   │
│  (CTP线程    │    │   ├── BlackCalc     │  ├── check_   │
│   enqueue    │    │   ├── BlackImplied  │  │   markets  │
│   worker     │    │   └── GvvVolCurve   │  ├── drain    │
│   batch)     │    └── PeriodicCurve     │  │ PendingQ   │
│              │       Fitter             │  └── stra_    │
├──────────────┼──────────────────────────┤    quote      │
│  Data Layer  │     Risk Layer           │               │
│              │                          │               │
│  OptionGrid  │  OptionRisk              │  Scanners(9)  │
│  (expiry→    │  (Greeks 聚合+           │  (MM/Butterfly│
│   strike→    │   持仓跟踪)              │   /VolSpread  │
│   C/P)       │                          │   /Open/...)  │
└──────────────┴──────────────────────────┴───────────────┘
```

### 1.1 线程模型

| 线程 | 职责 | 延迟目标 |
|---|---|---|
| CTP md thread | on_tick → enqueue_tick | < 1μs |
| CTP td thread | on_trade/on_order → enqueue | < 1μs |
| Async worker | batch drain → grid.onTick → computeValues → CTG.refresh → drainPendingQuotes | < 5ms |
| WT timer | on_bar/on_session_begin/end (同步) | - |

CTP 线程只做 enqueue（~0.5μs），所有业务逻辑在 worker 线程执行。

### 1.2 数据流

```
tick到达 (CTP线程)
  │
  ├─ enqueue_tick(stdCode, WTSTickData*) → AsyncEvent{char[32], TickDataRef}
  │
  ▼
worker线程 (batch drain)
  │
  ├─ grid->onTick(TickDataRef)
  │   ├─ 标的: m_underlyingPrice = price
  │   └─ 期权: createOption (dynamic discovery)
  │       ├─ CodeHelper::isStdChnFutOptCode → extractStdChnFutOptCode
  │       ├─ ExpiryData (YYYYMM, expireDate, holidays, maturity)
  │       └─ StrikeData (strike, Call/Put OptionData)
  │
  ├─ grid->computeValues(pricer)
  │   └─ CompositeOptionPricer::computeValues
  │       ├─ initValuesCompute (EMA更新, risk shifts, atmforward)
  │       ├─ FAST path (每tick, 缓存vol → theo/greeks)
  │       │   ├─ __computeTheoreticalValues → BlackCalc
  │       │   ├─ updateDistortValues (alpha + risk adjustment)
  │       │   └─ computeOurMarkets (spread → bid/ask → ourMarket)
  │       └─ SLOW path (~100ms, IV反解 + GVV曲面重拟合)
  │
  ├─ ctg->refresh()
  │   ├─ check_markets(desired=ourMarket, current=lastQuote)
  │   └─ 收集 pendingQuotes (UT_NEW/UPDATE/CANCEL)
  │
  └─ ctg->drainPendingQuotes()
      ├─ TPS限流
      └─ stra_quote(bidPx, bidSz, askPx, askSz) / stra_cancel
```

## 2. 核心组件

### 2.1 OptionGrid (数据网格)
- 三级结构: expiry (YYYYMM) → strike → Call/Put
- 动态发现: onTick 时自动创建 OptionData
- shared_mutex 保护: 读多写少场景
- 合约发现: CodeHelper::isStdChnFutOptCode (格式: SHFE.ag2608.C.14000)
- 到期日: 从 WTSTickData::getContractInfo() 获取 expireDate
- 交易日历: holidays.json (265天) → countTradingDays → maturity

### 2.2 CompositeOptionPricer (定价引擎)
- FAST/SLOW 双路径
- FAST (~1ms): 缓存vol → BlackCalc → theo + greeks
  - BlackCalc: Black76 模型, sigma = theoVol * sqrt(maturity)
  - Put-call parity: 先算 Call, Put = Call + df*(F-K)
  - Greeks: delta/gamma/vega/theta/vanna/volga/vegaTW
- SLOW (~5ms): IV反解 (Newton-Raphson, maxIter=100) → GVV曲面拟合
- alpha_adjustment: vegaflow + frontfut_skew + deltaflow (EMA 驱动)
- risk_adjustment: delta/vega risk shift + covariance + markup
- computeOurMarkets: spread → bid/ask → ourMarket → quote size

### 2.3 OptionPricer2 (底层定价器)
- __computeTheoreticalValues: BlackCalc 封装
- updateExpiryInfo: forward/discount/maturity/atmvol
- OpenMP parallel_for: 到期日分片并行定价

### 2.4 ControllableTradingGrid (执行调度)
- refresh: check_markets diff → pendingQuotes
- drainPendingQuotes: TPS限流 → stra_quote/stra_cancel
- check_markets: desired(ourMarket) vs current(lastQuote) → UT_NEW/UPDATE/CANCEL/NONE

### 2.5 GvvVolCurve (波动率曲面)
- GVV 模型: 3参数 (atmvol, skew, kurt)
- WLS3: 手写 3×3 Cramer 求解 (替代 GSL, ~0.1μs)
- PeriodicCurveFitter: 周期性重拟合 (~100ms)
- 降级策略: GVV → Linear → Constant

### 2.6 OptionAsyncEventProcessor (异步引擎)
- mutex + condition_variable + deque (非 lock-free)
- AsyncEvent: char[32] code + TickDataRef (trivially copyable)
- MAX_QUEUE_SIZE = 4096, 超限丢弃 + 计数
- batch drain: 每次 notify drain 全部队列

### 2.7 Scanners (信号模块)
- 9 个 Scanner (MM/Butterfly/VolSpread/Open/StrikeSpread/SyntheticFuture/Garch/LowBids/Simplex)
- 当前全部为 stub 实现 (19行, 只有 REGISTER_SCANNER)
- 接口: IScanModule → evalGuts/evalSynpair/sendOrders

## 3. 依赖替换

| 原依赖 (longbeach) | 替换方案 | 性能 |
|---|---|---|
| GSL (gsl_multifit_wlinear) | WLS3 (手写3×3 Cramer) | ~100x faster (0.1μs vs 10μs) |
| TBB (parallel_for) | OpenMP (#pragma omp parallel for) | 零库依赖 |
| QuantLib (Option::Call/Put) | enum OptionType {OT_Call=1, OT_Put=-1} | 零依赖 |
| ExchangeCalendar (DB) | holidays.json + countTradingDays | 零依赖 |
| longbeach ClockMonitor | m_time + setTime() | 零依赖 |

## 4. 配置

### 4.1 回测配置 (configbt.yaml)
```yaml
replayer:
    basefiles:
        commodity: ./common/commodities.json   # 含 ag + ag_o (category=5)
        contract: ./common/contracts.json      # 含 option 块 (strikeprice/underlying/expiredate)
        holiday: ./common/holidays.json        # CHINA 假日列表
        session: ./common/sessions.json        # FN0230 (ag 交易时段)
    mode: wtp
    path: ./storage
    stime: 202607030900
    etime: 202607031500
    tick: true

uft:
    module: ./uft/libWtOptionCore.so
    match_this_tick: true
    strategy:
        name: OptionMM
        params:
            underlyingCode: SHFE.ag.ag2608
            optionProduct: ag
            exchange: SHFE
            riskFreeRate: 0.03
            maxTPS: 50
            optionContracts:    # 当天挂牌的全部期权合约
                - "SHFE.ag2608.C.14000"
                - "SHFE.ag2608.P.14000"
                ...
```

### 4.2 合约配置要点
- commodities.json: `ag_o` 条目, `category=5 (CC_FutOption)`
- contracts.json: 期权合约需含 `option` 块:
  ```json
  "option": {"optiontype": 1, "underlying": "ag2608", "strikeprice": 14000.0, "underlyingscale": 15}
  ```
- stdCode 格式: `SHFE.ag2608.C.14000` (4段, 非5段)

## 5. 回测验证结果

| 验证项 | 结果 |
|---|---|
| 编译 | 509 wt_option 符号, 0 外部依赖 |
| tick 喂入 | 66562 ticks (ag2608 全天) |
| 期权发现 | 42 ATM 期权 (strikes 14000-16000) |
| BlackCalc | sigma=0.0413, Call mid=1012, Put mid=12 |
| Greeks | delta/vega/gamma/theta 有效 |
| alpha/risk | =0 (启动初期, EMA未积累+无持仓) |
| inputs_good | true ✓ |
| 报价 | SET bid=11x5 (P.14000), 1011x5 (C.14000) |
| pendingQuotes | 42 (全部期权) |
| segfault | 0 |

## 6. 已知问题 (优先级排序)

### P0 — 安全底线
1. **无防自成交 (STP)**: MMScanner 双边报价无交叉价检查
2. **无 pre-trade 风控**: OptionRisk 只有事后 pause, 无下单前拦截

### P1 — 数据/配置
3. **currentDate 硬编码**: `20260703` 在 OptionGrid.cpp (TODO)
4. **tick_size 默认值**: 部分路径用 `1.0` 或 `1e-6` (TODO: from contract info)
5. **fees = 0**: `__computeTheoreticalValues` 里 fees 硬编码为 0

### P2 — 代码质量
6. **std::cout 残留**: 21处 (GvvVolCurve/OptionPricer2/OptionRisk), 应改 WTSLogger
7. **Debug 日志**: ~15处 static int xxxCount, 应清理或改 traceLevel 控制
8. **Scanners 全 stub**: 9个 Scanner 各19行, 只有 REGISTER_SCANNER
9. **OptionPricer.cpp 未接线**: 255行 stub + TODO markers
10. **30处 TODO**: 主要在 contract info/tick_size/position wiring

### P3 — 性能
11. **Async queue 用 mutex**: 非 lock-free, CTP线程 enqueue 有锁竞争
12. **OptionValues 大对象**: 377行/154个数据成员, 每期权持有 → 缓存不友好
13. **GvvVolCurve fit 用 std::cout**: 5处 cout 在热点路径

## 7. 构建与部署

```bash
# 编译
cd /home/ubuntu/projects/wondertrader/src/build_stage1
make WtOptionCore -j$(nproc)

# 部署
cp WtOptionCore/libWtOptionCore.so dist/WtBtOption/uft/

# 回测
cd dist/WtBtOption
env LD_LIBRARY_PATH=../bin:./uft ../bin/WtBtRunner -c configbt.yaml -l logcfgbt.yaml
```

## 8. 文件清单 (按模块)

| 模块 | 文件 | 行数 |
|---|---|---|
| **UFT入口** | UftOptionStrategy.cpp/.h | 469+99 |
| | UftOptionStraFact.cpp | 51 |
| **异步** | OptionAsyncEventProcessor.cpp/.h | 303+159 |
| **数据网格** | OptionGrid.cpp/.h | 505+181 |
| | OptionData.cpp/.h | 14+127 |
| | StrikeData.cpp/.h | 75+113 |
| | ExpiryData.cpp/.h | 123+154 |
| **定价** | CompositeOptionPricer.cpp/.h | 1672+462 |
| | OptionPricer2.cpp/.h | 650+274 |
| | BlackCalc.cpp/.h | 257+66 |
| | BlackImpliedCalculator.cpp/.h | 62+152 |
| | OptionPricer.cpp/.h | 255+151 (stub) |
| **波动率** | GvvVolCurve.cpp/.h | 366+93 |
| | LinearVolCurve.cpp/.h | 137+67 |
| | PeriodicCurveFitter.cpp/.h | 402+124 |
| | WLS3.h | 111 |
| **执行** | ControllableTradingGrid.cpp/.h | 359+135 |
| | check_markets.cpp/.h | 58+33 |
| **风控** | OptionRisk.cpp/.h | 307+147 |
| | OptionRiskData.cpp/.h | 133+117 |
| **交易数据** | OptionTradingData.cpp/.h | 185+151 |
| | UnderlyingTradingData.cpp/.h | 110+218 |
| | OptionOrderInfo.cpp/.h | 64+77 |
| | FutureOrderInfo.cpp/.h | 58+65 |
| **PnL** | PnlTracker.cpp/.h | 41+52 |
| **Scanner** | Scanners/*.cpp/*.h (9个) | 187 |
| **Greeks** | OptionGreeks.cpp/.h | 51+67 |
| | OptionExpiryGreeks.cpp/.h | 122+138 |
| **数值** | OptionValues.h | 377 |
| | optioncoretypes.h | 52 |
