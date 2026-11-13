# WtFutuCore 五维度全面审计报告（2026-11-12）

> 审计对象：`src/WtFutuCore/`（期货做市双策略 ao2611/12 + ec2611/12 单进程生产模块）。
> 审计方法：一手精读（主会话逐文件）+ 四份子任务报告（①架构与依赖映射 / ②代码质量细查 / ③测试与文档完备性 / ④交易链路与风控纵深）合并。
> 证据基线：全部结论附 `file:line` 证据；四素材经交叉验证——一手精读 7/7 全命中，①号报告两批抽验 18+ 项全命中（唯一轻微偏差：`_halt_domain_lock` 声明实为 FutuRiskMonitor.h:612，①引用的 :450-482 为使用点区域，不影响采信）；HaltReason.h:95 与 MarketDataContext 位置两未决点直接核验通过。②③④按 7/7 命中基线采信。
> 审计规模：①号盘点时点顶层 90 文件 / 25,567 行源码（不含 signals/、arb/ 子目录；全模块含子目录 128 文件 / 38,323 行，复核补计）+ 文档 8,992 行（复核时点 10,435 行，审计期有新增，含本报告）。

---

## 维度一：架构设计（素材：①号报告 + 一手精读）

### 1.1 分层结构

五层架构自上而下（①号映射，include 关系一手复核）：

| 层 | 组件 | 证据 |
|---|---|---|
| L1 策略顶层 | UftFutuMmStrategy（实现 UFT 策略接口，直连协调与风控组件） | UftFutuMmStrategy.h:16-29（include SpreadOptimizer/TradingState/FutuRiskMonitor/RiskLiquidator/CloseoutOrchestrator/ArbExecutionBridge/MonitorBridge 等） |
| L2 协调层 | StrategyCoordinator（组合根）、RiskCoordinator、SessionPhaseManager、FutuModuleAssembler（装配） | StrategyCoordinator.h:7-31（include ApplicationDeps/ZombieLatchPolicy/RiskCoordinator/SessionPhaseManager/FutuPortfolio/QuotePolicyChain/EventDispatcher） |
| L3 风控/状态层 | FutuRiskMonitor、TradingState、RiskLiquidator、CloseoutOrchestrator、SelfTradeCalibrator | FutuRiskMonitor.h:612（`_halt_domain_lock` 声明）；TradingState.h:14-32 |
| L4 执行/簿记叶子层 | FutuQuoter、OrderRouter、UnifiedOrderTracker、FutuPortfolio 等 | FutuQuoter.h:344、OrderRouter.h:250、UnifiedOrderTracker.h:666（叶子锁） |
| L5 运行 IO/适配层 | FutuRuntimeOps、FutuEnginePort、ArbExecutionBridge、TdSpiOffload、signals/（MarketDataContext） | FutuEnginePort.h:23/:27；signals/MarketDataContext.h/.cpp（位置已直接核验） |

### 1.2 依赖面

- 外部依赖 TOP3 全部指向框架公共层：WTSLogger.h、FasterDefs.h、TimeUtils.hpp（①号 grep 计数 31/17/13；复核含 `../../` 双相对深度口径为 41/16/13，TOP3 排序不变）。
- `.h include .cpp` 零匹配——头文件洁癖良好；唯一漏网直引为 FutuDataDefs.h:11（`#include "../WtUftCore/UftDataDefs.h"`，绕过 FutuEnginePort 边界直引框架头文件；该文件全仓零引用方，孤儿属性复核实锤）。
- 显式线程仅 2 处：FutuHotParamWatcher.cpp:61、arb/AsyncArbitrageExecutor.cpp:65。其余全部单线程回调驱动，并发面极小。
- 引擎边界收敛于 FutuEnginePort.h:23/:27——策略层不直触引擎内部，经端口接口对接。

### 1.3 并发模型与锁层级

三级锁层级（①号 grep 14 处叶子锁 + 一手复核）：

- **L0** `_cb_mtx`（UftFutuMmStrategy.h:508）——顶层回调互斥，全局串行点。
- **L1** `_halt_domain_lock`（FutuRiskMonitor.h:612）——风控域锁。
- **L2** 叶子 RecursiveSpinLock 群（14 处）：UnifiedOrderTracker.h:666、StrategyCoordinator.h:472、SelfTradeCalibrator.h:235、OrderRouter.h:250、FutuQuoter.h:344、FutuPortfolio.h:459 等。

锁序单向（L0→L1→L2），未发现反向获取；TradingState.h:14-32 以成文 THREADING CONTRACT 固化该契约。

### 1.4 配置流

配置单源化：装配期统一解析注入，运行期无二次读盘；热参数走 FutuHotParamWatcher 独立线程。配置读取的"三套平行实现"问题见维度三 P1（规范版 FutuConfig.cpp:16-111）。

### 1.5 亮点

**①号五亮点：**
1. **wireDeps + validateDeps fail-fast**——依赖显式接线 + 启动期校验，缺依赖拒绝启动。
2. **并发面极简**——仅 2 显式线程（FutuHotParamWatcher.cpp:61、arb/AsyncArbitrageExecutor.cpp:65），其余单线程回调驱动。
3. **FutuEnginePort 边界**（FutuEnginePort.h:23/:27）——策略层与引擎解耦干净。
4. **配置流单源化**——装配期单点解析，无双读路径。
5. **运行期自检**——config validation + 依赖校验构成启动门。

**一手精读四亮点：**
1. **成交单写入点**：FutuRuntimeOps.processTradeFill :36-407 为成交唯一权威入口，账务状态机单点收敛，无平行记账路径。
2. **恢复决策集中化**：FutuRiskMonitor canRecover 七道闸（FutuRiskMonitor.cpp:545-658）+ closeout 状态唯一权威（:959-1192）， halt/lift/closeout 决策不散落。
3. **线程契约成文**：TradingState.h:14-32 THREADING CONTRACT 将锁层级写成显式契约，mirrorQPhase :224-240 相位语义有据可查。
4. **防双卖互斥设计**：RiskCoordinator.cpp:265-274 forceFlat 与 closeout defer 互斥，强平与了结链不会同时对同一持仓双卖。

### 1.6 架构风险（①号五项，按严重度）

| # | 风险 | 严重度 | 证据 |
|---|---|---|---|
| R1 | `HaltReasons::_bits` 进程级 static 位图被双策略共享 | **高** | HaltReason.h:95（`static inline std::atomic<uint64_t> _bits{0};`，已直接复读确认）；生产实证 11-06 ec halt 置位全局 RISK_HALT → ao 影子对比全天 RISK_HALT-only 分歧 17 条，分歧数与 halt 次数逐日正相关（11-02:2/11-03:2/11-04:8/11-05:10/11-06:17） |
| R2 | `_cb_mtx` 全局串行——顶层回调互斥使所有策略回调过单点 | **高** | UftFutuMmStrategy.h:508 |
| R3 | FutuDataDefs.h:11 绕过引擎端口边界直引框架头文件；且全仓零 include（孤儿） | 中 | FutuDataDefs.h:11（`#include "../WtUftCore/UftDataDefs.h"`，复核确认） |
| R4 | 退出清理证据缺口 | 中 | 一手精读：退出路径清理次序无日志/测试佐证 |
| R5 | arb 完成度欠账 | 低 | AsyncArbitrageExecutor 魔数（见维度三 P2） |

---

## 维度二：功能实现与交易链路（素材：④号报告 + 生产实证）

### 2.1 风控纵深：九层

L0 竞价门 → … → L8 closeout 三组件（CloseoutOrchestrator/CloseoutExecutor/RiskLiquidator）→ L9 taker 兜底。逐层独立触发、互为后备，强平链路（RiskLiquidator）与了结链路（closeout）经 defer 互斥（RiskCoordinator.cpp:265-274）防双卖。

### 2.2 链路关键权威点

| 环节 | 唯一权威 | 证据 |
|---|---|---|
| 成交回报入账 | FutuRuntimeOps::processTradeFill | FutuRuntimeOps.cpp:36-407 |
| 委托状态机 | FutuRuntimeOps::onOrderEvent | FutuRuntimeOps.cpp:931-1052 |
| 通道失效/恢复 | onChannelLost / onChannelReady | FutuRuntimeOps.cpp:1054-1102 / :409 |
| halt/lift 决策 | FutuRiskMonitor::canRecover 七道闸 | FutuRiskMonitor.cpp:545-658 |
| closeout 状态机 | FutuRiskMonitor closeout 段 | FutuRiskMonitor.cpp:959-1192 |
| 十步风控链 | RiskCoordinator::checkRisk | RiskCoordinator.cpp:117-402（286 行，复核） |

### 2.3 对账哲学

影子簿三分类对账：本地账 vs 柜台账差异按可解释/需关注/必须处置三类分流，是 halt_shadow 巡检方法论的机制基础。**结构性缺口：本地账 vs 柜台漂移无自动对账**（见维度五 T3）。

### 2.4 生产实证（11-02~11-11，验证功能实现的真实表现）

| 日期 | 事件 | 判定 |
|---|---|---|
| 11-02 | 09:06 撤单 ack 风暴 → ZOMBIE → HALT 84min；10:31 settlement-confirm 卡死报废 4.5h | 批 B2/B3 针对形态；watchdog 文件告警无人知的教训 |
| 11-03 | 开盘复测闩锁 3 秒 lift（对照 84min）；退避 1000/2000/4000 全天完整；20 ZOMBIE 全自愈 | **批 B 验收通过**（md5 a9b61057） |
| 11-04~11-06 | ec 连续 3 个交易日上午开盘跳空打穿隔夜库存 → IRREVERSIBLE halt（11-04 两次/11-05/11-06 各一次） | 行为正确；ec 隔夜库存来源需专项核查 |
| 11-09 | 批 B 升级链路首次实战（EXCEEDED maxHold 120s + PERSISTENT ≥300s 按设计触发）；ec2611 闩锁 3.3h 不放（reprobe 永久停止、无降级重探） | 批 C 立项候选 |
| 11-10 | SimNow 8193 心跳超时连环断连 → 21:18 settlement-confirm 挂死 → 夜盘全黑 ~1h40m | settlement 第五次复发（4097=正常收盘、8193=心跳超时） |
| 11-11 | 13:35 ao 首次日亏 IRREVERSIBLE（-21020<-20000）→ 强平 ao2611×17@2684 + ao2612×17@2662；19:50 settlement 卡 ≥50min，watchdog 零告警（会话窗口盲区），靠 20:40 cron 重启解围 | 日亏熔断+强平链全链路首次实战通过；watchdog 盲区实锤 |

成交计数法核验（`grep -c "Trade notified" Logs/Trader/trader_ctp_*.log`）：11-09=350 / 11-10=322 / 11-11=330，三晚 settlement 连环（9min 自愈 / 挂死 1h40m / ≥50min）期间计数正常，链路无静默丢单。

### 2.5 ④号风险排序（原序）

①本地账 vs 柜台漂移无自动对账 → ②halt 期硬闸盲区 → ③`_bits` 共享 → ④settlement 卡死 + 节末 fire-and-forget → ⑤ec 配置纪律。

---

## 维度三：代码优雅度（素材：②号报告）

### 3.1 优点（一手复核采信）

- **内存管理**：unique_ptr 64 处 ≫ 裸 new 5 处，所有权清晰。
- **日志纪律**：WTSLogger 提及 470 处（实际调用点 428，含头文件复核口径），真 `std::cout` 为 0（"cout"grep 命中均为 scout 变量子串误报，已排除复核），统一走 HotLogGate 限速。
- **命名一致**：`_` 前缀成员 3941:1（几乎无漏网）。
- **fail-fast 装配尾**：assemble 尾部 fail-fast（FutuModuleAssembler.cpp:729-761），配置错误启动即拦。

### 3.2 问题清单

**P0**
- **assemble 三合一巨石**：FutuModuleAssembler.cpp:64-762 单函数 699 行完成全部装配；限速硬编码 :382-384、兜底 :634、超时 :643 内嵌其中。

**P1**
- ArbExecutionBridge::onTick 418 行（ArbExecutionBridge.cpp:24-441），内嵌超时魔数 :71。
- RiskCoordinator::checkRisk 285 行十步链。
- TradingState.h:303 Release 下失效的 assert（依赖 assert 做运行防护的路径在 -DNDEBUG 下裸奔）。
- 配置读取三套平行实现（规范版 FutuConfig.cpp:16-111，另两套散布）。
- StrategyCoordinator.cpp 2007 行巨石：processTick :517-769、processQuoting :1179-1444 各 8 段；影子对账逻辑内嵌 :716-757。

**P2**
- 置信度 0.9 三策略重复定义。
- epsilon 1e-9/1e-10/1e-12 散布 10+ 处，无统一常数。
- `_raw_variant` 裸变体（StrategyCoordinator.h:214）。
- 空检查链 8+ 处；AsyncArb 魔数。

**P3**
- FutuDataDefs.h 孤儿文件（全仓零 include；:11 兼为边界漏网直引，建议二选一处置）。
- MicroAlphaEngine 空壳。
- 注释失配实证 1 处：FutuModuleAssembler.cpp:381（注释"对冲10单/秒"与代码 `setRateLimit(Source::HEDGING, 30, 1000)` 实际 30/s 不符）；:536-541 为已移除组件（MicroAlphaEngine）残留日志段落，属死代码而非失配；②号原列 :629-637 经逐行复核注释与代码一致（"加载失败，使用默认配置"），已从失配清单撤下。

### 3.3 勘误记录（②号自查）

refreshQuotes 返回值忽略疑点撤销：主调用点已捕获返回值（StrategyCoordinator.cpp:1426 调用 → :1435-1436 捕获）。

---

## 维度四：测试与文档完备性（素材：③号报告）

### 4.1 测试资产

- 44 文件 / 228 活跃 TEST（复核精确值；grep 全量 229 中含 1 条注释掉的旧用例 `//TEST(...)`），futu 相关 199 TEST / 32 文件（内容引用 futu 命名空间/WtFutuCore 头文件口径复核值；③号原口径 216 未复现，两口径均列出）；TestUnits 基线 **224/228**（4 失败均为已知环境项：test_session/test_shm/theta_grid×2 缺回放 CSV）。新增源文件后必须重跑 cmake（GLOB）。
- 单测对策略层改动覆盖良好（如 §20.8-B 批新增 test_v20_8_latch_batch 6 例全过）。

### 4.2 零直接测试 25 项（③号清点）

CloseoutExecutor、CloseoutOrchestrator、FutuQuoter、FutuRuntimeOps、OrderRouter、ArbExecutionBridge、FutuModuleAssembler、signals 多源等 25 个组件无直接单测——其中成交入账（processTradeFill）、closeout 状态机、装配器属高风险无测区，当前仅靠回测冒烟 + 生产巡检兜底。

### 4.3 配置契约

配置契约三件套四源 unknown=0（四份配置样例与文档逐一比对零未知键）——配置漂移风险当前受控；但 ec 块阈值漂移（ao=-20000 / ec=-50000）证明"契约一致"≠"取值合理"（见维度五 T5）。

### 4.4 文档健康度

- **CONFIG.md 656 行为最健康文档**，与实现同步良好。
- CLOSEOUT_EXECUTOR_DESIGN.md 未纳入 git 跟踪且锚点漂移（引用的代码位置已失配），需 `git add` + 重新校锚。
- 工作区未提交条目动态变化：③号盘点时点 59 条（33D/19M/7??）；复核时点 2026-11-12 为 56 条（33D/16M/7??）——审计期间所有文档治理与批 B 成果均未落 git，建议按主题分批 commit。

---

## 维度五：技术债优先级（素材：②④交叉 + 生产实证定序）

| 优先级 | 技术债 | 证据 | 影响 | 对应运维清单 |
|---|---|---|---|---|
| **T1** | **halt 期 checkRisk 整体跳过 = 结构性硬闸盲区** | RiskCoordinator.cpp:123-159 | halt 期间新风险事件（如另一腿继续恶化）无硬闸拦截 | ③settlement 自愈立项关联 |
| **T2** | **`_bits` 进程级共享位图** | HaltReason.h:95 | ec halt 污染 ao 影子（11-06 分歧 17 条实证）；P1-6 位图实例化是 HaltGate union/bits 激活的**前置硬阻塞** | ④P1-6 位图实例化 |
| **T3** | **本地账 vs 柜台漂移无自动对账** | ④号①位风险；影子簿三分类仅人工巡检 | 漂移静默累积，依赖巡检发现 | 立项候选 |
| **T4** | **assemble/processTick/processQuoting 三巨石** | FutuModuleAssembler.cpp:64-762；StrategyCoordinator.cpp:519-761/:1180-1440 | 改动回归面大、审查成本高（②号 P0+P1） | 重构池 |
| **T5** | **settlement 卡死连环（三晚）+ ec 配置纪律（阈值漂移）** | 生产实证 11-09/10/11；远程生产 dist/config.yaml:74 ao=-20000 / :176 ec=-50000 | settlement 卡死当前靠 cron/人工重启解围；ec 阈值未对齐致连续 4 日 IRREVERSIBLE | ②watchdog 盲区 + ③settlement 自愈 + ⑥ec 阈值对齐 |

> 定序说明：④号原序以"对账缺口"为首，②号以巨石为 P0；交叉后按**生产损失概率 × 修复阻塞关系**重排——T1/T2 互为因果链（halt 盲区在 `_bits` 共享下无法安全收窄），故置顶。

---

## 总评

| 维度 | 结论 |
|---|---|
| 架构设计 | **良**。五层清晰、并发面极小、边界干净；两处高危（`_bits` 共享、`_cb_mtx` 串行）均有明确修法与前置关系 |
| 功能实现 | **良+**。九层风控纵深在生产经历真实检验（日亏熔断强平、批 B 退避、闩锁升级链均按设计触发）；settlement 类故障是唯一未闭环的稳定性短板 |
| 代码优雅度 | **中+**。纪律性优点突出（内存/日志/命名/fail-fast），但三巨石与平行实现构成主要维护税 |
| 测试与文档 | **中**。单测基数健康（216 futu TEST），但 25 项零直接测试恰含最高风险组件；CONFIG.md 优秀，设计文档有漂移 |
| 技术债 | T1~T5 五项，T1/T2 存在修复顺序约束，其余可并行 |

## 建议行动（待用户裁决的运维清单）

1. 11-11 巡检三事件落账 `TODO.md` / AGENTS.md §20.8。
2. watchdog 盲区修复（19:45-21:00 段豁免缺口，或 settlement-stuck 全天候）——可与 ③settlement 自愈捆绑立项。
3. settlement 自愈立项。
4. P1-6 位图实例化（`_bits` 每策略私有；HaltGate union/bits 激活的前置硬阻塞）。
5. 批 C：闩锁 PERSISTENT 复活路径（11-09 ec2611 3.3h 不放实证）。
6. ec 阈值对齐（ao=-20000 / ec=-50000 → 统一 -20000）。
7. ec 隔夜库存专项核查（连续 4 上午 IRREVERSIBLE 的敞口来源）。

> 另有两项框架层待批池既有项不受本报告影响：批 A（持仓账根因，WtUftCore 框架层，待授权）、MonSvr 鉴权（WT_MON_TOKEN + 确认 117.135.127.173）。
