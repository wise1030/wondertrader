# WtFutuCore — 期货高频做市与套利策略（WonderTrader UFT 插件）

WtFutuCore 是运行于 WonderTrader UFT 引擎之上的**期货做市 + 内存套利**策略模块，
以插件形态（`libWtFutuCore.so`）由 WtUftRunner 加载。生产形态为**单进程双策略**：
ao（烧碱 ao2611/ao2612，SHFE，含夜盘）+ ec（集运指数 ec2611/ec2612，INE，仅日盘），
共享交易通道与账户，策略间簿记相互独立。

- 策略层只依赖 `Includes/` 公开接口；对引擎的全部直接依赖收敛于 `FutuEnginePort` 一个入口。
- 本文档只描述**当前生效**的项目状态；历史过程记录见 `AGENTS.md`，运维待办见
  `docs/TODO.md`，配置逐键说明见 `CONFIG.md`。

---

## 1. 目录结构

```
src/WtFutuCore/
├── UftFutuMmStrategy.h/.cpp      # 策略壳：框架回调入口、装配、事件转发
├── StrategyCoordinator.h/.cpp    # 单 tick 流水线（Stage 0~7）与报价编排
├── FutuModuleAssembler.h/.cpp    # 组件装配与依赖注入（ApplicationDeps）
├── FutuComponentFactory.*        # 模块工厂
├── FutuConfigLoader.*            # 主配置解析 + 校验
├── FutuConfigAudit.*             # 四源配置键级审计（unknown/read_missing）
├── FutuConfigMap.*               # 主配置→CoordinatorConfig 唯一映射
├── FutuConfig.* / FutuConfigValidator.* / FutuConfigAudit.*
├── FutuQuoter.*                  # 三层报价执行器（Obligation/Flexible/Bilateral）
├── QuotePolicyChain.h            # 报价决策链（六策略对象，固定顺序即优先级）
├── SpreadOptimizer.*             # GLFT 报价数学：skew/加宽/整档量化
├── SignalAggregator.h            # 信号聚合（六源 + IC 权重 + regime）
├── ToxicFlowDetector.*           # VPIN/毒性检测（单层"检测即双边暂停"）
├── MarketDataContext.*           # 盘口/逐笔上下文
├── UnifiedOrderTracker.*         # 订单槽状态机（撤单确认才清槽，zombie 升级）
├── OrderRouter.*                 # 统一下单路由（来源标记 MM/ARB/HEDGE/CLOSEOUT/RISK_REDUCE）
├── FutuRiskMonitor.*             # 风控闸门群（持仓/频率/日亏/断连/closeout 子态）
├── RiskCoordinator.*             # 风控统一入口 + taker 减仓
├── RiskLiquidator.*              # 强平执行
├── TradingState.h                # 分层状态机（phase/qphase/方向软禁）
├── HaltReason.h / HaltGate.h     # 停机原因位图 + 三模式报价门禁（默认 legacy）
├── FutuPortfolio.*               # 持仓/Delta 账本（引擎影子簿对账）
├── CloseoutOrchestrator.*        # 收盘触发/驱动（含原 Trigger 职责）
├── CloseoutExecutor.*            # 收盘渐进执行
├── FutuRuntimeOps.*              # 订单/成交/通道事件处理
├── SessionPhaseManager.*         # 交易时段/小节/收盘窗口判定
├── FutuHotParamManager.*         # 热参数共享内存 + 覆盖层
├── FutuHotParamWatcher.*         # 热参文件 watcher（仅实盘）
├── PerformanceMonitor.*          # 延迟 ring + 窗口化统计 + KPI 行
├── PerformanceAnalyzer.*         # 成交后分析
├── ZombieLatchPolicy.h           # zombie 闩锁计时（steady 时钟，新段重置）
├── HotLogGate.h                  # 热路径日志 1/N 采样闸门
├── FutuTime.h                    # tick→replay 时间转换单一来源
├── FutuEnginePort.*              # 引擎依赖唯一入口（EventNotifier 解析/告警）
├── SelfTradePrevention.* / SelfTradeCalibrator.*
├── CorrelationManager.* / EventDispatcher.h / BilateralQuoteStats.h
├── arb/                          # 套利子系统（见 §5.4）
├── signals/                      # 信号源（OFI/Momentum/Volatility/LeadLag/BookImbalance/TradeFlow + ICWeightTracker）
├── config/                       # 配置权威源（见 §7）
├── docs/                         # 项目文档：TODO.md（活跃清单）、review/（个案复核）、archive/（历史存档）
└── CONFIG.md / AGENTS.md / README.md
```

## 2. 总体架构

五层结构，自上而下单向依赖：

```
策略壳（UftFutuMmStrategy，框架回调入口，_cb_mtx 串行化全部回调）
  └─ 装配层（FutuModuleAssembler + ApplicationDeps：依赖注入 + validate 启动 fail-fast）
      └─ 协调层（StrategyCoordinator：单 tick 流水线 + QuotePolicyChain 决策链）
          └─ 领域层（Quoter / Tracker / RiskMonitor / Portfolio / Arb / Closeout / Signals）
              └─ 基础设施（HotParam / Performance / SessionPhase / HotLogGate / HaltGate）
```

**线程模型**（实盘四类线程，策略内以属主化 + 小组件自旋锁收敛共享状态）：

| 线程 | 职责 | 主要触碰 |
|---|---|---|
| MdSpi（行情） | processTick 全流水线 | coordinator/quoter/tracker/risk/portfolio |
| TdSpi（交易回报） | onOrder/onTrade/onEntrust → requoteAfterFill 同步补挂 | quoter/tracker/portfolio/risk |
| 套利执行（可选，`useAsyncArbThread`） | 信号循环，事件驱动等待（condition_variable） | arb 三件套 |
| 热参 watcher | 每秒解析 hotparams.yaml，diff 置 pending | HotParamManager |

回测为单线程同步执行（`useAsyncArbThread: false` 强制）。

## 3. 单 tick 数据流

```
on_tick(_cb_mtx)
  └─ processTick
       Stage 0  closeout 触发判定（时段门修正后的 is_trading_session）
       Stage 1  组装 TickContext（时间/ mid / 盘口快照，聚合单快照 getAggregates）
       Stage 2  preCheck（时段/竞价门/暂停态/频控）
       Stage 3  信号更新（SignalAggregator 六源 + 毒性）
       Stage 4  风控检查（RiskCoordinator：硬闸/软响应/taker 减仓）
       Stage 4.5 Router 撤单侧重试（escalateStaleCancels）
       Stage 5  报价决策（QuotePolicyChain 六策略 → PreTradeDecision）
       Stage 6  自动撤单 + zombie 闩锁生命周期（ZombieLatchPolicy）
       Stage 7  refreshQuotes 三层执行（Obligation/Flexible/Bilateral）
  └─ on_trade（同步）：账本/风控/双边统计 → requoteAfterFill 补挂
```

事件驱动补挂：`onOrderEvent` 撤单终态后经 `isOrderSubmissionAllowed`（时段+竞价复合门）
触发 requoteAfterFill；订单事件经 tracker 反查 quoter（O(1) 路由）。

## 4. 配置体系

| 文件 | 内容 | 生效方式 |
|---|---|---|
| `config.yaml`（策略节点） | 合约/报价/风控/收盘/性能/日志闸门 | 启动加载 |
| `coordinator.yaml` | 模块开关/信号/毒性/自动撤单/闩锁/skew/arb 执行 | 启动加载 |
| `hotparams.yaml` | 24 个热参数（报价基数/量/权重/阈值等） | **稳态权威**：watcher 秒级热更，覆盖主配置同名键 |
| `spread_arbitrage.yaml` | pair 定义/风控/子策略参数/ThetaGrid 网格 | 启动加载 |

- 全部读取经 `FutuConfig::readX` 单一插桩点，启动输出四源 `[CONFIG-AUDIT]` 报告
  （unknown / read_missing / 漂移）；fixture 配置 unknown=0 硬断言（契约测试锁定）。
- 主配置→CoordinatorConfig 经 `FutuConfigMap` 唯一映射（13 字段）。
- **生产运行态权威 = 远程 dist 配置**；本目录 `config/` 为样例/开发模板
  （`config_demo_prod.yaml` 为生产快照脱敏版，仅作部署演示）。

## 5. 关键语义与行为约定（当前生效）

### 5.1 delta / position 口径边界
- **策略逻辑 = delta 口径**（skew、qty 衰减、义务、穿越授权归一化分母只用 delta 软限）；
  **风控逻辑 = position 口径**（maxPosition 硬闸触发即停报）。两者禁止混用。

### 5.2 报价数学（SpreadOptimizer）
- GLFT 框架：spread 加宽随波动/毒性/库存 util 缩放；**skew 整档量化**——
  驱动 = 组合（w=0.5)+合约（w=1.0) 符号加权求和，量化到整数 tick（滞环防抖），
  消灭亚 tick 死偏移；中心平移与 alpha 叠加，义务带宽（默认 10t）内恒合规。
- 高库存授权（|util|≥1）时减仓侧可贴中/穿越（cap=min(k_max, spread−1)）。

### 5.3 订单生命周期（UnifiedOrderTracker）
- **撤单确认才清槽**：撤单标记后槽位保留，挂单门禁 = 槽空；超时按**指数退避**
  重发撤单（基线间隔 ×2^n，1s→2s→4s，生产总预算 7s）；重试耗尽置 zombie
  （保留跟踪+计入 pending，永不遗忘活单）。
- zombie 升级 → 合约闩锁停报 + 引擎兜底撤单；存活 zombie 清账后自动释放。
- 闩锁计时 = steady 墙钟"本次连续持有"：新闩锁段重置原点、同段续期保单调；
  持有超 `maxHoldMs`（默认 120s）停止重探风暴（每段一次 EXCEEDED）；超 5 分钟
  升级 ERROR + 挂起单号清单诊断。

### 5.4 套利子系统（arb/）
- 价差信号 → 异步执行器双腿下单（并发双发 + 自成交 O(1) 防护 + FAK 对手价链）；
  缺腿四级处置：提交失败即撤对侧 → 对侧成交即反向对冲 → orphan 对手价/市价分级 → 放弃。
- 插件化信号：mean_reversion（默认）/ pairs_trading / statistical_arb / theta_grid。
- **ThetaGridStrategy**（价差反转网格）：θ=spread−EMA、λ=前 5 日 |θ| 分位带沿进出、
  跨腿 lead-lag 确认（CrossLegContext）、FAK ≤2tick 入场锚定盘口 touch、
  出场走 B-3 maker 消耗；`shadow` 模式产出事件 CSV 与 λ 标定文件。
- arb 组合回撤超线 → EMERGENCY：IRREVERSIBLE 停机 + 同步 qphase 阻断报价 + arb 禁用；
  通道恢复时若 halt 存续则 arb 不自动复活。

### 5.5 停机/暂停体系
- `TradingState.qphase`（报价相位）与 `FutuRiskMonitor._trading_halted`（交易停机）
  双轨；停机原因镜像到 `HaltReasons` 原子位图（影子双写，5min KPI 报告分歧计数）。
- 报价门禁经 `HaltGate` 求值：三模式 `legacy`（默认，仅既有标志）/ `union` /
  `bits`；切换为纯配置改动。
- ERROR 相位单轨：下单错误走指数退避自探恢复，不消耗风控恢复预算。
- 日亏熔断：含浮亏口径超 `maxDailyLoss` → IRREVERSIBLE 停机 + 强平（收盘平仓豁免）。

### 5.6 收盘平仓（Closeout 族）
- Orchestrator（触发/驱动，时段窗口可配）→ Executor（渐进执行：DRAIN 静止门禁 →
  ASSESS 锚定实际仓位（closeOnly 可选）→ EXECUTE 分层 FAK 保底对触价 → SWEEP）；
  状态权威单一存放在 FutuRiskMonitor::CloseoutSub，session begin 强制清态。

### 5.7 毒性响应（单层）
- `is_toxic=true` → TOXICITY 相位：停新报 + 边沿撤单一次，保持期（cooloffMs）满恢复；
  无方向单边抑制、无 GLFT 加宽联动。episode 进出/时长/峰值有观测行。

### 5.8 其他
- 双边统计：义务口径（我方双边在场深度，非市场深度），段内覆盖率 `cov`、
  L0 宽度 `w0` 与全深度加权 `avg` 双口径；周期行内存积压 + 定点/周期 flush。
- 热参数：watcher 值比对驱动，tick 线程 drain 应用；应用后交叉复查（权重和/量纲/
  阈值序）只告警不阻断；启动打印与主配置的漂移摘要。
- 日志：热路径（QUOTE/STICKY/REQUOTE/TOXIC/SIGNAL_DECOMP）经 HotLogGate 1/N 采样；
  实盘 logcfg 异步落盘。

## 6. 构建 / 测试 / 回测 / 部署

```bash
# 构建（Linux）
cd src/build_all && cmake .. && make -j$(nproc) WtFutuCore     # Debug 回测用
# 新增源文件/测试文件后必须重跑 cmake（GLOB configure 期求值）

# 测试
make -j$(nproc) TestUnits && ./build_x64/Debug/bin/TestUnits/TestUnits
# 基线 228 项 / 224 过；4 个环境性失败（test_session/test_shm 框架依赖 +
# test_theta_grid×2 缺 /tmp/opencode 回放 CSV，跑前 mkdir -p /tmp/opencode）

# 回测冒烟
cd dist/WtBtFutu
cp <build>/libWtFutuCore.so uft/ && cp src/WtFutuCore/config/coordinator.yaml .
LD_LIBRARY_PATH=./uft:$LD_LIBRARY_PATH timeout 900 ./uft/WtBtRunner \
    -c configbt_v5.yaml -l logcfgbt.yaml < /dev/null
# 全量基线: -c _ec_5d.yaml；判据 = 统计带（笔数±1.5%）+ 健康度
# （0 HALT/zombie/error、session end Delta=0、资金收敛）；回测无逐比特判据（见 §7）
```

**生产部署**（详见根 AGENTS.md §5 与 `scripts/deployment/`）：
- 仅 Release；远程备份 `.bak_YYYYMMDD_xxx` → `.so.new` + `mv -f` 原子替换 →
  kill 后轮询等退出 → 启动后 `ps` 确认单实例；cron 值守 08:40/20:40。
- 策略层改动仅需 `libWtFutuCore.so`；**WtUftCore 头文件成员变更必须 runner+lib
  同树重编译、成对部署、md5 双校验**（ABI/ODR 铁律）。
- 运维工具：`scripts/deployment/channel_watchdog.sh`（settlement 卡死/管线静默/
  行情断连告警）、`reconcile_journals.py`（成交日志对账）、`convert_trades.py`
  （监控页成交转换）、`analyze_toxicity_markout.py`（毒性 episode 离线评估）。

## 7. 已知限制（当前事实）

1. **回测非逐比特**：mocker 成交量拆分用 `srand(time(NULL))` 播种，同配置两次运行
   成交序列不同；另有未定位的跨时段漂移（±1.6%）——一切 A/B 判据用统计带。
2. **柜台账户资源类拒单无分类通道**：CTP 50/51/31（平仓量不足/资金不足）在多策略
   共享账户下属常态，框架 on_entrust 无错误类型字段，策略只能通用计数
   （`orderErrorThreshold` 现为放宽运维值，待框架分类后回收）。
3. **stra_cancel_all 口径回测/实盘不一致**：实盘按 fullCode（两段式）匹配，回测按
   stdCode（三段式）——策略内所有兜底撤单必须传 fullCode 或空串（引擎已策略级化：
   只撤本 context 登记的单）。
4. **WtUftEngine on_session_end 生产不触发**：收盘清态/flush 一律走 section-break
   或 closeout 完成驱动，不依赖框架会话结束回调。
5. **实盘 on_entrust 仅失败触发**：成功回报不存在，依赖成功事件的逻辑在实盘恒空。
6. **框架补丁（越界记录，上游修复后可还原）**：UftStraContext DATA_SIZE_STEP 8000→
   200000；WtUftRunner 补 initEvtNotifier() 调用；libWtMsgQue.so 需置于 runner 工作目录；
   WtUftRunner 构建内嵌 `$ORIGIN` RUNPATH。
7. **适配器层验证盲区**：回测 mocker 不经 TraderAdapter，适配器回调/锁路径改动
   BT 与单测均不设防——评审强制项：锁域调用图审计（临界区内禁止调用自带锁函数）。
8. **SimNow 环境特性**：撤单 ack 延迟/丢失/回报迟到为常态（日均数百条良性竞态），
   zombie 闩锁/停报语义为此设计；settlement-confirm 在午后重连路径可能卡死数分钟
   （watchdog 告警 + 人工重启为当前处置口径）。
9. **PERF 指标口径**：延迟统计按汇总周期结窗（跨周期不残留）；告警按通道限频；
   TSC 非 invariant 环境（云 VM）下绝对 ns 不可跨机引用，只作同机相对比较。
10. **TOXIC 计数口径**：抑制事件 = Runner.log warning 级 "pause active" 行；
    debug 级 `is_toxic=` 行为评分采样，两者不可混计；该指标天然 ±15% 噪声带。
11. **热路径日志采样**：HotLogGate 默认 1/10，排查单笔行为时先确认
    `logging.hotQuoteEnabled/hotQuoteSampleEvery` 配置。
12. **监控页策略清单**：WtUftEngine 不写 marker.json，新增策略需手工维护
    `generated/marker.json`。
