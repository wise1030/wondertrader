# 全面审阅分析报告（2026-11-25）

> 范围：本轮（11-24/25）全部未提交改动、系统架构与功能接线、低延迟高并发适配性、quantbox 可借鉴点、冗余与优化空间。**仅分析，未改代码。**
>
> 证据：git diff（17 文件，+1225/-336）、`~/build/wtn_rel2` 全量构建与 9 个测试二进制（全 rc=0）、生产日志（Runner_2026-11-24/25）、quantbox 原版（`quantbox/strategy/optioncore`、`optiontrader`）。

---

## 0. 执行摘要

- **当前线上状态**：v4.1 部署（15:36，PID 3131341）健康，通道 ready、guards reset、无未知键、无 PriceLimit REJECT、无 panic；v3/v4 全部核心机制（risk shift、方向性抑制、期货兜底、REJECT-ONLY 释放、PositionGuard reset、teenie、可配置 panic 信号）已上线。
- **未提交改动总体判断**：主要功能正确且测试齐全，但有 **3 个需要后续跟进的真实问题**（§1.3：配置 patch 曾吞 `pricer:` 段头（已修）、vega shift 量纲（已缓解未根治）、`_riskSeverity` 现为空转计算）。
- **性能结论**：批处理 p50 中位 **8μs**、p95 中位 **37μs**、p99 中位 **65μs**，**满足低延迟高并发做市要求**；但 p90 尾部存在 **20–70ms 周期性尖峰**（SLOW 全量重算/压力测试/拟合），需要把这些重活从热路径挪走（§3.2）。
- **quantbox 借鉴**：原版 panic 语义（已对齐）、二次型 risk shift（已接通）是本轮最大收获；下一步最值得借的是 **GARCH 定价器（OptionPricer3）与盘口深度（IBook）**，以及扫描器族（§5）。
- **冗余**：残留死代码已清（updateOurMarketSide/Future）；仍有 `_positions` 与 `PositionBook` 双份持仓视图（legacy 兼容）、PeriodicCurveFitter 同步备用路径、`riskShiftDelta/Vega` 曾死通道（已启用）、`risk_prem_fut/rp_quote` 等未使用配置项。

---

## 1. 未提交改动逐项审阅

### 1.1 文件清单与判定

| 文件 | 变更 | 判定 | 备注 |
|------|------|------|------|
| `src/TraderCTP/TraderCTP.{h,cpp}` | 曾加 watchdog，后全部还原 | ✅ 干净 | 与 HEAD 一致，已重编 |
| `CompositeOptionPricer.{h,cpp}` | L3 shift、stickyScale、teenie、size shrink、量纲参数、panic 退避、fit 门控、删除死函数 | ⚠️ 有 2 处后续事项 | §1.3.1/§1.3.2 |
| `OptionPricer2.cpp` | fit 未就绪不算 theo（valuesReady 门控） | ✅ 安全 | 已验证无鸡生蛋（拟合走 IV 独立 pass） |
| `OptionQuoteManager.{h,cpp}` | REJECT-ONLY 300s 释放、max_side_orders 强制执行、activeSideCount | ✅ 安全 | 串行换价是消除多层挂单的必要代价 |
| `HftOptionStrategy.{h,cpp}` | guard 快照边界/reset、时段门控、channel_ready 重置、空 ids 退避、bail 分级、failStreak 告警、可配置 panic 开关、风险严重度 | ⚠️ `_riskSeverity` 空转 | §1.3.3 |
| `OptionHedgeCoordinator.{h,cpp}` | bail 阈值/目标位接线 | ✅ 安全 | multi-leg 与 shared residual 目标一致 |
| `PositionGuard.{h,cpp}` | `reset()` + `onFill` 未初始化忽略 | ✅ 安全 | 已消除 reconcile 误初始化 |
| `UiSnapshotBuilder.cpp` | theo 仅在 activeValues().isPriced() 时输出 | ✅ 安全 | 防陈旧理论价 |
| `QuoteRiskHelpers.h`（新） | shift/size/teenie/sticky/reducing/backoff 纯函数 | ✅ 安全 | 单测覆盖（含方向语义） |
| `ConfigValidator.h` | 新键白名单 + `panic` 段 | ✅ 安全 | 已消除 UNKNOWN key 误报 |
| `tests/test_enhancements.cpp` | readiness gate、撤单 ack 回灌、risk helpers 测试 | ✅ 安全 | 与生产语义同步 |
| `tests/test_uibridge.cpp` | makeLeg 补 `setPriced(true)` | ✅ 安全 | 对齐新 theo 门控 |
| `dist/WtRunnerOption/config.yaml` | risk_prem、L3、teenie、bail、panic_fail、signals.risk、panic 段 | ⚠️ 曾吞段头，已修并同步 | §1.3.1 |
| `analysis/*`（诊断/方案/对照报告） | 文档 | ✅ | 与实施一致 |

### 1.2 测试与构建证据

- 原生盘全量构建 rc=0（含头文件改动后全量重编）；9/9 测试二进制通过（`test_enhancements`、`test_uibridge` 等），测试与新 `.so` ABI 同步。
- 新增纯函数测试：`quadraticShift`（含 clamp）、`riskSizeScale`（方向与 vega 双维）、`teenieCapPrice`、`stickyScaleFor`、`reducesDelta/Vega`、`isReducingOrder`、`escalateBlackout`；readiness gate 改为 pre-fit 零理论价语义。

### 1.3 需要跟进的真实问题

#### 1.3.1 配置事故（已修，需流程固化）
- 首次 patch 误吞 `pricer:` 段头，导致 9 个 risk-shift 参数落入 `delta_hedge` 段成死键，15:05 前 pricer 实际全默认。已修复（15:08），并验证 0 未知键。
- **后续要求**：config 变更后必须启动 grep `UNKNOWN key|ConfigValidator`；最好把生产 config 的 diff 也纳入每次部署检查单。

#### 1.3.2 vega shift 量纲（缓解未根治）
- 现状：`vega_adj = shift_vega × vega_leg`，`shift_max_vega=0.05` → 平移约 5–8 元，已可控；但 `shift(元) × vega(元/1%vol)` 量纲不纯是设计缺陷。
- **建议（下版本）**：vega 偏移改为 vol 空间定义 `adj = shift_vol(%vol) × vega_leg`，或把 distort/max 参数直接定义为"最终元平移量/leg greek"避免二次放大；当前参数可用，勿再放大。

#### 1.3.3 `_riskSeverity` 空转
- v4 移除 runawayBreach 后，`_riskSeverity` 仍每批计算但无消费方。无害，建议接入 UI 快照（风险档位显示）或删除以省少量 CPU。

---

## 2. 系统架构与功能接线核查

### 2.1 主链路（已验证工作）

```
ParserCTP → HftStraBaseCtx.on_tick → OptionAsyncEventProcessor（MD 合并/控制事件不丢）
  → OptionGrid.onTick(updateMarket/动态发现)
  → on_batch_complete:
      checkRiskLimitsEx / checkMarginLimits（ungated）
      checkDeltaHedge（时段+状态门控）
      panic 评估（pricerPanic/checkerPanic；failStreak 仅告警）
      enabled && marketFresh && shouldCompute:
          CompositeOptionPricer FAST（isValuesReady 门控）
          updateRiskShiftsDelta/Vega + stickyScale
          CTG.refresh → pendingQuotes → TPS → drain
          OQM.updateOrders（max_side_orders/REJECT-ONLY/cancel-stuck/guard）
          executeQuoteIds/executeOrderIds（R1 价格限位 + 空 ids 退避 + reducing 计数）
          TraderAdapter → CTP
```

### 2.2 模块接线矩阵

| 模块 | 状态 | 说明 |
|------|------|------|
| OptionAsyncEventProcessor | ✅ 在用 | MD tick 按 code 合并；控制/会话/通道事件不丢 |
| OptionGrid / ExpiryLifecycle | ✅ 在用 | 动态发现、到期剔除、session begin purge |
| CompositeOptionPricer | ✅ 在用 | FAST/SLOW；fit 门控已生效 |
| OptionPricer2 + AsyncVolFitter | ✅ 在用 | 异步拟合、曲线发布、readiness 联动 |
| PeriodicCurveFitter（sync） | ⚪ 备用路径 | `fitter.mode: sync` 才用；非冗余，可保留 |
| ControllableTradingGrid | ✅ 在用 | refresh/rank/TPS/drain；panicClear 期货保留 |
| OptionTradingGrid/UTD | ✅ 在用 | 期货腿 OQM 与底层数据 |
| OptionQuoteManager | ✅ 在用 | 每合约订单状态机；max_side_orders 生效 |
| PositionGuard | ✅ 在用 | short 边喂入 + reset/reconcile/onFill 防御 |
| OptionRisk / OptionExpiryGreeks | ✅ 在用 | 组合 greeks、逐月 delta |
| RiskLimitsEx / MarginGuard | ✅ 在用 | REJECT latch（现仅告警+UI，不直接 Panic） |
| RiskSignals（Toxicity/PnlLimit） | ✅ 在用 | **config `signals.risk[]` 驱动，今日已加载** |
| FillPriceChecker | ✅ 在用 | 有 `panic.enable_fill_price_checker` 开关 |
| DeltaHedgeExecutor/Coordinator | ✅ 在用 | bail 分级、in-flight、reject backoff、目标位 |
| PositionOffsetMgr / RiskFilterChain | ✅ 在用 | 偏移/过滤链 |
| IssuedOrderRegistry / QuoteStatistics | ✅ 在用 | 订单归因、报价统计 |
| OptionScanCoordinator | ⚪ 无扫描器配置 | panic stop/start 已接线；无扫描器即空转，合理 |
| ExpirationSimulator / AttributePublisher / AnalysisRecorder | ✅ 在用 | session end/属性/分析 |
| UiBridge + middleware + REST `/api/param` | ✅ 在用 | 快照、热参、审计、4MB 行缓冲 |

**未接入/可选项**：OptionPricer3（GARCH）、盘口深度 IBook、更多扫描器、alpha 信号的 config 化（`signals.alpha[]`）。

### 2.3 明确的非问题（避免误判）

- `OptionRisk::setHedgePosition: unknown hedge code`：仅 debug 日志且**不创建幽灵持仓**（未找到即返回），无会计错误；但应过滤期权代码，避免日志噪声（§4.2）。
- `OptionPricer2::computeImpliedValues` 与 bootstrap median IV 在 `initValuesCompute` 中，未被 fit 门控阻断。
- `values(0)` 与 `activeValues()` 双缓冲语义已正确处理。

---

## 3. 低延迟高并发适配性

### 3.1 实测数据（11-25 全天 2466 个采样批次）

| 指标 | 中位 | p90 | 最大 |
|------|------|-----|------|
| batch p50 | **8 μs** | 19.8 ms | 21.0 ms |
| batch p95 | **37 μs** | 68.2 ms | 72.5 ms |
| batch p99 | **65 μs** | 68.2 ms | 72.5 ms |
| batch p99.9 | **150 μs** | 68.2 ms | 72.5 ms |

**判定**：中位与常规分位极低，完全满足低延迟高并发要求（150 活跃合约 + 418 订阅）；异步事件合并避免了 MD 洪峰时队列失控。

### 3.2 尾部尖峰归因与优化建议

p90 的 20–70ms 尖峰与以下周期任务同频：
1. `slow_compute_interval=10s` 的 **SLOW 全量重算**（全部 strike theo/greeks/ourMarket）。
2. 每 60s **StressScenario**（全组合情景压力测试）。
3. 每 60s **AsyncVolFitter 快照提交/发布**（提交本身轻，但发布时全量重标定）。

**建议（优先级高）**：
- SLOW 全量重算改为 **underlying-driven/debounce** 或后台线程（拟合已异步，SLOW 可照搬 SingleFlightTaskRunner 模式）；
- 压力测试移出 worker 热路径（独立低频线程或按事件采样）；
- 若启用 OpenMP（`use_parallel` 当前 false），先 replay 验证再考虑；当前单 worker 已足够中位指标，不建议为尾部分位冒线程风险。

### 3.3 并发与状态一致性

- 热路径主要状态（grid/CTG/OQM/registry/PositionBook）均在 **async worker 单线程** 内；CTP 回调统一入队；UiBridge 独立 IO；热参写也回 worker。无新增竞态。
- `OptionQuoteManager::activeSideCount` 的串行换价是业务需要（禁多层），不是性能缺陷；其代价是每次换价多一个撤单 RTT，建议保留。

---

## 4. 进一步优化空间（按优先级）

### 4.1 P0（正确性/运营）
1. **流程**：config patch 自动校验（UNKNOWN key、pricer 段完整性、必填段存在）；部署 checklist 固化。
2. **`_riskSeverity` 接 UI 或删除**（当前空转）。
3. **日志降噪**：`OptionRisk::setHedgePosition` 仅在期货/对冲代码时调用；当前每次持仓回调对全部期权代码打 debug，属于无意义 IO。
4. **hot_params.json**：`tdate=0` 且残留 `targ_delta:202612` 等已到期键——session begin 应按当前交易日清理/重写，避免陈旧键回放。

### 4.2 P1（性能/行为）
5. SLOW/Stress/拟合发布移出热路径（§3.2）。
6. `OptionPricer2::computeValue` 的 fit 门控与 SLOW 路径一致化已做；可进一步让 **SLOW 只在 fit 发布后或标的变化时**触发。
7. **参数面板**：`params.schema` 仍含 5 个 `wgt_*` alpha 权重——按既定要求（alpha 权重不进 UI 热参面板）应移除，热参仅保留全局报价参数与逐月 targ/sprd。

### 4.3 P2（结构）
8. `_positions`（legacy net projection）与 `PositionBook` 双视图：评估收敛到 PositionBook 单源（当前 `_positions` 仅供旧 PnL/风险路径）。
9. `risk_prem_fut`、`rp_quote` 等未使用配置项：标注 deprecated 或移除，减少配置面。
10. `max_risk_d/max_risk_v` 不移植（RiskLimitsEx 已覆盖），保留结论。

---

## 5. quantbox 可借鉴点（按价值排序）

| 候选 | 原版位置 | 价值/理由 | 建议 |
|------|----------|-----------|------|
| **GARCH 定价器（OptionPricer3/GarchModel/GarchState）** | `strategy/optioncore`（GarchModelParams_autogen 等） | 当前 Black+GVV 在 thin/抖动盘有 bootstrap 尺度爬行历史（11-18 记录）；GARCH 提供状态化波动率跟踪，可能减少拟合 drought 与翼部失真 | **首选评估**：与 GVV 做 replay A/B，先离线不改主链 |
| **盘口深度 IBook** | 原版 sticky/teenie/queue-pos 依赖 `getBook()` | 现版 teenie 只用 best bid/ask；sticky_force/更准 queue position/市场有效性判定需要深度 | 二期：只读接入，先用于 teenie/market freshness |
| **扫描器族**（Butterfly/Garch/LowBids/MM/Open/Simplex/StrikeSpread） | `optiontrader/*Scanner*` | 可用组合单（Spread/Syn/SimulComboOrder）做 vega/delta 的结构性降险，补齐"期权组合降险"工具箱 | 二期按业务选 1–2 个（Butterfly/StrikeSpread） |
| **alpha 信号 config 化**（`signals.alpha[]`） | `setupSignals` 已支持 | 现 wgt_* 顶层硬编码权重；可按 signal enable/params 配置，便于灰度与归因 | 小改动，纳入 P2 |
| **max_risk_d/max_risk_v** | `CompositeOptionPricerParams.xml` | alpha 侧硬限，与 RiskLimitsEx 语义重叠 | **不移植**（避免双限位） |
| **risk_tolerances 完整表（distort/shift_max per greek）** | 原版 lua 表 | L3 二次型参数原型；已以标量+热参实现等价能力 | 已完成借鉴，无需再引 lua |
| **GarchScanner/波动率扫描** | `optiontrader/GarchScanner` | 与 GARCH 定价配套 | 随 GARCH 评估决定 |

---

## 6. 冗余与清理清单

| 项 | 处理建议 |
|------|----------|
| `updateOurMarketSide` / `…Future`（死函数） | ✅ 已删除（本轮） |
| `_positions` vs `PositionBook` 双持仓视图 | 评估收敛（P2） |
| `PeriodicCurveFitter` 同步备用路径 | 保留（backtest/调试有用），非冗余 |
| `risk_prem_fut`、`rp_quote` 等未用配置 | 标 deprecated/移除（P2） |
| `riskShiftDelta/Vega` 曾死通道 | ✅ 已接通 L3，不再冗余 |
| `params.schema` 中 `wgt_*` | 移除出热参面板（P1） |
| `OptionRisk::setHedgePosition` 期权代码 debug | 过滤（P0） |
| `_riskSeverity` | 接 UI 或删（P0） |

---

## 7. 结论

1. **功能与架构**：主链路完整、模块基本全部接入；当前未发现新的致命实现错误。量化/定价/风控/订单/状态机/UI 六层职责清晰。
2. **低延迟高并发**：**满足要求**（中位微秒级），尾部周期性重活是唯一性能短板，且可通过调度优化而非架构重写解决。
3. **quantbox 借鉴**：本轮已完成 panic 语义、二次型 risk shift、teenie、sticky/targ 对齐；下一步建议 **GARCH 定价器 + IBook 深度 + 扫描器族** 的离线评估。
4. **待办优先级**：SLOW/Stress 挪出热路径（P1 性能）、hot_params 清理与 schema 去 wgt（P1）、`_riskSeverity`/日志降噪（P0）、vega shift 迁移 vol 空间（下版本正确性）、GARCH/IBook 离线评估（P2）。

**未改任何代码；报告中的优化项均为后续授权候选。**
