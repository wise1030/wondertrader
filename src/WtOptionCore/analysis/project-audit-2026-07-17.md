# WtOptionCore 期权高频做市项目现状分析报告

**日期**: 2026-07-17
**分析人**: 奶酪 (Hermes Agent)
**方法**: 自主代码侦察 + 3 个并行子agent深读（架构/业务逻辑/代码质量+性能）
**Git HEAD**: 4f8eada9 (WtOptionCore P10-P11 参数配置优化+热更新+OQM补齐)
**工作区状态**: 118 个未提交变更（69 个文件，+3,275/-3,764 行）

---

## 一、项目总体概况

**代码规模**: 154 个源文件
- 活跃代码 ~19,100 行（53 个 cpp 在 CMakeLists 中编译）
- 废弃代码 ~7,281 行（27 个文件不在编译链中，占 27.6%）

**框架**: WonderTrader HFT 引擎
- 策略名: `OptionMM`，工厂名: `OptionStraFact`
- 从 quantbox/optiontrader 迁移，经历 V1(自建引擎) → V2(UFT) → 当前(HFT) 三代

**实盘验证状态**:
- SimNow 实时行情接入已通过（7,403 合约订阅，1,816 个 ag_o 期权自动发现）
- 定价链路全通（theo ≈ market 偏差 <1%）
- **P0 阻塞**: isForwardReady 被覆盖导致不报单

---

## 二、四维度评分

| 维度 | 评分 | 关键风险 |
|------|------|---------|
| 架构 | 6.6/10 | God Object strategy(1,784行17+组件)、生命周期无显式 teardown、listener raw ptr |
| 业务逻辑 | 5.7/10 | 3个P0 bug(isForwardReady/risk公式/fee)、risk shift 失效 |
| 代码质量 | B+ (78/100) | 4,808行死代码、单文件1,802行、static debug counter 污染 |
| 性能 | C+ (65/100) | Debug 构建、SLOW 全量遍历、双重 shared_mutex、6,000次红黑树查找/cycle |

**综合: 6.2/10 — 骨架健康但存在多个 P0 阻塞点，未达实盘状态**

---

## 三、架构分析 (6.6/10)

### 当前分层

```
HftOptionStrategy (1,784行) — 策略入口，协调所有组件
  ├── OptionAsyncEventProcessor (353行) — 异步事件队列 + worker 线程
  ├── OptionGrid (739行) — 3级数据网格(expiry→strike→call/put) + 合约发现
  ├── OptionTradingGrid (238行) — 中间层，创建 OTD/ETD/UTD + OQM
  ├── CompositeOptionPricer (1,802行) — 通用做市定价器(FAST/SLOW + ourMarkets)
  │     └── OptionPricer2 (721行) — Black76/BS 定价 + GVV vol curve + doFit
  ├── ControllableTradingGrid (565行) — 报价收集 + TPS限流 + Scanner 合并
  ├── OptionQuoteManager (666行) — per-contract 订单生命周期管理
  └── OptionRisk (307行) — 组合 Greeks 聚合 + 对冲需求计算
```

### 架构优点

1. 组合式替代 quantbox 三层继承——各组件职责清晰，无循环依赖
2. 异步事件处理——CTP 回调线程只 enqueue (~0.5us)，全部业务逻辑在 worker 单线程执行，无竞态
3. 批量处理——on_tick_batch/on_batch_complete 实现"一 batch 一次 compute + 一次 refresh"
4. 配置驱动——13 个热更新参数 + 分层 YAML 配置
5. 三种业务场景统一——商品期权(SHFE)、郑商所双品种(CZCE SRC+SRP)、股指期权(CFFEX IO) 通过配置自动适配

### 架构问题

| 问题 | 位置 | 严重度 |
|------|------|--------|
| HftOptionStrategy 是 God Object（1,784行，17+组件成员） | HftOptionStrategy.cpp | P2 |
| setupAsyncCallbacks 566行，最大函数 | HftOptionStrategy.cpp:812-1378 | P2 |
| on_batch_complete lambda 170行，职责过重 | HftOptionStrategy.cpp:908-1080 | P2 |
| OTG/CTG executor lambda 重复设置 | HftOptionStrategy.cpp:564-574 vs 732-744 | P2 |
| CTG 直接 dynamic_pointer_cast\<CompositeOptionPricer\> | ControllableTradingGrid.cpp:147-151 | P1 |
| IOptionPricer 接口臃肿（11个纯虚函数） | IOptionPricer.h | P3 |
| 析构顺序无显式保证（listener raw ptr 悬垂风险） | HftOptionStrategy.cpp:53-56 | P1 |
| OptionGrid 持有 m_fitter/m_optionPricer（职责越界） | OptionGrid.h:209-210 | P3 |
| OptionTraderContext 是 struct 而非接口 | ControllableTradingGrid.h:37-42 | P3 |
| _pricerType 读取但未使用（死配置） | HftOptionStrategy.cpp:206 | P2 |

### 生命周期管理风险 (5/10)

- `HftOptionStrategy::~HftOptionStrategy` 只 `_async->stop()`，不清理 listener 注册
- `_risk/_otg/_pricer/_ctg` 都注册到 `_grid->addListener(...)`，grid 存 raw `IOptionGridListener*`
- 析构顺序依赖成员声明顺序碰巧安全，无显式 teardown
- `ControllableTradingGrid::m_otg` 是 raw ptr，`_otg` 先释放则悬垂
- `OptionQuoteManager::m_ctx` 是 `IHftStraCtx*` raw ptr，框架 ctx 生命周期不受控
- `ExpiryData::setHedgeUTD(utd.get())` 存 UTD raw ptr，OTG 析构后悬垂

---

## 四、业务逻辑分析 (5.7/10)

### 已完成的业务功能

- 定价链路：BlackCalc → GVV vol curve → FAST/SLOW 调度 → theo/iv/greeks 输出
- 报价生成：computeOurMarkets 完整实现（spread + alpha_adj + risk_adj + QuoteMode 状态机 + trade shock back-away）
- Vol curve 拟合：doFit 双路径（定时 triggerDoFit + SLOW 末尾 doFit），与 quantbox 对齐
- 合成期货 forward：put-call parity 加权平均 + future mid 参与（quantbox 设计）
- 风控：OptionRisk 组合 Greeks 聚合 + risk_adjustment + auto-panic（PnlLimitSignal）
- 热更新：13 个 sync_param 实时生效 + on_params_updated 回调
- QuoteMode 状态机：ON/AUTO/CLOSE/OFF + auto_close 自动切换
- Scanner 框架：9 个 Scanner 已注册到 CTG + combo order 执行
- 多品种/多到期月：per-expiry underlyingCode/hedgeCode + secondaryHedgeCodes
- Attribute Publisher + PnL Tracker + Expiration Simulator + Option Value Writer
- rankOption 7 因子完整实现（isBest/crossing/delta/spread/expiry/type权重）
- TPS 限流 + 保留 dropped 重试（B16）
- PositionGuard + RiskFilterChain + 自成交防护 STP

### P0 阻塞问题

#### P0-1: isForwardReady 覆盖 → 不报单

- **位置**: OptionGrid.cpp:671
- **根因**: `__getBestSyntheticPrice` 在 validCount 不足时无条件 `setForwardReady(false)`，覆盖之前成功设置的 true。`updateTheoreticalValuesFuture` 已不再恢复（L659 removed），没有恢复路径
- **影响链**: 夜盘流动性不足 → validCount < minStrikes → setForwardReady(false) → expiry_ready=false → computeValue 不调用 → inputs_good=false → ourMarket.clear() → 不报单
- **修复方向**: 粘性语义——仅在从未 ready 时才覆写 false；曾 ready 则保留。或维护 lastValidTime，超过 N 秒无有效 forward 才置 false

#### P0-2: risk_adjustment total_risk 公式错误 → 风控信号失效

- **位置**: CompositeOptionPricer.cpp:1589-1590
- **根因**: `total_risk = vega_risk2 + delta_risk2 - cov`，但 **`delta_risk`(shift*og) 和 `vega_risk`(shift*og) 完全没加进 total**
- **影响**: risk shift 机制（updateRiskShiftsDelta/Vega 计算的风控倾斜）从未真正影响报价，所有希腊字母风险容忍度(risk_tol)失效
- **修复**: `total_risk = delta_risk + vega_risk + delta_risk2 + vega_risk2 - cov`

#### P0-3: fee 未注入 → 报价 spread 偏低 → 实盘亏损

- **位置**: OptionData.h:98-99 + OptionGrid.cpp:303-310
- **根因**: OptionInfo.fee 默认 0，`setFee()` 有接口但**全文搜索无调用点**
- **影响**: `__getOptionCosts` L709 `core_spread += 2.0 * m_fees` 实际加 0，报价 spread 偏低 2 倍手续费
- **修复**: `OptionGrid::__createOption` 中从 commInfo 读取 fee 注入

#### P0-4: tick 去重不工作

- **位置**: OptionAsyncEventProcessor.cpp:300
- **根因**: `unordered_map<const char*>` 用指针地址做 key，同一合约不同事件的 code 地址不同
- **影响**: 行情爆发时 worker 做 N 倍无用功
- **修复**: 改 `unordered_map<string_view, const TickData*>` 或用 internalId 索引

#### P0-5: Debug 构建

- **位置**: CMakeCache.txt `CMAKE_BUILD_TYPE:STRING=Debug`
- **影响**: 全部热路径代码慢 3-10 倍
- **修复**: `cmake -DCMAKE_BUILD_TYPE=Release` + `-march=native -flto`

### P1 业务问题

| # | 问题 | 位置 | 影响 |
|---|------|------|------|
| 1 | QM_CLOSE 状态切换矛盾 | CompositeOptionPricer.cpp:996-1008 | enable_auto_close=false 时逻辑混乱 |
| 2 | GVV 参数未从 config 注入 | OptionPricer2.cpp:216-224 | 拟合精度受限 |
| 3 | hedge 对冲执行未实现 | OptionRisk 只计算需求 | 无自动对冲下单 |
| 4 | GammaScalpOptionPricer 未接入 | HftOptionStrategy.cpp:206 | _pricerType 是死配置 |
| 5 | combineMarkets 简化（仅 best 合并） | ControllableTradingGrid.cpp:230-242 | 多 pricer 时无法充分利用定价源 |
| 6 | m_optUpdateSet 写入但未消费 | CTG:97,169 写入 vs refresh 未消费 | 增量刷新未实现 |
| 7 | Gamma/Theta risk shift 缺失 | CompositeOptionPricer | 无 gamma_tol/theta_tol |
| 8 | close_thresh 硬编码 0 | CompositeOptionPricer.cpp:1003 | 应从 config 读取 |
| 9 | future tick_size=1.0 fallback | CompositeOptionPricer.cpp:775,1113 | 静默错误 |
| 10 | future feePct/contractSize 未注入 | UnderlyingTradingData.h:205-207 | 期货报价不准 |
| 11 | updateOurMarketSide 死代码 | CompositeOptionPricer.cpp:1159-1295 | cancelByPrice 无法下沉 |
| 12 | QuoteMode 切换无 hysteresis | CompositeOptionPricer.cpp:994 | AUTO↔CLOSE 抖动 |

---

## 五、代码质量分析 (B+ 78/100)

### 优点

- TODO/FIXME 为 0——技术债务标记干净
- 注释质量高——每个文件有详细的文件头注释，关键方法有行内注释（B1-B16 编号注释）
- README 完善（625行），覆盖架构/组件/配置/部署全链路
- 命名规范统一（m_/m_sp/m_map 前缀，wt_option namespace）
- shared_ptr 使用合理（weak_ptr 防循环引用），热路径无 new

### 问题

| 问题 | 位置 | 量化影响 |
|------|------|----------|
| 废弃代码 7,281 行(27.6%) | OrderManager/WtOptContext/WtOptEngine/WtOptionStrategy/VolCurve/OptionPricer 等 27 个文件 | 认知负担 |
| setupAsyncCallbacks 566行 | HftOptionStrategy.cpp:812-1378 | 7个 lambda 全在一个函数里 |
| computeOurMarkets 239行 | CompositeOptionPricer.cpp:847-1086 | 报价生成核心逻辑过长 |
| init() 256行 | HftOptionStrategy.cpp:61-317 | config 读取 + 参数初始化混在一起 |
| CompositeOptionPricer.cpp 1,802行 | 全文 | FAST/SLOW/risk/alpha/onFill/clearLastTrades 混在一处 |
| static debug counter 无线程保护 | CompositeOptionPricer.cpp:852,626,1534,310 | 全局 cache line 争用 + UB |
| OptionGrid.cpp 死注释代码 | OptionGrid.cpp:371-376,384-390 | 死代码 |
| 9 个已删除文件仍在 git 工作区 | ForecastSignal.h/UftOptionStrategy 等 | 需要 commit 清理 |

---

## 六、性能分析 (C+ 65/100)

### 已实现的性能优化

- CTP 回调线程只 enqueue (~0.5us)，LockFree SPSC queue + overflow fallback
- 批量事件处理 + O(N) bucket sort 替代 O(N log N) stable_sort
- FAST/SLOW 定价调度（FAST=增量/SLOW=全量 refit）
- 持仓合并单遍执行（P5: 多次 getAllOptions 融合为一次遍历）
- char[32] 替代 std::string 避免堆分配（AsyncEvent）
- pendingQuotes 保留 drop 重试（B16: retainedDrops）
- underlying-driven compute scheduling（debounce _minComputeInterval=0.02s）

### 性能热点（按预期收益排序）

| # | 问题 | 位置 | 量化影响 |
|---|------|------|----------|
| 1 | Debug 构建（-g 无 -O） | CMakeCache.txt | 全部热路径慢 3-10x |
| 2 | SLOW 全量遍历 1,816 合约 | CompositeOptionPricer.cpp:409-462 | 单次 SLOW ≈ 5-20ms 阻塞 worker |
| 3 | updateRiskShiftsVega O(N²) | CompositeOptionPricer.cpp:1358-1466 | 百万级操作/cycle |
| 4 | ExpiryRiskConfig map 6,000次find/cycle | CompositeOptionPricer.cpp:856 等 | 应改 array/vector 索引 |
| 5 | OptionGrid shared_mutex x2 (11处lock) | OptionGrid.cpp 多行 | ~500ns/cycle 纯浪费 |
| 6 | tick 去重失效（指针比较） | OptionAsyncEventProcessor.cpp:300 | 行情爆发时 worker N 倍负载 |
| 7 | Worker CV wait_for(100us) 硬底延迟 | OptionAsyncEventProcessor.cpp:248 | 空闲空转/突发+100us |
| 8 | m_atmFwdCache 每 cycle clear+rebuild | OptionGrid.cpp:569-580,682 | map 查找/插入开销 |
| 9 | PendingQuote 每 cycle vector clear+push+sort | CTG:213-336 | ~50-200us/cycle |
| 10 | clearLastTrades 2次 map find/computeOurMarkets | CompositeOptionPricer.cpp:939-954 | ~100us/cycle |
| 11 | onTick std::string by-value + string 哈希 | OptionGrid.cpp:83-160 | ~30-50ns/tick |
| 12 | 无 -march=native -flto -O3 | CMakeLists.txt | 编译优化缺失 |
| 13 | OpenMP 链接但未使用 | OptionPricer2.cpp use_parallel_for | SLOW 并行 -70% 未启用 |

---

## 七、优化改进建议（按优先级排序）

### P0 — 实盘阻塞（本周必修）

1. **isForwardReady 粘性语义** — OptionGrid.cpp:670-672 仅在从未 ready 时才覆写 false
2. **risk total 公式修复** — CompositeOptionPricer.cpp:1589 加 delta_risk + vega_risk
3. **fee 注入** — OptionGrid::__createOption 从 commInfo 读 fee 调 setFee()
4. **tick 去重 key 修复** — OptionAsyncEventProcessor.cpp:300 改 string_view 或 internalId
5. **Release 构建** — cmake -DCMAKE_BUILD_TYPE=Release

### P1 — 实盘质量提升（下周修）

6. OptionGrid 读写竞态修复（onTick 无锁遍历 m_expiries vs __createOption 持锁写）
7. SLOW 增量计算（dirty 标记，只重算有变化的合约）
8. updateRiskShiftsVega 改增量（去 O(N²)）
9. ExpiryRiskConfig map → array/vector 索引
10. QM_CLOSE 状态切换矛盾修复
11. GVV 参数从 config 注入
12. 显式 teardown 顺序（HftOptionStrategy::shutdown()）
13. m_optUpdateSet 消费（增量 refresh）
14. pricerType 工厂（GammaScalp/Standard 接入）
15. hedge 自动对冲执行

### P2 — 代码清理 + 性能优化

16. 废弃代码 7,281 行 mv → _trash/legacy/
17. setupAsyncCallbacks 566行拆分为 7 个独立方法
18. CompositeOptionPricer.cpp 1,802行拆 4 文件
19. OptionGrid shared_mutex → atomic<double>
20. m_atmFwdCache map → array
21. Worker CV 改 spin+cv 混合策略
22. PendingQuote.code string → internalId
23. Release 加 -march=native -flto -O3
24. static debug counter 移除（改条件日志宏）
25. clearLastTrades map → internalId 索引
26. Gamma/Theta risk shift 补齐
27. OpenMP SLOW 并行化启用
28. future tick_size/feePct/contractSize 显式注入

### P3 — 架构优化

29. OptionGrid 去 m_fitter/m_optionPricer（职责越界）
30. IOptionPricer 拆分（抽 IVolCurveProvider）
31. OTG/CTG executor 统一（删 OTG 侧重复设置）
32. CTG::m_otg raw ptr → weak_ptr
33. OQM 补 cancelByPriceRange/max_side_orders>1
34. combineMarkets 按 MultiMarket level 全量合并
35. 交易所规则表外置（CFFEX_MAP/CZCE后缀/\_o后缀→JSON）
36. OptionTraderContext struct → interface
37. on_batch_complete lambda 拆分

---

## 八、行动路线图

**本周（P0）**: 修 5 个 P0 → 实盘可用
1. isForwardReady 粘性语义
2. risk total 公式补 delta_risk + vega_risk
3. fee 注入
4. tick 去重 key 改 string_view
5. Release 构建

**下周（P1）**: 10 项质量提升 → 实盘稳定

**2-4 周（P2/P3）**: 代码清理 + 性能优化 + 架构对齐 → 对齐全量 quantbox

---

## 九、分析方法记录

- 自主代码侦察：git log/status/diff、文件行数统计、CMakeLists 编译链确认、废弃文件识别、关键函数逐行阅读（on_init/setupGrid/setupPricer/setupCTG/setupAsyncCallbacks/on_batch_complete/on_tick/__getBestSyntheticPrice/computeOurMarkets/computeValues_SLOW/updateTheoreticalValuesFuture/updateDistortValues/refresh/drainPendingQuotes/worker_loop）
- 3 个并行子agent深读：架构维度（init/on_init/async/CTG/OTG/OQM）、业务逻辑维度（computeOurMarkets/updateDistortValues/__getOptionCosts/__getBestSyntheticPrice/updateTheoreticalValuesFuture/computeValue/doFit/OptionRisk/GvvVolCurve）、代码质量+性能维度（OptionGrid 全文/CompositeOptionPricer FAST-SLOW/OptionAsyncEventProcessor 全文/CTG refresh+drain/CMakeLists/TODO搜索/日志统计）
- skill wt-option-core 记录交叉验证（发现多项 skill 记录已过时：Scanner已注册/combineMarkets已实现/tickSize已注入/setForwardReady(true)已被移除）
