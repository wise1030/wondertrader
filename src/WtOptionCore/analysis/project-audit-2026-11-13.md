# WtOptionCore 项目全面审计报告

> **版本**：v2（2026-11-13 复核修订版）
> **修订记录**：
> - v1（2026-11-13 首轮交付稿）：4 分域并行深挖 + 关键发现复核。
> - v2（同日）：① 对 v1 全部事实型结论做第二轮独立重验（17 项）；② 对首轮"复核意见"做自审（部分结论软化/修正）；③ **新增 1 个 P1 发现（ExpiryData 日内分数读取墙钟，前两轮均遗漏）**；④ 修正 4 处数字勘误；⑤ 改进路径重写为带验收标准的四梯队方案。
> **审计对象**：`src/WtOptionCore`（约 140 个 C++ 源文件 / 2.7 万行，测试 8 套 3,363 行，64 个提交）＋框架边界（`src/Includes`、`Share/TimeUtils.hpp`）
> **方法**：分域并行深挖 + 两轮关键发现逐条独立复核；所有结论附文件:行号证据。

---

## 一、执行摘要

### 总体定位

WtOptionCore 是一个**期权做市系统从 quantbox 到 UFT/WonderTrader 框架的完整迁移+重构项目**，功能面覆盖期权双边报价、gamma scalping、波动率拟合、9 类扫描器、组合套利单、delta 对冲与收盘 flatten、到期结算、三层风控、归因与统计。经过 4 个月 64 个提交的持续治理（P0/P1/B/R/D 编号修复体系 + analysis/ 下 17 份历史报告），项目处于**"功能完备、债务可控、正在收敛"**的健康轨道上。

### 分维度评分

评分锚点：8–10 优秀（可直接实盘运维）/ 6–7.9 良好（有明确债务与治理路径）/ 4–5.9 合格（功能可用但债未量化）/ <4 高风险。权重：正确性 40%、可维护性 25%、可测性 20%、文档与过程 15%。

| 维度 | 评分 | 一句话结论 |
|---|:---:|---|
| 架构设计 | **7/10** | 模块边界总体清晰、DIP 多处真用；但策略类是无可争辩的"上帝协调者"，全部周期调度寄生在 tick 流上 |
| 功能实现 | **7/10** | 定价/执行/风控链完整且注释可追溯；存在 1 个回放正确性 P1（ExpiryData 墙钟）、1 个配置契约缺口（scanner 限额静默失效）、1 个待验证功能风险（combo 130ms 超时） |
| 代码优雅度 | **6.5/10** | 注释与决策记录质量罕见地高、TODO/FIXME/HACK 标记 0 处；但重复实现多（FP 助手 5 份、FAST/SLOW 调度 3 套）、魔数散落、最大文件 3,787 行 |
| 测试与验证 | **5.5/10** | 8 套测试覆盖风控/扫描/AEP/拟合关键面；但无测试框架、两大核心模块（COP 1,823 行 / CTG 排序）0 专测，且存在测试名与内容漂移 |
| 基础设施/运维 | **6.5/10** | 回放门禁（HARD/SOFT 分类）与 README 活文档是亮点；`.so` 手工同步、无 `-Wall`、无 sanitizer、样例配置漂移 |

### 最需要关注的问题（v2 修订版，按 影响×暴露面 排序）

| # | 问题 | 级别 | 证据 |
|---|---|---|---|
| 1 | **ExpiryData 日内分数读取墙钟**——`getIntradayFraction()` 用 `TimeUtils::getCurMin()`（系统时钟，`Share/TimeUtils.hpp:173-180`）而非模拟时间。回放中 T（maturity 日内部分）取决于**回放运行时刻的墙钟**：不同时段跑回放 → 不同 T → 不同 IV/希腊字母/理论价/报价/成交。此前跨跑一致性验证全部在同一墙钟窗口（00:36–01:30）内完成，恰好掩盖了该问题。**直接影响全部历史回放结果的绝对水平有效性** | **P1（新增）** | `ExpiryData.cpp:114`；`Share/TimeUtils.hpp:173-180`；`Includes/WTSSessionInfo.hpp:142-185`（timeToMinutes autoAdjust 语义） |
| 2 | **OptionPricer2 的 BCI 缓存以裸 `OptionData*` 为键且永不清理**——`purgeExpired` 每次 session begin 调用，`s_cache` 悬空键持续增长；若分配器地址复用，旧 `ExpiryInfo`（到期日/年化）被错误复用 → **潜在错误定价**（依赖 allocator 行为，需复现确认）；map 无界增长为已证实事实 | **P1** | `OptionPricer2.cpp:422`；`OptionGrid.cpp:591-599`；`OptionExpiryLifecycle.cpp:8-11` |
| 3 | **scanner 的 per-expiry 数值限额静默失效（配置契约缺口）**——`maxPosOpt/maxPosFut/maxOrderSize/minProfit` 被解析进 `expiryOverrides`，但 `getExpiryOverride()` 全模块零消费者（`IScanModule.h:188-191` 仅定义），执行路径只消费 `enabled`。scanners 段 schema 豁免（`ConfigValidator.h:119`）、README 与现行 `full.json` 均未文档化，仅过时的 `advanced.json` 有旧式键——**当前无人受影响，但配置入口存在且静默丢弃** | **P2（契约类，修复成本极低）** | `HftOptionStrategy.cpp:2666-2682`；`IScanModule.h:139-141,177,188-191` |
| 4 | **combo 腿超时 130ms × 回放 tick 节奏（待验证的功能风险）**——超时以模拟时钟计、每批清扫（`HftOptionStrategy.cpp:1803-1827`）；500ms tick 节奏下组合腿实际只有约一个事件窗完成成交；`cancelReason="own timeout (leg never traded…)"` 日志佐证其高频触发。与历史 scanner 组合高失败现象同向，**高嫌疑，需 A/B 验证**（130ms vs 500ms/1s 对组合成功率与滑点的影响） | **P2（待验证）** | `ComboOrders.h:79,161,207`；`ComboOrders.cpp:213-224`；`HftOptionStrategy.cpp:1803-1827,2616-2618` |
| 5 | **365/252 混合日历口径 + 到期日 T 下限**——discount 日历日（`ExpiryData.cpp:29`）、maturity 交易日（`:140-143`）、theta 365+252 混用（`BlackCalc.cpp:193-194`）；到期日 `bdays=0` clamp 到 1/252（`:103-105`）。跨链一致但从未文档化；"到期日 IV 偏高程度"**待量化验证** | **P2** | 证据链见 §4.2 |

结构类债务单列榜首：**HftOptionStrategy 上帝协调者**（37 个本地 include、约 120 个成员字段、3,787 行实现、`setupAsyncCallbacks` 单函数 1,111 行）——治理路径见 §九。

---

## 二、项目概况与演进

- **体量**：约 140 个 C++ 源文件 / 2.7 万行（不含测试）；`HftOptionStrategy.cpp` 3,787 行为最大单文件，其次 `CompositeOptionPricer.cpp` 1,823 行、`OptionGrid.cpp` 914 行、`OptionQuoteManager.cpp` 895 行、`OptionPricer2.cpp` 874 行。测试 8 套 3,363 行。
- **演进轨迹**（git 考古）：`1865c997`（首次落地）→ `312dc70f`（quantbox→UFT 完整迁移）→ 近 60 个提交构成三条主线：**P0/P1 修复族**（跨线程断链、状态机收敛、per-expiry 定价隔离）、**R 系列重构**（EdgeScannerBase 合并、OTD/UTD 公共基类、dead code 清除）、**近三周的治理收尾**（AEP 公平排水、拟合治理、三层风控、timer/会话调度电平化）。
- **过程质量**：本项目最突出的隐性资产——代码内 TODO/FIXME/HACK 标记 **0 处**（另有 1 处运行时 deprecated 迁移告警 `HftOptionStrategy.cpp:462`，属设计而非债务）；几乎每个修复都带"根因+实观测证据+验证方式"的编号注释；`analysis/` 下 17 份历史报告全程可追溯（2026-07-17 报告发现的"`_pricerType` 读取未用"已在 `HftOptionStrategy.cpp:323-330` 确认修复闭环）。

---

## 三、架构设计评价

### 3.1 分层总图（实测依赖方向）

```
宿主 (WtRunner/WtBtRunner, dlopen 插件)
  └─ HftOptionStraFact (工厂)
      └─ HftOptionStrategy ── 策略协调者（37 本地 include / ~120 成员字段）
          ├─ 事件泵   OptionAsyncEventProcessor（worker 线程，双队列）
          ├─ 数据网格 OptionGrid（ExpiryData→StrikeData→OptionData 三级容器）
          ├─ 生命周期 OptionTradingGrid（OTD/UTD/ETD 三张表 + OQM + OrderGateway）
          ├─ 执行门控 ControllableTradingGrid（rank→TPS→drainPendingQuotes）
          ├─ 定价     IOptionPricer ← {CompositeOptionPricer, GammaScalpOptionPricer}
          │              └─ 共享内核 OptionPricer2（Black+IV反演+曲线托管）
          ├─ 拟合     AsyncVolFitter → SingleFlightTaskRunner（独立线程）
          ├─ 风控     TradingState（单写者状态机）/ RiskFilterChain / RiskLimitsEx
          │              / PositionGuard / PositionOffsetMgr / StressAnalyzer / OptionRisk
          ├─ 扫描     OptionScanCoordinator ← 9 个 IScanModule
          ├─ 对冲     OptionHedgeCoordinator → DeltaHedgeExecutor（纯函数 decide）
          └─ 支撑     OptionTimeService / CommandBus / QuoteStatistics / 归因/值序列
```

**include 方向核验**：底层 include 策略层的**严格违规为 0**；模块头未被任何外部模块 include（`nm -D -u` 无 Wt/wtp 符号，插件边界干净）。

### 3.2 架构资产（值得肯定的设计）

1. **双队列事件泵的公平性设计**（`OptionAsyncEventProcessor.h:155-182`）——SPSC 无锁环承接 MD 高频流、mutex vector 承接低频 trader 事件、trader 每批 256/1024 保底配额、控制类事件豁免容量门永不丢弃、tick 溢出按合约合并保最新（`cpp:221-258`）。事件值语义化（实测 `sizeof(AsyncEvent)=272B`，无内嵌指针）使队列安全。
2. **`TradingState` 单写者状态机**（`TradingState.h:6-9`）——`setState → applyTradingState`（`HftOptionStrategy.cpp:3462-3499`）是 `enabled/panicked` 的唯一权威写者；`_channelDown` 是真正的引擎线程→worker 围栏（`cpp:2850` 写 / `2523` 清 / 4 处 acquire 读）。
3. **三处教科书式 DIP**：`IStrikeFinder`（交易所选档注入）、`IScanModule`（9 扫描器工厂静态注册，CTG 只依赖接口）、`IOptionPricer`（`OptionGrid::computeValues` 多态分发）。
4. **`_dirtyOptions` 变更集 + OptionData 双缓冲 values 表**（`OptionData.h:110-122`）——worker 单写者约定下读路径免锁，正确取舍。
5. **batch 内 position 先于 trade 的顺序约束**（`AEP.cpp:504-530`）——记录了"reordering destroyed the controlled bridge"的教训。

### 3.3 架构债务（按影响排序）

**A1 HftOptionStrategy 上帝协调者**
- 37 个本地模块头 include + **约 120 个成员字段**（含 4 个嵌套 config struct；不同计数口径 119–150）横跨 ≥30 个子系统；
- `setupAsyncCallbacks`（`cpp:1429-2539`，实测 1,111 行）以 9 个 lambda 承载 worker 侧**全部**业务编排；
- init + 5 个 setup* 共约 915 行纯装配代码。
- 后果：任何子系统变更都要打开这个文件；编译依赖簇与之互为因果。
- 可抽取的 6 个职责块：`SessionScheduler`（`isInQuotableZone` 纯函数已是范本）、`VolFitDriver`、`RiskMonitor`、`OrderExecutorAdapters`、`FillAttributionExporter`、`AsyncCallbackSetup`。

**A2 全部周期调度寄生在 tick 流上**
框架已无 `on_schedule`（`WtHftEngine.cpp:332-336` 整段注释；`HftMocker.cpp:286` 同），策略在 `on_tick` 里自合成 timer（`cpp:2770-2791`）。B4 停报价、收盘 flatten、保证金检查、对冲、拟合触发全部依赖行情驱动——行情静默期调度停摆。电平化整改已把"漏动作"压到最低，"无 tick 无心跳"仍是结构事实；上实盘前建议加独立毫秒定时器兜底唤醒 worker。

**A3 领域中心（OptionData）的契约风险 + 未兑现的抽象**
- `OptionData.h` 被 34 个文件 include——这是期权领域模型的正常中心性，**不是问题本身**；真正的问题是：头注释声称双缓冲，实际原子保护只覆盖 values 表，`m_market/m_position/m_active/m_currentMarket` 均为裸字段，线程契约仅靠"worker 单写者"约定（`OptionData.h:152-165`）。
- `IOptionGrid` 接口**未被任何消费方注入使用**（全部持具体 `OptionGridPtr`），且接口头直接 include 具体数据类（`IOptionGrid.h:8-10`）——属于"未兑现的抽象"（无注入、无 mock 使用），其剩余价值是签名约束与未来测试替身，应要么启用要么精简。
- `IScanModule.h`/`IOptionGrid.h` 接口依赖具体数据层，削弱隔离价值。

**A4 横向耦合点**
- `AsyncVolFitter` 直接依赖具体 `OptionPricer2` 类（`AsyncVolFitter.h:3-9,91`）——构建期依赖、属同域协作，影响有限；**真正的痛点是 `tryPublish(OptionPricer2&)` 的具体类签名使 fitter 无法用 mock pricer 做单测**。
- CTG 对 `m_ctx->enabled` 存在防御性第二写点（`ControllableTradingGrid.cpp:137,154`，注释声明为 callback 未接线时的 fallback）——与单写者纪律有残留张力。
- OTG↔CTG 之间裸指针（`ControllableTradingGrid.h:130`）；两处重复 executor typedef（`OptionTradingGrid.h:49-51` vs `ControllableTradingGrid.h:49-62`）。

**A5 框架类型渗入库层**
OTG 持 `IHftStraCtx*` 并调 `stra_get_comminfo`、OptionExpiryLifecycle 调 `stra_cancel_all`、QuoteStatistics/ExpiryData 各持 `WTSSessionInfo*` 裸指针——迁移期现实，但堵死了网格层脱离框架单测的可能。

### 3.4 线程模型评价

整体正确性建立在"worker 单写者 + 原子围栏"纪律上，执行良好（QuoteStatistics 零锁 `std::map` 的安全前提"全部回调在 worker 线程"已逐点核验成立）。三个弱点：① 未来新增监控线程（如 CommandBus 读 `getStats`）即破坏零锁前提——**建议在类注释加"加读者前必先加锁"显式告警**；② control 类事件绕过容量上限理论上无界（发生率极低）；③ `has_events()` 恒 true（`AEP.cpp:354-363`）使 worker 每 100µs 空醒。

---

## 四、功能实现评价

### 4.1 定价与波动率链（系统的心脏）

**结构**：`OptionPricer2` 为共享引擎（Black 内核 + IV 反演 + 曲线托管），`CompositeOptionPricer`/`GammaScalpOptionPricer` 为两种策略壳（`OptionPricerMode.h` 明令"禁止合并"，边界意识正确）。

**数学实现质量**：`BlackCalc` 为 QuantLib BlackCalculator 血统移植（erfc 精确 CDF，d1/d2/gamma 链正确）；IV 反演用经典 Brent（`BlackImpliedCalculator.h:31-120`）；per-strike 平价合成远期四价齐+价差合法+期货 mid 按 1/spread 加权（`OptionGrid.cpp:750-818`）；NaN/除零防护链有原则——**"Never turn a failed vol into a 1% quote"**（`OptionPricer2.cpp:663-671`）。

**口径债**：maturity 交易日 /252、discount 日历日 /365、theta 365+252 混用（`BlackCalc.h:42-45` 自认）——各链内部一致，但从未文档化；到期日 `bdays=0` clamp 1/252（`ExpiryData.cpp:103-105`）。

**新增 P1——ExpiryData 日内分数的墙钟源（详见 §4.6）**：该问题与 365/252 口径债同处 T 计算链上，但性质更严重（正确性/确定性，而非口径选择）。

**性能账**：SLOW 轮每期权最深 ≈ 5×100 次 Brent 迭代；缓解已在位（α=1 闭式快路径、FAST 增量定价、OpenMP 门控、防抖、SingleFlight 丢过期快照）。三套 FAST/SLOW 调度器并存（COP/OP2/GSP）是收敛候选。

**就绪语义**：`forwardReady && fitReady` 双门 + FAST 缓存清洗 + `m_atmvol` 中位数引导冻结（`OptionPricer2.cpp:545-582`），解开拟合自锁；有单测（`test_enhancements.cpp:1722-1818`）。

### 4.2 执行链

订单生命周期逐段验证：pricer 写 desired market → CTG `refresh→check_markets→rank→TPS→drainPendingQuotes` → OTD/UTD 公共 `updateOrders()`（`InstrumentTradingDataBase.h:85-97`）→ OQM 双侧报价状态机 → OrderGateway → `stra_quote/stra_buy`。撤单有 watchdog + sweep + stuck 冻结。localid 由框架生成、策略只登记——边界清楚。

### 4.3 风控栈（三层，分层清晰）

第 1 层 per-order 过滤在 OQM::updateOrders 单点前置（`OQM.cpp:182,206`）；第 2 层 per-instrument 持仓限制——**scanner 组合单的 per-expiry 限额未生效（§4.5）**；第 3 层全局 panic 单一写者、OR-merge 触发源。`DeltaHedgeExecutor::decide` 纯函数 + `OptionHedgeCoordinator` in-flight 所有权是对冲侧正确切分。

### 4.4 Scanner 生态

9 扫描器静态注册，4 个同构 edge 已合并为 `EdgeScannerBase`，GARCH(1,1) 真实现，hit→CTG 节流→组合腿单→对冲腿链路完整。弱点：`MMScanner::fireOption` 为迁移遗留空壳（`MMScanner.cpp:28-33`，真路径在 `publishHit`）；`IScanModule::log` 用 `std::cout`（`IScanModule.cpp:72`）违反 WTSLogger 约定。

### 4.5 功能缺口与风险清单（v2 修订版）

| 级别 | 问题 | 位置 | 说明 |
|---|---|---|---|
| **P1** | **ExpiryData 日内分数读墙钟** | `ExpiryData.cpp:114`；`Share/TimeUtils.hpp:173-180` | 回放中 T 的日内部分取决于运行时刻墙钟 → IV/希腊字母/理论价/报价随墙钟漂移；跨跑一致性此前恰在同一墙钟窗口内验证，未暴露。修复=注入模拟秒（timer 载荷已有秒级时间源），A/B=不同时段各跑一次回放 diff。**影响全部历史回放绝对水平的解读** |
| **P1** | BCI 指针键缓存 vs purge | `OptionPricer2.cpp:422` vs `OptionGrid.cpp:591-599` | 悬空键+无界增长已证实；"地址复用→错误定价"为**潜在**（依赖 allocator），需复现确认 |
| **P2** | scanner per-expiry 限额静默失效 | `HftOptionStrategy.cpp:2666-2682`；`IScanModule.h:188-191` | `getExpiryOverride()` 零消费者；配置入口存在且静默丢弃。决策门：接线 or 删除字段+文档（§九 T0） |
| **P2** | combo 130ms 超时 × 回放节奏 | `ComboOrders.h:79,161,207`；`HftOptionStrategy.cpp:1803-1827` | 模拟时钟+每批清扫 → 500ms tick 下组合腿约一个事件窗；cancelReason 日志高频佐证。需 A/B 验证后定级 |
| **P2** | 365/252 口径 + 到期日 T 下限 | `ExpiryData.cpp:29,103-143`；`BlackCalc.cpp:193-194` | 跨链一致但未文档化；到期日 IV 偏高**待量化** |
| **P2** | 默认 `curve2_type="gvv"` 双曲线退化 | `OptionPricer2.h:122-128` 自认 | `volcurve_weight` 失效 |
| **P2** | COP 构造内第二条 OP2 创建路径 | `CompositeOptionPricer.cpp:130-133` | `black_params` 当前无人填，隐蔽双引擎陷阱 |
| **P3** | PerfCounter 空壳 | `OptionPricer2.h:65-77` | `start/stop` 从不累计耗时（v2 已实读确认） |
| **P3** | ConstantVolCurve 两口径分叉 | `ConstantVolCurve.h:30-31` | `operator()(od,atmf)` 恒返 1（主曲线乘数语义）vs `operator()(diff)` 返 m_constant——疑似刻意但无注释，需文档化 |

### 4.6 P1 详述：ExpiryData 墙钟日内分数（v2.1 机制勘误）

**机制（v2.1 修正，实施时实读 `timeToMinutes`/`offsetTime` 后更正）**：`ExpiryData::getMaturity() = (bdays + intradayFraction)/252`（`ExpiryData.cpp:140-143`），`intradayFraction` 的"当前时刻"来自 `TimeUtils::getCurMin()` = `localtime(getLocalTimeNow())`——**墙钟，且格式为 HHMMSS**（`Share/TimeUtils.hpp:173-180`）。而 `timeToMinutes()` 的契约是 **HHMM**（`Includes/WTSSessionInfo.hpp:142` 注释"格式如0910"），`offsetTime` 在 session offset=0 时原样透传。两缺陷叠加：

1. **格式错配（主因）**：HHMMSS 数值（如 10:30:00=103000）大于所有 HHMM section 上界 → `timeToMinutes` 走完全部 section 后 `bFound=false` 返回 `INVALID_UINT32` → `elapsedMins >= totalMins` → frac 钳到 1e-6。即**除墙钟 00:00–00:08:59（数值 < 首个 section 起点 900，autoAdjust 对齐到 frac=1.0）外，日内分数恒 ≈ 0**。后果：日内 T 衰减**从未生效**——回测与实盘皆然（实盘墙钟同样几乎总落在 00:09 之后）；到期日 `getSettlementFraction/getPremiumFadeFraction` 同路径，到期日当天结算分数也恒 ≈ 0。
2. **时钟源（次因）**：即便格式正确，回测里墙钟=运行时刻而非模拟时刻。

**为什么此前未暴露**：本项目所有回放验证都运行在 ~00:36–01:38 的同一墙钟窗口，全部落在"frac≈0"分支，结果一致反而掩盖了缺陷；且 v1/v2 审计的机制描述（"T 随墙钟时刻连续漂移"）也不准确——实际是双值阶跃 + live 同样失效，比原描述更严重。

**修复（v2 T1-4 已实施）**：timer 载荷秒级模拟时间注入 `OptionGrid::setCurSecOfDay → ExpiryData::setCurSecOfDay`，`getIntradayFraction` 转为 HHMM 后走 `timeToMinutes`（与原墙钟路径在 00:00–00:09 窗口的行为保持等价）。保留的既有限制：跨午夜 session 的午夜后时刻（如 00:10 < 夜盘 section 原始下界 2100）auto-align 到 frac=1.0——与原实现行为一致，留待后续精度改进。

**验证**：单测 `test_expiry_intraday_fraction_sim_clock`（手算 9:00-15:00/360 分钟基准锚定 HHMM 转换）；跨墙钟窗口 A/B（上午 vs 夜间各跑一次回放，修复前 quotes 应有别、修复后必须一致）作为 KPI 升级项执行。

---

## 五、代码优雅度评价

### 5.1 做得好的

- **注释即设计文档**：几乎每个编号修复带根因+验证说明；"决策考古"注释（如 `IVolCurve.h:97-108` 单源化自述）在量化代码里罕见；
- **代码卫生**：TODO/FIXME/HACK 标记 0 处；`optioncoretypes.h` 公共头足够薄；
- **可测性意识**：`isInQuotableZone`、`DeltaHedgeExecutor::decide` 等核心决策已纯函数化。

### 5.2 需要改进的

1. **重复实现**（v2 已复核计数）：FP 比较助手 **5 份**（`OptionPricer2.cpp:65-72`、`PeriodicCurveFitter.cpp:47-52`、`LinearVolCurve.cpp:19-22`、`GvvVolCurve.cpp:33`、`ControllableTradingGrid.cpp:37`）；`PriceError` 残差类 2 份；good-points 插值 2 份；翼点 decay-carry 2 份（async/sync 刻意双份）；FAST/SLOW 调度 3 套；价差成本模型 2 套；`round_to_tick_by_side` 变体未收编（`CompositeOptionPricer.cpp:65-77` 仍在用）。
2. **魔数散落**：`0.01` vol 地板、`1e-6` tick 哨兵多处重复、combo timeout `0.130s`×3 无配置项（已升级为 §4.5 待验证项）、`±3%` ATM 引导带宽。
3. **风格混杂**：迁入文件 4 空格 + `m_` 前缀 vs 核心 tab + `_` 前缀；注释中英文混杂。
4. **伪工具**：`LatencyHistogram` 全量存储+每次查询重排序（`LatencyHistogram.h:21-27`，v2 实读确认非直方图）；`SingleFlightTaskRunner` 的 TaskResult 通道空转（`:81 (void)result`）。
5. **热路径小污点**：`isRawOptionCode` 用 `std::regex`；commKey `fmt::format("{}.{}")` 拼接 9 处应提 helper。

---

## 六、测试与验证

| 现状 | 证据（v2 复核口径） |
|---|---|
| 8 套测试 3,363 行，7 个注册 CTest + 1 个手工 benchmark | `CMakeLists.txt:260-307` |
| 覆盖良好面：AEP 6 用例、扫描器工厂+信号值、拟合就绪三段式、风控链/registry/OQM 增强 | `test_async_event_processor.cpp`、`test_scanners.cpp`、`test_enhancements.cpp:1722-1818` |
| **零专测的核心模块**：`CompositeOptionPricer`（1,823 行，tests/ 0 引用——v2 grep 确认）、CTG rankOption/rankFuture 排序与 TPS（`ControllableTradingGrid.cpp:410-524`）、`ConfigValidator`、HftOptionStrategy 装配 | 已复核 |
| **测试名实漂移**：`test_ctg_ranking.cpp` 头注释声称测 ranking+TPS，正文 0 处引用（5 处命中全在注释）；README 同步失真 | `tests/test_ctg_ranking.cpp:3-10`；`README.md:618,696` |
| 无测试框架（gtest 引用 0 处）、`test_enhancements.cpp` 2,175 行大杂烩；无 `-Wall`、无 sanitizer | v2 grep 确认 |

**评价**：测试**面**的选择正确（错一次亏大钱的地方有测试），**深度**不足——最复杂的排序与定价组合逻辑裸奔。

---

## 七、基础设施 / 配置 / 运维

- **回放门禁是亮点**：HARD 4 类硬门禁 / SOFT top15（`run_option_replay.sh:64-92`）。
- **运维薄弱点**：`.so` 依赖手工拷贝（yaml/replay_common 有漂移刷新、`.so` 恰好没有）；脚本硬编码机器路径；无热更新。
- **配置体系**：`ConfigValidator` 静态 schema **19 个具名 section** + signals/scanners 两段动态豁免（`ConfigValidator.h:119`），unknown WARN；**样例配置已漂移**——`full.json` 多键逐键 WARN，`advanced.json` 整体过时仍被 README:305 引用。
- **构建**：`-O3 -march=x86-64-v3 -flto` 有明确理由注释；**全文件 0 处 `-Wall/-Wextra`、0 sanitizer**（v2 grep 确认）。
- **README（719 行）**：活 runbook；3 处过时如上。

---

## 八、审计边界与未覆盖清单（v2 新增——诚实声明）

以下 8 个面**本轮未深入**，不代表无问题，留待专项：

1. **PnL/持仓会计端到端正确性**（PnlTracker FIFO/乘数、ExpirationSimulator 结算、fill attribution 账实核对）；
2. **锁顺序/死锁**（OptionGrid 双锁 + AEP + fitter 线程 + OptionValueWriter 线程，无锁序图）；
3. **断线/重启恢复语义**（channel_ready 扫单后持仓重建、在途单归属、registry 跨会话保留）；
4. **资源泄漏全面扫描**（本轮仅查 BCI；其余 static/thread_local 与线程生命周期未查）；
5. **性能实测**（LatencyHistogram 实跑分位、队列 drops/overflow、SLOW 轮耗时——本报告性能结论均为静态阅读推断）；
6. **配置数值域校验**（负乘数/0 TPS/日期越界，ConfigLoader 仅硬校验 6 字段）；
7. **交易所规则差异**（STP/平今平昨/最大下单量——框架/执行层为主，策略层仅触及月份码与 STP 片段，属跨层议题）；
8. **QuoteStatistics 零锁前提的未来读者**（加监控读口前必先加锁，建议写入类注释）。

---

## 九、改进路线（v2 重写：四梯队 + 验收标准）

原则：先止血与护栏，再结构；每项附验收方式；结构重构以 golden replay 基线为门禁。

| 梯队 | 事项 | 成本 | 验收方式 |
|---|---|---|---|
| **T0 当天（决策+度量+止血）** | ① scanner 限额**决策门**：查 schema/README/用户配置 → 有使用者则接线到 combo 前检（复用 RiskLimitsEx 形态）+单测；无则删字段+README 声明未实现。判据明确，不留"二选一" | 1-2h | 决策记录入 README；对应单测 PASS 或字段删除后 grep 无残留 |
| | ② `.so` 同步进 `run_option_replay.sh`（build 产物 hash 对比后自动拷贝 dist） | 1h | 脚本内 cmp 通过；故意改 hash 能触发拷贝 |
| | ③ AEP session-end 指标行（drops/overflow 深度/批延迟 p50/p95 落日志） | 1-2h | 日志出现指标行；`queueDrops()>0` 时可见 |
| **T1 3-5 天（正确性修复）** | ④ **ExpiryData 墙钟修复**：注入模拟秒（复用 timer 载荷时间源），`getIntradayFraction` 改用注入值 | 1d | **修复前**上午/夜间各跑一次回放 diff quotes（预期非空）；**修复后**跨墙钟窗口 diff 为空；8/8 测试 PASS |
| | ⑤ BCI 缓存修复：**首选去键化**（`thread_local` 单实例 + 每次全量赋值；缓存仅省一次 hash map 查找，成本可忽略；天然兼容 OpenMP）——备选：internalId 稳定键 + purge 钩子 / 代际号。v1 方案"搬进 OptionData"侵入数据层，**不推荐** | 0.5d | 新增生命周期单测（purge 后复用场景）；回放 greeks 不变 |
| | ⑥ combo 超时配置化 + A/B（130ms vs 500ms/1s × scanner 回放，组合成功率/滑点/腿单取消率） | 1d | A/B 报告；若 130ms 确为回放主因则改默认值并文档化 |
| | ⑦ `-Wall -Wextra` 清理 | 0.5d | 警告数=0 |
| **T2 2-3 周（结构，带护栏）** | ⑧ **先建 golden replay 基线**（动作日志序列 + 成交集合 + 关键指标容差；复用本周已验证的 diff 能力） | 1d | 基线文件入库；同一二进制两次回放 diff 为空 |
| | ⑨ 分块抽取 `HftOptionStrategy`：SessionScheduler → VolFitDriver → OrderExecutorAdapters（每块一提交 + 一次基线 diff，diff 非空即回滚） | 每块 2-3d | 每次 diff 为空；8/8 测试 PASS |
| | ⑩ ranking 纯函数化 + 3 个测试（rankOption/rankFuture/TPS）；`test_ctg_ranking` 名实归位 | 1-2d | 新测试 PASS；文件头注释与内容一致 |
| | ⑪ 收敛：FP 助手收编 `optioncoretypes.h`；FAST/SLOW 调度器合一 | 2d | 代码去重；回放基线 diff 为空 |
| **T3 持续（工程）** | ⑫ 测试 runner 改造（失败聚合/筛选；再评估是否值得 gtest 迁移——当前 3.3k 行 harness 先补测试更重要） | 按需 | 失败可定位到用例 |
| | ⑬ 样例配置与 schema 对齐；README 修正（GoogleTest 说法/ctg_ranking 描述/advanced.json）；365/252 与 ConstantVolCurve 口径写入设计决策章节 | 1d | 文档评审通过；full.json 启动零 WARN |
| | ⑭ asan/ubsan 独立构建目标 | 1d | sanitizer 构建绿 |

---

## 十、结语

这个项目最值得肯定的不是某个模块，而是**工程过程本身**：64 个提交带职责前缀、修复带编号与实观测证据、analysis/ 全程可追溯、README 是活的 runbook、回放有硬门禁。主要短板也很典型：**迁移期大杂烩尚未完成最后收敛**（上帝协调者、三套调度器、五份 FP 助手），**测试深度落后于系统复杂度**（最贵的两个模块零专测），以及 v2 新增的**回放时钟正确性 P1**——它提醒：本项目的全部验证基建（golden diff、回放门禁）都建立在"回放即模拟"的假设上，而这个假设在 T 的计算链上有一个静默缺口，修复它同时会升级整个验证体系的口径。

> 本报告 v1 → v2 的修订本身即一次演示：两轮复核 + 一轮自审，共修正 4 处数字、软化 2 处过强结论、新增 1 个 P1。"再查一轮"在这个项目里持续产生价值。
