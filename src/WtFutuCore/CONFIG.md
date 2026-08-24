# WtFutuCore 配置文件逐键说明（含热更新配置）

> 配套文档：README.md（架构与业务逻辑）。本文覆盖全部 yaml 的每个字段：
> 键路径｜类型｜示例值｜缺省｜说明（单位/效果/生效方式）。
> 行号锚点 = 解析代码位置，可对照检查。
>
> **生效时机三类**：
> - `启动`——进程启动读取一次，改后需重启
> - `热更`——实盘由 watcher 1Hz 轮询 hotparams.yaml 生效（回测不生效！）
> - `共享内存`——重启后残留旧值立即生效（见 §4 优先级链）

---

## 目录

- [§1 配置加载链路总览](#1-配置加载链路总览)
- [§2 主配置（configbt_v5.yaml / _ec_5d.yaml）](#2-主配置)
- [§3 模块配置 coordinator.yaml](#3-模块配置-coordinatoryaml)
- [§4 热更新配置 hotparams.yaml（27 参数）](#4-热更新配置-hotparamsyaml)
- [§5 日志配置 logcfgbt.yaml](#5-日志配置-logcfgbtyaml)
- [§6 套利配置 spread_arbitrage.yaml](#6-套利配置-spread_arbitrageyaml)
- [§7 死键清单（配了也不生效）](#7-死键清单)
- [§8 易错点与排障速查](#8-易错点与排障速查)

---

## 1. 配置加载链路总览

```
WtBtRunner/WtUftRunner -c <主配置.yaml>
 ├ replayer/env 节 → 引擎层解析 (HisDataReplayer.cpp:169-298 / WtBtRunner.cpp:80)
 ├ uft.strategy.params.* → 整体传给 strategy->init()
 │    → FutuConfigLoader::load (FutuConfigLoader.cpp:12) + 校验(:208-342)
 │        ├ params.coordinatorConfig → StrategyCoordinator::loadConfigFromVariant
 │        │                            (StrategyCoordinator.cpp:177-192)
 │        └ 各 modules.* 节 → FutuModuleAssembler 经 _raw_variant 分发各 fromVariant
 └ 实盘另起 hotparam watcher 轮询 hotparams.yaml(1Hz) → 共享内存 → on_tick drain 应用

读值语义(V8-R5): FutuConfig::readDouble/readBool 对 键缺失/VT_Null/空串/类型错
  回落默认值; readBool 支持数值型(YAML `1`→true)   (FutuConfig.cpp:16-75)
```

---

## 2. 主配置

两个样例 schema 完全一致：`dist/configbt_v5.yaml`（ao 品种冒烟）与 `_ec_5d.yaml`
（EC 品种 5 日基线）。差异仅在数值与个别可选键。

### 2.1 replayer 节（回放引擎，HisDataReplayer.cpp:169-298）

| 键 | 类型 | v5 示例 | 缺省 | 说明 |
|---|---|---|---|---|
| replayer.mode | string | storage | 空 | storage/bin/wtp（引擎层） |
| replayer.path | string | ./storage/ | 空 | 历史数据根目录；有 store 子节则读 store.path |
| replayer.stime / etime | uint64 | 202606012000 / 202606051530 | 0 | 回测起止 yyyyMMddHHMM |
| replayer.tick | bool | true | false | 是否回放 tick（键缺失=false） |
| replayer.align_by_section | bool | false | false | 重采样 bar 按小节对齐 |
| replayer.dont_simtick_if_notrade | bool | true | false | 无成交不模拟 tick |
| replayer.fees | string | ./common/fees.json | 空 | 费率文件 |
| replayer.basefiles.session/commodity/contract/holiday | string | ./common/*.json | 必填 | 基础数据四件套 |

### 2.2 env 节（WtBtRunner.cpp:80-125）

| 键 | 示例 | 说明 |
|---|---|---|
| env.mocker | uft | cta/hft/sel/exec/uft 分派 |
| env.slippage | 0 | 仅 cta/sel mocker 使用（UftMocker 无滑点参数） |

### 2.3 uft 节（UftMocker.cpp:128-165）

| 键 | 示例 | 缺省 | 说明 |
|---|---|---|---|
| uft.module | ./uft/libWtFutuCore.so | 必填 | dlopen 策略工厂 |
| uft.use_newpx | false | false | 撮合新价模式（引擎层） |
| uft.error_rate | 0 | 0 | 模拟拒单错误率 ‱（压测 ERROR/HALT 路径用） |
| uft.match_this_tick | true | false | 当 tick 内即时撮合 |
| uft.strategy.id / name | futu_mm_ao_bt / FutuMM | 必填 | 实例 id 与工厂创建名 |
| uft.strategy.params.* | ↓ | ↓ | **策略层全部配置的入口**（下表） |

### 2.4 uft.strategy.params（策略层，FutuConfigLoader.cpp）

#### 身份与路径

| 键 | 类型 | v5/_ec_5d | 缺省 | 校验 | 说明 |
|---|---|---|---|---|---|
| anchorCode | string | SHFE.ao.ao2609 | "" | E2/E3 error | 锚定合约（lead-lag 的 lead、closeout 时刻来源）；必须出现在 contracts |
| isBacktest | bool | true | **false** | — | 回测开关。**缺失时 live-only 补挂在回调栈发单→mocker 迭代器失效死循环**（AGENTS §4） |
| coordinatorConfig | string | ./coordinator.yaml | "coordinator.yaml" | — | 模块开关/参数唯一权威文件路径（:35） |
| spreadArbitrageConfig | string | ./spread_arbitrage.yaml | 同名缺省 | — | 套利配置路径（:36） |

#### contracts[] 数组

| 键 | 类型 | 示例 | 缺省 | 校验 | 说明 |
|---|---|---|---|---|---|
| contracts[].code | string | INE.ec.ec2607 | 必填 | — | 三段式 stdCode |
| contracts[].multiplier / tickSize | double | (未配,-1) | -1=自动 | — | -1 时从基础数据回填；查不到→1.0/0.2+warn（Assembler:766-819） |
| contracts[].maxPosition | double | 50 | -1 | — | **风控硬顶（手，position 口径）**——触发 BLOCK_SIDE/HALT/AUTO REDUCE |
| contracts[].maxDelta | double | 50 | -1 | warn 若 >maxPosition | **单合约 delta 软限**——skew 归一化分母/block_add 阈值（delta 口径）；≤0 关单合约 skew |
| contracts[].targetPosition | double | 0 | 0 | — | 目标持仓，超出倾向减仓 |

> 口径铁律：maxPosition 只进风控闸门；maxDelta 只进策略报价参数。二者数值可以不同
> （_ec_5d 相等），不同时 util 缩放变化（test_inventory_delta_separation 守护）。

#### quoting 节

| 键 | 类型 | v5 | 缺省 | 校验 | 说明 |
|---|---|---|---|---|---|
| numLevels | uint32 | 1 | 1 | E6 [1,10] | 报价层数 |
| baseSpread | double | 2.5 | 2.0 | E10 (0,20] ticks | 基础价差(ticks)。**热参 base_spread 可覆盖**（v5 有漂移告警属预期） |
| baseQty | double | 3.0 | 5.0 | E11 (0,100] 手 | 义务层报单量。热参可覆盖 |
| qtyDecay(levelQtyMultiplier) | double | 0.5 | 0.7 | W4 [0.1,1.0] | 层量衰减系数。热参 level_qty_multiplier 可覆盖 |
| levelStep | double | 1.0 | 1.0 | E7 (0,100] ticks | 层间价距 |
| stickyThreshold | double | 1.5 | 1.0 | W5 (0,10] ticks | **顶单黏性专用**（B1 收窄语义）：价格改善 ≥ improve_retreat_ratio×sticky 才重挂。热参可覆盖 |
| improveRetreatRatio | double | 2.0 | 2.0 | — | 改善侧容忍倍数（与 selfTradeCalibrator.retreatTicks 是**两个无关机制**） |
| maxPriceDeviation | double | 20.0 | 20.0 | W6 [0,100] ticks | 最大价格偏离（超限撤旧不挂新）。热参可覆盖 |
| useBilateralQuote | bool | false | false | E9 与 obligationLevel≠0 互斥 | 路径 A 双边报价接口（当前禁用，启用需专项验证——B+ 复核遗留） |
| priceProtection | bool | true | true | — | 价格保护总开关（protect_ticks 关闭应走此开关而非配 0） |
| protectTicks | double | 1.0 | 1.0 | — | 保护带宽度(ticks)：报价不得进入对手价×带内。热参可覆盖（下界 0.5） |
| qtyDecayFactor | double | 3.0 | 2.0 | — | qty 衰减软风控因子 |
| obligationMinQty | double | 3 | 10.0 | E12 >0; W2 <scoutQty | 义务总深度阈值(手)：同侧总挂量低于此值强制补义务层 |
| obligationMaxSpreadTicks | double | 10 | 10.0 | — | 义务层最大带宽(ticks)（须>skewCrossMaxTicks） |
| obligationLevel | uint32 | (未配)/_ec_5d=1 | 0 | E8 <numLevels | scout 多层义务层序号：level<此值为自由探测层(scoutQty)，==为义务层(baseQty)，之后 flexible 衰减层 |
| scoutQty | double | (未配)/_ec_5d=1.0 | 1.0 | W1 ≤baseQty | scout 层单量(手)；scout 成交=逆向信号→撤同侧义务层 |

#### portfolio 节

| 键 | 示例 | 缺省 | 校验 | 说明 |
|---|---|---|---|---|
| maxDelta | double | 50 | 50.0 | E4 (0,1e8] 组合 delta 软限——组合 skew 分母/WIDEN util。热参 max_delta 可覆盖 |
| hedgeRatio | double | 1.0 | 1.0 | E13 [0,1] 默认对冲比率（correlation beta 平滑的初值） |
| ~~hedgeDeltaThreshold~~ / ~~hedgeCooldownMs~~ | — | — | — | **死键**（loader 无读取点） |

#### risk 节

| 键 | 示例 | 缺省 | 校验 | 说明 |
|---|---|---|---|---|
| maxExposure | double | 35000000 | 35000000.0 | E5 >0 组合敞口上限(元) |
| maxDailyLoss | double | -200000 | -200000.0 | 日亏线(元)；装配取 abs 作正容忍度；触发=IRREVERSIBLE halt |
| frequency.maxOrdersPerSec | uint32 | 50 | 50 | E14 [1,500] 报单频控（P0-2 后 MM 报单经 recordOrders 计入） |
| frequency.maxCancelsPerSec / maxTradesPerSec | uint32 | 30/10 | 30/20 | 撤单/成交频控 |
| frequency.maxDeltaChangePerSec | double | 50.0 | 3.0 | delta 变化率上限/s（DELTA_RATE 触发源） |
| frequency.deltaRateWindowSec / deltaRateCooldownMs | uint32 | 10/5000 | 2/15000 | delta 速率滑窗/冷却（恢复须过冷却） |
| frequency.positionHardBlockRatio | double | (未配)/_ec_5d=1.0 | 1.0 | block_add 阈值：\|delta\| ≥ contract_max_delta×ratio 时 flexible 加仓侧跳过（键名历史遗留，实际 delta 口径） |
| frequency.deltaCriticalMult / deltaWarningMult | double | 1.5/0.8 | 同左 | delta 分级告警倍数（CRITICAL/WARNING） |
| frequency.widenThreshold / pauseThreshold / flattenThreshold | uint32 | 1/2/3 | 同左 | 库存分级响应档位（widen/pause/flatten 的 util 阈值分子） |
| frequency.positionWarningL1/L2 | double | (未配) | 0.8/0.9 | 持仓预警档位 |
| frequency.maxConsecutiveSameSide / sameSideWindowMs / sameSidePauseMs | uint32 | (未配) | 5/3000/5000 | SideFillBreaker 同侧连续成交熔断参数 |
| frequency.cooldownMs | uint32 | 30000 | 30000 | halt 恢复冷却 ms |
| frequency.checkIntervalMs | uint32 | 5000 | 5000 | 恢复尝试节流 ms |
| frequency.recoveryThreshold | double | 0.8 | 0.8 | 恢复条件：util 降至阈值以下比例 |
| frequency.maxRecoveryCount | uint32 | 3 | 3 | 每 session 恢复预算次数（D1 后下单错误不再消耗它） |
| frequency.pnlRecoveryRatio | double | 0.5 | 0.5 | 盈利恢复条件比例 |
| frequency.autoClearIrreversibleOnReset | bool | (_ec_5d true) | **false** | 日界自动清 IRREVERSIBLE halt（慎开——日亏线保险丝失效风险自担） |

#### closeout 节

| 键 | 示例 | 缺省 | 说明 |
|---|---|---|---|
| minutesBefore | uint32 | 10 | 5 | 白盘收盘前 N 分钟触发平仓（close_time 自动从 anchor session 推导，不可配 :154-155/Assembler:771-805） |
| nightMinutesBefore | uint32 | 10 | =minutesBefore | 夜盘版（EC 无夜盘则删键） |
| flattenPosition | bool | true | true | 是否实际平仓（false 仅停报） |
| maxRetries / retryIntervalMs | uint32 | 10/3000 | 3/5000 | 平仓重试 |
| drainTimeoutMs | uint32 | 3000 | 3000 | 排空超时 |
| depthRatioPassive/Mid/Aggressive | double | 0.3/0.5/0.8 | 同左 | 渐进平仓三档深度比率（盘口占比） |
| sweepThresholdMs / sweepTicks | uint32 | 5000/3 | 同左 | 扫单判定（时间窗内价格移动超 ticks 判扫单→升档） |
| useFak | bool | true | true | 平仓用 FAK 单 |

#### order_control 根级键

| 键 | 示例 | 缺省 | 说明 |
|---|---|---|---|
| orderErrorThreshold | uint32 | 3(v5)/1000(生产临时) | 10 | 连续下单错误计数阈值→qphase=ERROR+cancelAll（D1 后不再叠加 haltTrading）。生产 1000 是 31 拒单事故临时值待回收 |
| maxOrders | uint32 | 32 | 32 | 最大在途订单数 |
| maxPendingPerSide | double | (未配)/_ec_5d=30 | 30.0 | 单侧最大挂单量(手)，超限撤旧+跳过 |
| ~~stpMinPriceGap~~ | — | — | — | **死键**；STP 权威在 coordinator.modules.selfTradePrevention |

#### performance 节

| 键 | 示例 | 缺省 | 说明 |
|---|---|---|---|
| latencyThreshold | uint64(ns) | 1000000 | 100000 | 监控延迟阈值 |
| enabled | bool | false | true | 开关（实际装配仍以 coordinator.usePerformanceMonitor 为准） |
| logInterval | uint32 | 60000 | 1000(ms) | 输出间隔 |
| warnThresholdNs / criticalThresholdNs | uint32 | 100000/500000 | 10000/50000 | 分级告警(ns) |

---

## 3. 模块配置 coordinator.yaml

**模块开关与参数的唯一权威**（config 内同名节自 V8-R6 起零读取）。全部为启动期生效；
与 hotparams 重叠的键会被热参覆盖（见 §4 优先级链）。

### 3.1 根级开关与运行参数

| 键 | 类型 | 示例 | 缺省 | 生效 | 说明（加载点 StrategyCoordinator.cpp） |
|---|---|---|---|---|---|
| useMarketMaking | bool | true | true | 启动 | MM 总开关；false 级联关 toxicity/spreadOptimizer(:237,260-263) |
| useSpreadArbitrage | bool | true | false | 启动 | 套利开关；=true 时 STP 强制开启(Assembler:309-312) |
| useAsyncArbThread | bool | false | **true** | 启动 | **回测必须 false**（主线程同步保可复现，Assembler:145） |
| usePerformanceMonitor / usePerformanceAnalyzer | bool | false/false | false | 启动 | 延迟监控/绩效分析开关 |
| use_signal_aggregator | bool | true | true | 启动 | 新信号架构；false 走 legacy(:239) |
| takerReduceThreshold | double | 1.1 | 1.3 | 启动 | util=\|净头寸\|/maxPosition 超此值→taker 减仓；0=禁用(:321) |
| takerReduceTargetUtil | double | 0.8 | 0.8 | 启动 | 减仓目标利用率(:322) |
| takerReduceCooldownMs | uint32 | 30000 | 30000 | 启动 | 每合约限频 ms(:323) |
| requoteAfterFillMinIntervalMs | uint32 | 200 | 200 | 启动 | 成交后重挂最小间隔 ms；0=禁用(:326) |
| sectionBreakSecondsBefore | uint32 | 10 | 10 | 启动 | 每节收盘前 N 秒撤单停报（旧分钟键×60 兼容 :331-335） |
| bilateralStatsLogIntervalSec | uint32 | 300 | 300 | 启动 | 双边统计周期输出间隔 s；0=禁用(:338) |
| bilateralStatsLogDir | string | ./Logs | ./Logs | 启动 | 双边统计持久化目录（相对 CWD） |

### 3.2 pipeline 节

| 键 | 示例 | 缺省 | 说明 |
|---|---|---|---|
| paramUpdateInterval | uint32 | 100 | 100 | 自适应参数更新间隔(tick)；**配 0 强制改 1 防 SIGFPE**(:275-278) |
| alphaSensitivity | double | 2.0 | 2.0 | Alpha 灵敏度（公允价偏移系数，冷启动保护也用）。热参 alpha_sensitivity 可覆盖 |

### 3.3 modules.signalAggregator（signals/SignalAggregator.h:78-186）

presence 即启用：`signals.<name>` 存在才启用该信号源。

| 键路径 | 示例 | 缺省 | 说明 |
|---|---|---|---|
| signals.ofi.window | 50 | 50 | OFI 窗口(tick 数) |
| signals.trade_flow.window | 100 | 100 | 交易流窗口 |
| signals.trade_flow.largeTradeThreshold | 50.0 | 50.0 | 大单阈值(手)——**单一口径**：同步灌入 MarketDataContext/TickTransactionInferer(P0-4) |
| signals.book_imbalance.threshold | 0.2 | 0.2 | 盘口失衡阈值 [0,1] |
| signals.momentum.window | 50 | 50 | 动量窗口——最近 min(window,128) 收益（S6 后真实生效，行为变化已归因） |
| signals.momentum.emaAlpha | 0.1 | 0.1 | EMA 平滑系数 |
| signals.lead_lag.window | 50 | 50 | 领先滞后窗口 |
| signals.lead_lag.maxAgeMs | (未配) | 0 | B5 alpha 时效老化 ms；0=关闭。唯一持久化 last-value 通道的防护 |
| model.type | linear | linear | 组合模型；仅注册表内 "linear" 合法，否则 error+拒装配(Assembler:492-495) |
| model.weights.ofi/trade_flow/book_imbalance/momentum/lead_lag | .35/.25/.20/.15/.05 | 同左 | Layer1 base 权重；和≈1 由热参交叉复查 warn。热参 ofi_weight 等 5 键可覆盖 |
| model.strongThreshold | 0.7 | 0.7 | 强信号阈值。热参 strong_threshold 可覆盖 |
| model.rollingWindow / rollingInterval / icUpdateInterval | (未配) | 500/20/50 | B3 归一化窗口/滚动节拍/IC 更新间隔(tick) |
| volatility.window | 100 | 100 | realized_vol 窗口（始终启用） |
| volatility.elevatedThreshold | 0.0005 | 0.0005 | ≥→ELEVATED+widen（EC p95 实测标定 S10） |
| volatility.extremeThreshold | 0.0017 | 0.0017 | ≥→EXTREME+pause（≈p99.5） |
| volatility.statsLogInterval | 0 | 0 | vol 分布埋点间隔 tick（标定工具，默认关） |
| warmupTicks | 20 | 50 | 预热 tick 数（is_ready 门/ColdStartPolicy 输入） |

### 3.4 modules.toxicityDetector（ToxicFlowDetector.h:53-96）

加载期边界校验：越界 warn+回落默认。

| 键路径 | 示例 | 缺省 | 边界 | 说明 |
|---|---|---|---|---|
| enabled | true | true | — | 模块级开关**唯一位置**(:255) |
| adverseThreshold | 0.75 | 0.10 | (0,1] | combined 触发阈值（R2 定标：分数密集区 0.65-0.85，0.65→0.75 敏感度低） |
| vpinThreshold | 0.60 | 0.10 | (0,1] | VPIN 独立 OR 条件（T4） |
| vpinWeight | 0.5 | 0.5 | [0,1] | combined 中 VPIN 权重（alpha=1-w） |
| window | 20 | 50 | — | VPIN 桶数窗口 |
| bucketSize | 50 | 1000 | — | VPIN 桶大小(量) |
| minWarmupBuckets | 5 | 5 | — | 预热桶数门 |
| cooloffMs | 5000 | 5000 | — | 毒性冷却 ms（qphase=TOXICITY 时长） |
| alphaWeight / bookWeight | 0.5/0.3 | 0.3/0.3 | 和>0 | alpha 通道内权重（通道内归一 T6） |
| selfTradeWeight | 0.4 | 0.4 | [0,1] | 自成交通道权重（单次施加 T2） |
| extremeSignalWeight | 0.8 | 0.8 | — | 极端信号兜底权重 |
| extremeSignalThreshold | 0.9 | 0.9 | [0.5,1] | extreme 判定门槛（R2：原 0.6 落在 OFI 常态区属误触发路径） |

> 三层响应联动（A4 注记）：本节 is_toxic → 停边+cooloff；GLFT toxicity_spread_factor 加宽
> （过 toxicity_min_score 门槛）；GLFT pause mult≥max×pauseSpreadMultRatio。

### 3.5 modules.spreadOptimizer（GLFTParams::fromVariant，SpreadOptimizer.h:89-120）

节点缺失时整组默认并 warn。baseSpread/tick_size/portfolio_max_delta 不读本节点。

| 键路径 | 示例 | fromVariant 缺省 | 说明 |
|---|---|---|---|
| phi | 0.20 | 0.20 | 波动率加价系数（实际角色=A4 澄清；库存厌恶由 delta_skew_* 承担）。热参可覆盖 |
| deltaSkewThreshold | 0.3 | 0.3 | 组合 skew 触发 util（死区）。热参可覆盖 |
| deltaSkewFactor | 1.5 | 1.5 | 组合 skew 强度。热参可覆盖 |
| deltaSkewPower | 1.5 | 1.5 | 两维 skew 幂次（fastPow 特化 1.5/2.0/1.0/0.5） |
| maxSpreadMult / minSpreadMult | 3.0/1.0 | 同左 | spread clamp 区间（min>max 由 crossCheck warn）。热参可覆盖 |
| confidenceWeightMin / Max | 0.3/1.0 | **0.2**/1.0 | 公允价置信度插值权重。热参可覆盖 |
| lowConfidenceThreshold / lowConfidenceSpreadFactor | 0.3/2.0 | 同左 | 低置信扩价(M8)。后者热参可覆盖 |
| depthSensitivity | 0.5 | 0.5 | 深度敏感度。热参可覆盖 |
| depthSensitivityScale / depthNormalization / noDepthSpreadMult | 0.2/100/1.5 | 同左 | 深度项辅助参数 |
| volScale / volPercentileScale | 5.0/50.0 | 同左 | φσ² 缩放/分位归一分母 |
| pauseSpreadMultRatio | 0.9 | 0.9 | spread_mult≥max×此值→暂停报价(SpreadOptimizer.cpp:217) |
| toxicitySpreadFactor | 1.0 | 1.0 | 毒性加宽系数(L1)。热参可覆盖 |
| toxicityMinScore | (未配) | 0.05 | B4：加宽最小分数门槛（噪声过滤） |
| inventorySkewGain | 1.0 | 1.0 | 归一化库存 skew 增益（1.0=util≥1 贴 mid） |
| skewCrossMaxTicks | 3.0 | 3.0 | util≥1 减仓侧穿越 mid 的最大 tick（须<obligationMaxSpreadTicks） |
| portfolioSkewWeight / contractSkewWeight | 0.5/1.0 | 同左 | 双维 skew 权重（C1 后同 ticks 量纲=稳定相对力度；和≤0 退回 max 模式） |

### 3.6 其余 modules 节点

| 键路径 | 示例 | 缺省 | 加载点 |
|---|---|---|---|
| selfTradeCalibrator.toxicityWindowMs/adverseThreshold/minSamples/moveThresholdTicks | 5000/0.6/5/1.0 | 同左 | SelfTradeCalibrator.h:131-134 |
| selfTradeCalibrator.retreatTicks | 3 | **2.0** | 成交后退让 ticks（FillRetreatPolicy 消费；与 improveRetreatRatio 无关） |
| selfTradeCalibrator.retreatCooldownMs | 5000 | **3000** | 退让冷却 ms |
| selfTradePrevention.enabled | true | true | STP 开关唯一权威（arb=true 强制 ON） |
| selfTradePrevention.stpMinPriceGap | 1.0 | 1.0 | 最小价差 gap(ticks)(H7 键名统一) |
| selfTradePrevention.allowSamePrice | false | false | 是否允许同价对冲 |
| selfTradePrevention.priceAdjustTicks | 1.0 | 1.0 | 价格调整 ticks |
| autoCancel.maxAgeMs | 10000 | 10000 | 订单最大存活 ms→进入 auto-cancel 流程。热参无此键（结构参数不热更） |
| autoCancel.staleExtensionTicks | 2.0 | 2.0 | **B1 新键**：STALE 延寿价格偏离阈值(ticks)——偏离≤此值的挂单不被 auto-cancel 判死（原 sticky×2 隐式值显式化；旧 priceDeviation 键删除=死接口清理） |
| autoCancel.cancelRetryIntervalMs | 300 | 300 | B+ 撤单 ack 超时重发间隔 ms |
| autoCancel.cancelMaxRetries | 3 | 3 | 最大重试次数，达到→IS_ZOMBIE 升级 |
| correlationManager.windowSize/minCorrelation/spreadZThreshold | 100/0.5/2.0 | 同左 | CorrelationManager.h:57-59 |

---

## 4. 热更新配置 hotparams.yaml（27 参数）

**唯一的热更文件**（实盘 watcher 1Hz；回测完全不生效）。snake_case 键名。

### 4.1 参数总表

| 键 | dist 值 | 注册默认来源 | 热路径边界[lo,hi] | 应用目标 |
|---|---|---|---|---|
| base_spread | 2.0 | config.quoting.baseSpread | [0.5,20] | GLFTParams + FutuQuoter |
| base_qty | 10.0 | quoting.baseQty | [1e-6,100] | FutuQuoter |
| level_qty_multiplier | 0.7 | quoting.qtyDecay | [0,1000](宽松,warn走复查) | FutuQuoter |
| level_step | 1.0 | quoting.levelStep | [1e-6,100] | FutuQuoter |
| max_delta | 30 | portfolio.maxDelta | [1e-6,1e8] | Portfolio.portfolio_max_delta + Coordinator |
| **contract_max_delta** | 30 | anchor 合约 ci.max_delta（B2） | [1e-6,1e8] | Portfolio.setContractMaxDelta **全部合约**（差异化需重启） |
| alpha_sensitivity | 2.0 | coordinator.pipeline | [0,1000] | Coordinator.setAlphaSensitivity |
| ofi_weight | 0.35 | signalAggregator 配置 | [0,10] | aggregator->updateWeights |
| trade_weight | 0.25 | ↑ | [0,10] | ↑ |
| book_imbalance_weight | 0.20 | ↑ | [0,10] | ↑ |
| momentum_weight | 0.15 | ↑ | [0,10] | ↑（五权和≈1 复查 warn） |
| lead_lag_weight | 0.05 | ↑ | [0,10] | ↑ |
| strong_threshold | 0.7 | model.strongThreshold | [0,1] | updateWeights |
| confidence_weight_min | 0.3 | GLFTParams | [0,1] | GLFTParams |
| confidence_weight_max | 1.0 | GLFTParams | [0,1] | GLFTParams |
| phi | 0.20 | GLFTParams | [0.01,2.0] | GLFTParams |
| delta_skew_threshold | 0.3 | GLFTParams | [0,0.9] | GLFTParams |
| delta_skew_factor | 1.5 | GLFTParams | [0,100] | GLFTParams |
| max_spread_mult | 3.0 | GLFTParams | [0,100] | GLFTParams |
| min_spread_mult | 1.0 | GLFTParams | [0,100] | GLFTParams |
| depth_sensitivity | 0.5 | GLFTParams | [0,1000] | GLFTParams |
| toxicity_spread_factor | 1.0 | GLFTParams | [0,100] | GLFTParams |
| low_confidence_spread_factor | 2.0 | GLFTParams | [0,100] | GLFTParams |
| sticky_threshold | 1.0 | quoting.stickyThreshold | [0.01,10]（0=churn 防呆拒收） | quoter->updateStickyParams |
| improve_retreat_ratio | 2.0 | quoting.improveRetreatRatio | [0,1000] | 同上 |
| protect_ticks | 1.0 | quoting.protectTicks | [0.5,1e4]（<半 tick 拒收；关闭走 price_protection 开关） | quoter->updateProtectionParams |
| max_price_deviation | 20.0 | quoting.maxPriceDeviation | [0,1e6] | quoter->updateMaxPriceDeviation |

### 4.2 热更新链路专节

```
生效时机轴（实盘）:
  T0 进程启动 ── initial applyAll: 共享内存残留旧值 → 盖到 config/coordinator 初始化值上
                 （"重启不丢上次热更结果"; UftFutuMmStrategy.cpp:383-395）
  T0+ε        ── logDriftSummary 无条件打印 27 键 文件值 vs 注册默认 差异
                 （回测也打印! 差异键=回测/实盘行为分叉点）
  T0~1s       ── watcher 首轮: hotparams.yaml 解析值写入共享内存 → 置 pending
  下一 tick   ── on_tick 内 drain(_cb_mtx 内): consumePendingApply→applyAll
  此后每秒    ── watcher 全量 parse+值比对(mtime 门控已废——秒粒度丢修改);
                 变更才写共享内存+置脏; watcher 线程绝不 applyAll

稳态权威链:  hotparams.yaml > 共享内存残留 > config/coordinator
回测:        watcher 不跑、共享内存热参不生效 ⇒ 回测吃的是 config/coordinator 值!
             ⇒ 排障先看 [HOTPARAM-DRIFT] 行, 别看错权威文件

parse 校验规则 (FutuHotParamManager.cpp:242-338):
  文件不可载 → 整体失败(本轮跳过)
  "abc"/"true"/空串 → strtod 全串校验拒收该键 warn(atof 会静默变0, 故显式校验)
  NaN/inf/越界(bounds 表) → 拒收该键保留旧值 warn
  未知键 → 忽略(只遍历 27 个注册名)
  合法变更 → 写共享内存 + old->new 审计日志 + 变更计数

applyAll 应用顺序 (:74-203):
  GLFTParams → quoter 四参数 → SignalAggregator 权重 → alpha_sensitivity
  → Portfolio max_delta/contract_max_delta(B2) → sticky/protection/max_price_deviation
  → crossCheckIssues(warn 级 [HOTPARAM-CHECK], 五项一致性检查, 不阻断)

crossCheckIssues 五检查项 (:349-444):
  ① 五路权重和偏离 1.0 超 ±0.1          ② 满挂深度 < obligationMinQty
  ③ portfolio_max_delta > 任一合约硬顶   ③b contract_max_delta > 硬顶
  ④ min_spread_mult > max_spread_mult    ⑤ confidence_weight_min > max
```

### 4.3 热更注意事项

1. **改文件即生效，但下一 tick 才应用**（drain 机制），且仅实盘。
2. 单键拒收不影响其它键（逐键独立校验保留旧值）。
3. `contract_max_delta` 应用到**全部合约**同一值；需要 per-contract 差异只能重启改 yaml。
4. 结构性参数不在热参表（autoCancel/maxOrders/orderErrorThreshold 等）——改需重启。
5. 共享内存按**名字**注册寻址，27 键增删天然兼容旧布局。
6. 回测想验证某组参数 → 直接改主配置的对应键（coordinator.yaml 或 config yaml），
   并保持 dist/hotparams.yaml 与之同步以免 drift 告警误导。

---

## 5. 日志配置 logcfgbt.yaml（WTSLogger.cpp:135-201）

`dyn_pattern` 节下每个子键为一个 logger 类别：

| 键 | 说明 |
|---|---|
| dyn_pattern.strategy / root | logger 名模板："root"=全局 Runner 日志；"strategy" 含 %s 替换(:149/:158) |
| *.async | false=同步 spdlog；true=async_logger(线程池 8192,2) |
| *.level | debug/info/warn/err/critical/off |
| sinks[].type | basic_file_sink/daily_file_sink/console_sink/ostream_sink |
| sinks[].filename | outputs/Strategy_%s.log、outputs/Runner.log（%s=logger 名；自动建目录） |
| sinks[].pattern | spdlog 格式串，如 '[%Y.%m.%d %H:%M:%S - %-5l] %v' |
| sinks[].truncate | basic_file_sink 覆盖写 |

> **排障要点**：策略类 debug 日志（[TOXIC]/[SIGNAL_DECOMP]/[QUOTE]/[HOTPARAM-*]）落
> **outputs/Runner.log**（root logger）；outputs/Strategy_uft.log 只含引擎/订单级行。

---

## 6. 套利配置 spread_arbitrage.yaml（SpreadArbitrageManagerInit.cpp:18-213）

| 键路径 | 说明 |
|---|---|
| enabled | 总开关 |
| enhanceMarketMaking | MM 增强（历史遗留，R3 清理后仅观测模式） |
| primaryStrategy | 主策略标识 |
| maxTotalPosition / maxPairs | 组合限制 |
| minSignalConfidence | **单键双层统一**：Manager 放行闸 + executor 丢弃阈（R3 统一，dist=0.5） |
| signalCooldownMs | 信号冷却 |
| minProfitThresholdTicks | 平仓利润阈值(ticks) |
| arb_close.enabled | 平仓总开关（**出厂 disabled——回测零覆盖来源**，灰度前需专项验证） |
| arb_close.allow_signals.* | 允许触发平仓的信号类型 |
| arb_close.stop_loss_policy.order_flag + timeout_ms | 止损单标记+超时 |
| arb_close.timeout_policy / close_in_flight_timeout_ms | 超时平仓策略/in_flight 超时(A5 replay 时钟后可复现) |
| arb_close.max_close_size_pct / oversold_protection / overshoot_cooldown_ms | 尺寸上限/过卖保护/过冲冷却(B-5) |
| arb_close.intent_broadcast | intent 广播开关 |
| pairs[].id / leg1 / leg2 | pair 定义（三段式 stdCode） |
| pairs[].ratio / ratio2 | 价差比率 |
| pairs[].leg1Multiplier / leg2Multiplier | **腿乘数（R4b A-1 修复死亡链后真实生效**，默认 1.0） |
| pairs[].maxPosition | 单 pair 持仓上限 |
| pairs[].entryZScore / exitZScore / stopLossZ | z-score 进出场/止损 |
| pairs[].windowSize | 统计窗口 |
| pairs[].stopLossPct / maxTrendBars / addSafetyRatio | 百分比止损/趋势条数上限/加仓安全比 |
| risk_limits.portfolioStopLoss | 组合止损线（EMERGENCY→IRREVERSIBLE halt A8） |
| risk_limits.maxTotalPosition / maxSinglePair | 组合/单 pair 上限 |
| risk_limits.maxCorrelationBreak / maxDivergenceZscore / maxDivergenceTime | 相关性断裂/发散容忍 |
| statistical.meanReversion + trendFollowing | 两族统计策略参数 |

---

## 7. 死键清单（配了也不生效）

| 键 | 位置 | 状态 |
|---|---|---|
| `modules:` 整节 + 根级 `stpMinPriceGap` | 主配置 yaml | **零读取**——开关/毒性/STP 权威全在 coordinator.yaml（V8-R6 起） |
| portfolio.hedgeDeltaThreshold / hedgeCooldownMs | 主配置 | loader 无读取点 |
| quoting.alwaysObligation | 主配置 | 无消费者 |
| useHedging（注释提及） | coordinator 根级 | 代码无读取点（Assembler coordBool 仅根级 Hedging 有） |
| _ec_5d 内嵌 modules.toxicityDetector.adverseThreshold=0.14/vpinThreshold=0.07 | _ec_5d.yaml | **不生效**！实际 0.75/0.60 来自 coordinator.yaml——排查毒性阈值勿看错文件 |
| env.slippage | 主配置(env) | 仅 cta/sel mocker 用，UFT 忽略 |

## 8. 易错点与排障速查

| 症状 | 先查 |
|---|---|
| 回测与实盘行为不一致 | `[HOTPARAM-DRIFT]` 行（Runner.log）——差异键即分叉点；回测不吃热参 |
| 改了 hotparams.yaml 没效果 | 是回测吗？（watcher 不跑）/ 看共享内存残留优先级 / 看 parse 拒收 warn |
| 毒性太敏感/太钝 | coordinator.yaml 的 adverseThreshold/vpinThreshold（不是主配置内嵌节！） |
| 报价不动 | qphase 状态（getPhaseStr）、TOXICITY cooloff、vol EXTREME pause、spread_mult≥max×0.9 pause、义务深度不足 force_obligation |
| 单被撤又挂反复 | sticky_threshold/improve_retreat_ratio 顶单黏性 vs staleExtensionTicks 延寿两个机制区分 |
| HALT 后不恢复 | IRREVERSIBLE?（daily-loss/arb EMERGENCY）/ maxRecoveryCount 耗尽?/ cooldown 未过?/ D1 后下单错误只走 ERROR 自探恢复（10s×2^n≤60s） |
| 撤单"消失"但交易所还在 | zombie 升级链（cancelRetryIntervalMs×cancelMaxRetries→IS_ZOMBIE→闩锁+stra_cancel_all fullCode 兜底） |
| CSV 对比不上昨天 | **不存在逐比特判据**（splitVolume srand(time(NULL))）——用统计带 ±1.5% + 健康度指标 |

---
*文档生成：2026-08-24③，基于当时 HEAD 行号锚点；后续提交可能漂移，以方法名检索为准。*
