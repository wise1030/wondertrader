# WtFutuCore 工作规则

## 1. 先规划后执行（不可边思考边修改）

任何修改必须严格按以下顺序进行：

1. **查原因**：先用只读手段（日志、代码、数据）定位根因，给出证据链
2. **设计方案**：从业务逻辑出发给出方案，列出备选与取舍
3. **制定计划**：分文件/分步骤的修改清单 + 验证方法，**写入本文件（AGENTS.md）**
4. **确认后修复**：用户确认计划后再动手改代码

禁止在调查/思考过程中直接改代码。禁止"改一下试试"式的试错修复。
**任何情况下都必须先把方案写入本文件再修改，无一例外（2026-08-17 用户明确要求，每次执行时牢记）。**

## 2. 修改范围限制

- **源码修改仅限 `src/WtFutuCore/` 目录内的文件**
- 不得修改框架/库文件（WtBtCore、WtUftCore、WtCore、Share、Includes、WTSTools 等）
- `dist/WtBtFutu/` 是回测部署目标：二进制与配置按既定流程从 src 构建/拷贝，
  运行副本的调参（如 `_ec_5d.yaml`）允许，但配置的权威来源是 `src/WtFutuCore/config/`
- 发现框架层缺陷时：记录在本文件"已知外部限制"，不得越界修复
- **策略层不得处理柜台/账户级报单错误的分类与业务逻辑**（2026-08-17 用户明确）：
  柜台错误（平仓仓位不足 50/51、资金不足 31、流控等）的分类、重查、降级处理
  是框架层（TraderCTP / TraderAdapter / WtUftCore）的职责。策略层只做与自身
  持仓/风险相关的通用处理（如通用错误计数），不得解析柜台错误码/错误文本。

### 架构原则：delta 与 position 的语义边界（2026-08-19 用户明确）

- **策略逻辑 = delta 口径**。报价计算的全部环节（skew、qty 衰减、义务报价、
  穿越授权、retreat 等）只使用 delta：contract delta（= position × hedge_ratio）、
  portfolio delta；归一化分母只用 delta 软限（`contract_max_delta` / `portfolio_max_delta`）。
- **风控逻辑 = position 口径**。`maxPosition` 等持仓硬限制只用于风险闸门，
  触发即暂停报价/交易（halt_quoting / pending_drain / side_pause / taker reduce）。
- **禁止混用**：风控硬顶（maxPosition）不得作为策略报价参数的归一化分母或
  触发阈值；策略软限不得直接触发风控硬停止。同一概念（如 portfolio delta）
  全链路必须单一定义（原始/净口径只选一种）。

## 3. 构建与部署

```bash
cd src/build_all && make -j$(nproc) WtFutuCore
cp src/build_all/build_x64/Debug/bin/WtUftRunner/futu/libWtFutuCore.so dist/WtBtFutu/uft/
cp src/WtFutuCore/config/coordinator.yaml dist/WtBtFutu/
```

## 4. 回测

```bash
cd dist/WtBtFutu
LD_LIBRARY_PATH=./uft:$LD_LIBRARY_PATH timeout 900 ./uft/WtBtRunner -c <config.yaml> -l logcfgbt.yaml < /dev/null
```

- 策略日志: `outputs/Strategy_uft.log`，Runner 日志: `outputs/Runner.log`
- 成交/资金: `outputs_bt/uft/{trades,funds,positions,closes}.csv`
- 回测 coordinator 必须 `useAsyncArbThread: false`（主线程同步）
- 回测策略配置必须显式 `isBacktest: true`（默认 false！）。该开关不仅控制
  "on_trade 只撤不挂"，B+ 起还门控 live-only 路径（事件驱动补挂）：
  缺失时在回测回调栈内同步发单 → mocker `_orders` 迭代器失效 → 死循环
  （2026-08-21 实证：unordered_dense::erase→next 无限循环，CPU 105% 日志冻结）

## 5. 生产部署（远程 ubuntu@129.211.5.54）

- 部署物：`src/build_release` 编译的 Release `libWtFutuCore.so` → 远程 `dist/uft/`
  （部署前远程备份旧版为 `.bak_YYYYMMDD_xxx`）；**禁止部署 Debug 版**（assert 风险，
  见 dist/WtRunnerFutu/rebuild_release.sh 头注释）
- **进程管理有 cron 值守**（`monitor/wt_restart.sh`，工作日 08:40 / 20:40 重启），
  手工 kill 后会被 cron 重建；手工启动前必须确认没有其它实例在跑，
  **任何时刻同一账户只允许一个 WtUftRunner 实例**（2026-08-17 曾出现双实例事故）
- 配置变更先备份（`.bak_YYYYMMDD_xxx`）再 sed

## 当前方案（2026-08-17 夜盘，待执行）

**背景**：当日白盘 50/51 拒单→HALT 事故后实施过 Fix1（策略层解析柜台错误分类），
用户裁定该逻辑属框架层职责 → **回滚 Fix1**，保留 Fix2/3/4（均为策略自身风控逻辑，合规）。

**回滚清单**：
1. `FutuRuntimeOps.cpp`：删除 `isBenignAccountReject` 及良性分支，恢复通用错误计数
   （死单清理/arb 残腿清理/finalizeOrder 保持原样，对所有失败一致执行）
2. `UftFutuMmStrategy.h`：删除 `_pos_insufficient_rejects` 成员
3. 保留：Fix2（canRecover 去 delta_util 软闸门）、Fix3（熔断加减仓感知+阈值5）、
   Fix4（CLOSEOUT 豁免 REVERSIBLE halt + retry session 检查）、L0 触板全停

**回滚后的行为预期**：50/51/31 拒单重新计入通用错误计数（阈值10→HALT），
但 Fix2 使 REVERSIBLE halt 可自动恢复（每 session 3 次），Fix4 保证收盘减仓不被锁死；
真正的分类修复需求记录于下方"已知外部限制"，待框架层处理。

**验证**：Release 编译 → TestUnits → 回测回归 → 远程单实例重启观察

## 当前方案（2026-08-17 夜盘②，待执行）

**背景**：Fix1 回滚后 31 拒单恢复计入通用计数 → 22:21 再次 10/10 HALT。
框架分类未修复前，策略在"账户保证金被其他策略打爆"的夜晚会反复
halt→recover→halt（每 session 3 次恢复预算耗尽后停摆）。

**决策（纯配置调参，不涉及策略层错误分类逻辑）**：
- 远程 `config.yaml` 临时 `orderErrorThreshold: 10 → 1000`
  （今晚账户级 31 拒单是预期常态：买单=回补空仓可成交，卖单=开仓被拒无市场影响；
  通用阈值调高属既有参数的运维调整，非策略层实现错误处理）
- **临时措施**：框架层完成拒单分类修复后须调回（记录在案）
- 重启流程修正（前两次事故教训）：kill 后**轮询等待进程退出**（SIGTERM 可能要
  10s+，失效再 SIGKILL），确认无实例后才启动，启动后再次 `ps` 确认单实例

**验证**：重启后观察 5 分钟 — 预期买方报价正常成交回补，卖方拒单仅 warn 不再 HALT

## 已完成方案（2026-08-21，B+ 订单槽状态机，已部署验证）

**背景**：8/19-8/20 僵尸单事故 — 撤单"发送即遗忘"+ tracker 5s force-untrack
→ 撤单静默失效（主因: halt cancelAll 爆发撞 CTP 前置流控, doCancel 同步失败被吞）
→ 僵尸单在交易所存续（8/19 timeout 3633 次 → 次日晨残留 858 手），冻结可平量、
幽灵成交污染簿记。两轮复核确认的 B+ 方案（撤单确认后才清槽 + 重试 + zombie 升级）。

**修改清单**：
1. `UnifiedOrderTracker.h/cpp`：mark 时写 cancel_time（新增 now 参数）；
   PendingCancel 仍计入全源 pending（删 mark/clear 两处 addPendingQty 增减，自成交排除不变）；
   checkAutoCancel 超时改"重试动作 + cancel_time 刷新 + retry_count++"，
   K 次后置 IS_ZOMBIE（保留跟踪/计入 pending/升级），**永不 force-untrack 活单**；
   配置键 pending_cancel_timeout_ms 改为重试间隔(默认300ms) + 新增 cancel_max_retries(默认3)
2. `FutuQuoter.cpp`：5 处"发送即 clear"删除（handleObligation/handleFlexible/cancelAll/
   cancelSide/onScoutFillCancelObligation），id 保留至 onOrder 终态移除；
   挂单门禁 = level.order_ids.empty()（PendingCancel 不挂新单）；
   "skip stale/pending cancel" warn 降级 debug（B+ 下是正常态）
3. `StrategyCoordinator.cpp`：processAutoCancel 发撤单前先 tryMarkPendingCancel；
   zombie 升级 = error 日志 + 该合约 halt 闩锁（FutuRiskMonitor 新增 zombie halt 集合）
   + `stra_cancel_all(fullCode)` 引擎侧兜底（**必须传 fullCode 格式 "SHFE.ao2609"**
   或空串全撤，见"已知外部限制"fullCode/stdCode 不一致）
4. `FutuRuntimeOps.cpp`：live-only 事件驱动补挂 — onOrderEvent 撤单终态后
   复用 requoteAfterFill 全部守卫补挂；回测维持下一 tick（保可复现性）
5. 通道锚点：onChannelReady 清 zombie 集合+halt 闩锁（对不上的交持仓对账）；
   on_channel_lost 的 cancelAll+halt 照旧

**验证（全部完成）**：TestUnits `test_order_slot_bplus.cpp` 6/6 通过；
全量 46/48（2 个环境性既有失败）；回测 A/B 626→620 笔（噪声内）、
0 HALT/0 zombie/0 timeout/0 error；Release md5 82b86679… 部署远程单实例，
校验 0 警告，13:30 起零重试/零 zombie/零 skip、零段错误（当日 117 万行
segv 均为旧实例 13:08:39 退出竞态刷屏）。实盘撤单重试路径待报价流恢复
（簿记破位 halt 属 W3 既有事项）后观察。

**明确不做**：mocker 撤单必成功，超时/重试/zombie 路径回测不可复现，
靠单测+实盘小流量验证；回测与实盘补挂时序不同属有意为之。

## 已完成方案（2026-08-21③，B+ 复核修复，已验证）

**背景**：B+ 落地后复核发现（用户已确认按建议修复）：
- P1-1 `_zombie_halt`/`_last_soft_warn_ms`（FutuRiskMonitor）被数据线程
  （processAutoCancel/processQuoting）与交易线程（onChannelReady/requoteAfterFill）
  无锁并发读写 unordered_map = UB/崩溃风险
- P1-2 clearZombies 只清 tracker，quoter 槽残留 id 成孤儿：tryMarkPendingCancel 对
  未知 id 返回 false -> 该层挂单门禁永久关闭且不再补发撤单；且断连中失败的撤单
  重连后无人补发（引擎 onRspOrders 只重建自身账本，不对策略回调）-> 部分回归
  8/19 "发送即遗忘" 事故模式
- P2-3 zombie halt 闩锁仅 onChannelReady 清除：无断连的流控吞单场景合约停牌至
  下次重连/重启；`_zombie_escalated` 按合约去重导致同合约第二个 zombie 无升级/
  无日志/无 cancelAll 兜底
- P2-4 handleBilateralQuote sticky 判定取 order_ids[0]，B+ 下头部常为 pendingCancel
  残留 -> 撤单飞行期每 tick 强制顶单 churn（路径A当前禁用，属潜伏缺陷）
- P3：FutuQuoter.cpp:749 残留 warn 未降级；UnifiedOrderTracker.h T4 注释旧口径；
  dist coordinator.yaml 缺新键；UftStraContext.cpp DATA_SIZE_STEP 越界修改未记录；
  delta/position 语义分离改动包无方案记录

**修改清单（全部完成）**：
1. `FutuRiskMonitor.h/cpp`：SpinLockGuard 保护 `_zombie_halt`（新增 retainZombieHalts
   释放无存活 zombie 合约的闩锁）与 `_last_soft_warn_ms`（两处读改写）
2. `UnifiedOrderTracker.h/cpp`：`clearZombies()` 返回 untrack 的 zombie id 列表；
   checkAutoCancel 维护存活 zombie 合约集合（`getAliveZombieContracts()`），zombie
   清零的合约重置 `_zombie_escalated` 去重（重新武装升级）；T4 注释更新
3. `StrategyCoordinator.cpp`：processAutoCancel 每tick `retainZombieHalts(存活集合)`
4. `FutuRuntimeOps.cpp`：onChannelReady 中 clearZombies 返回非空时
   `orderApiCall(stra_cancel_all(""))` 引擎侧全撤清扫 + 广播
   `quoter->onOrder(id, true, 0, 0, 0)` 清孤儿槽
5. `FutuQuoter.cpp`：handleBilateralQuote sticky 判定改"从尾向头找最新 live 单"；
   :749 warn 降级为静默
6. `test_order_slot_bplus.cpp`：+3 用例（ClearZombiesReturnsUntrackedIds/
   EscalationRearmsAfterZombieGone/ZombieHaltLatchReleaseAndRearm）
7. `dist/WtBtFutu/coordinator.yaml` 已同步（md5 与 src 一致）

**验证（全部完成）**：
- TestUnits：OrderSlotBPlus 9/9、全量 55/57（2 个既有环境性失败）
- **逐比特 A/B**：逆向 16 处编辑重建修复前 .so，md5 `b1fead96…` 与修复前部署件
  完全一致 -> 证明修复与基线唯一差异为本轮修改
- 回测行为零回归：`_ec_5d` 修复前 21933 笔 / 修复后 22169/22264/22150 笔（±1.5%
  mocker 随机噪声内）；`configbt_v5` 修复前后均 3 笔；0 zombie/0 timeout/0 HALT，
  PnL 正常收敛（5 日动态权益 60.9 万）
- 路径A（双边接口）修复无回测覆盖（useBilateralQuote=false），启用前需专项验证

**遗留发现（B+ 验证记录勘误）**：AGENTS.md B+ 条目的"回测 A/B 626->620 笔"用当前
dist 任何配置均不可复现（`_ec_5d` 实际 ~22k 笔且修复前二进制同样如此；残留的 621
行 trades.csv 疑为陈旧输出）。22k 为 B+ 二进制的真实行为，非本轮修复引入；若需
复核请用逐比特 A/B 法（备份于 /tmp/opencode/prefix_ab/）。

## 已完成方案（2026-08-19→21，delta/position 语义分离，补记）

**背景**：2026-08-19 用户明确语义边界原则（见 §2 架构原则），本条目补记其落地
（与 B+ 同工作树实现，此前漏记方案条目）。

**修改清单**：
- `FutuRiskMonitor::computeInventoryStrategyInputs`：分子 = 同向 delta +
  同向 pending×hedge_ratio，分母 = contract_max_delta（策略软限）；
  maxPosition 硬闸门独立于 checkHardPositionRisk，block_add 阈值改
  |delta| >= maxDelta×ratio
- `PreTradeDecision.h`：StrategyInputs 字段更名 long_util/short_util ->
  long_delta_util/short_delta_util
- `SpreadOptimizer`：单路径 skew（legacy contractMaxDelta 双路径删除），
  输入 = PortfolioContext.contract_delta_util；inventorySkewScale 参数删除
- `FutuPortfolio`：getPortfolioDeltaUtilization(净口径) 删除，全链路统一
  getRawPortfolioDeltaUtilization(原始口径)；getQtyMultiplierByRisk 死代码删除
- `UftFutuMmStrategy`：on_init 增 maxDelta/maxPosition 配置关系校验（软限>硬顶
  或 maxDelta 缺配时告警）
- 配置注释更新（config.yaml/coordinator.yaml/hotparams.yaml，键名保留兼容）

**验证**：test_inventory_delta_separation.cpp 8 用例（delta 口径/义务触发/硬停独立/
零配置回退/skew 单路径）；行为影响：maxDelta≠maxPosition 的合约 util 缩放变化
（_ec_5d 等宽配置数值不变）。

## 当前方案（2026-08-22，V8 诊断报告 R1 轮，用户已确认执行）

**R1 已实施完成（代码在树），验证结论见下，R2 方案追加于后。**

### R1 实施记录（2026-08-22）

修改清单（全部完成）：
1. **P0-1**：FutuHotParamManager（syncFromFile 只写共享内存+置 pending、值比对去重、
   26 参数边界表+strtod 全串校验拒收 "abc"/负值/布尔、parseHotParamFile 纯函数）；
   FutuHotParamWatcher（每轮 parse+diff 取代 mtime 秒粒度门控、去掉 Targets、
   空 hotparams.yaml 不再导致 watcher 不启动）；UftFutuMmStrategy::on_tick 锁内
   drain（consumePendingApply -> on_params_updated，recursive_mutex 嵌套安全）
2. **P0-2**：FutuRiskMonitor 新增 recordOrders(n) 批量接口，record*/pruneRateWindows
   四处 SpinLockGuard(_rate_lock) 串行化（修复既有 SPSC 多生产者违规）；
   StrategyCoordinator processQuoting/requoteAfterFill 两处捕获 refreshQuotes
   返回值计数；closeout/liquidator 有意不计数（紧急路径不因频控 HALT 卡死）
3. **P0-4**：createMarketDataContext(config, code, tick_size) 装配 setContract +
   setLargeTradeThreshold（大单阈值单源于 signalAggregator.signals.trade_flow）；
   assembler 传 ci.code/ci.tick_size；MarketDataContext::onTick 首帧未装配一次性告警
4. **T1+T5**：StrategyCoordinator 补 ofi_component 搬运；QuotePolicyChain 方向交换
   （1=激进买流->抑制 ask，§6.2 语义）；SelfTradeCalibrator 死字段注释标注
5. **A1**：AsyncArbitrageExecutor 短价差 spread_impact 符号 + 注释公式修正

测试：新增 test_hot_param_manager(9)/test_toxicity_direction(10)/test_v8_r1_p0(9)
= 22 用例全过；全量 77/79（2 个既有环境性失败，0 回归）。

**验证（逐比特 A/B 三方对比，基线经 md5 888db4db… 复现证明逐比特一致）**：

| 指标 | 基线(B+③) | R1 全量 | R1 仅关 P0-4 |
|---|---|---|---|
| _ec_5d 成交笔数 | 22,268 | 5,104 | 5,286 |
| closeprofit(0612) | 616,850 | 128,600 | 151,225 |
| TOXIC 抑制事件 | 74 | ~12,600 | 5,311 |
| HALT/zombie/timeout/error | 0 | 0 | 0 |

**关键发现（R1 不可独立上生产，须与 R2 同批部署）**：
- 主因是 **T1 激活 ofi 通道**：毒性分数从 trade-only 通道的 0.12-0.15 常驻升至
  0.25-0.29（阈值 adverse_threshold=0.10 是 ofi 恒 0 时代标定），触发 72 倍
  （74->5311），其中 90% 因 ofi/imbalance 符号分歧走双边抑制+冷却 -> 报价长期
  暂停，成交/PnL 双降 77%
- 根因即报告 T6（权重不归一：alpha_toxicity 上限 0.3+0.3=0.6）+ T7（large_trade_ratio
  未传，trade 通道被压半）+ S5（OFI pressure 阶跃退化，符号噪声大、|ofi| 频繁饱和
  ±1 触发 extreme 通道 503 次）+ T2（realized 权重平方 0.16）+ T4（VPIN 独立触发
  被门面丢弃）--全部属 R2"毒性评分归一化"范畴
- P0-4（tick_size 0.2->0.5）为次因（-3.4% 成交），其方向正确但效应需在 R2
  归一化后重新评估
- P0-1/P0-2/A1 回测路径无行为影响（watcher 不跑回测/0 次频控违规/arb 路径禁用）

### 1. P0-1 热参数 watcher 线程收编（FutuHotParamWatcher/Manager/UftFutuMmStrategy）

现状：watcher 线程检测 mtime -> syncFromFile -> **在 watcher 线程内直接 applyAll**，
裸写 `_config.quoting`/SignalAggregator 权重/FutuPortfolio 参数（绕过 _cb_mtx，
唯一未串行化的写者）；且无值校验（"abc"->0.0、负值照收）、mtime 秒级粒度同秒修改丢失、
start() 首轮重复 sync。回测侧引擎无 `start_watching`（仅 WtUftRunner.cpp:363 调用），
故不可依赖 commit_section 通知路径（回测不触发）。

**方案**（dirty flag + tick 线程 drain，报告建议项）：
1. `FutuHotParamManager`：
   - `syncFromFile` 拆两步：新增纯函数 `parseHotParamFile(filepath)`（加载+校验，
     返回合法 (idx,value) 列表，便于单测）；syncFromFile 逐项与共享内存现值比对，
     **有差异才写入并计数**，变更数 >0 时置 `std::atomic<bool> _pending_apply`；
     **删除 syncFromFile 内的 applyAll 调用**（签名去掉 Targets/strategy_id）
   - 新增 26 参数 min/max 边界表 + finite 校验：越界/NaN -> warn+跳过该键（保留旧值）；
     变更时逐键输出 old->new 审计日志
   - 新增 `consumePendingApply()`
2. `FutuHotParamWatcher`：watchLoop 改为**每轮直接 parse+diff**（值比对替代 mtime 门控，
   修复秒粒度丢失与首轮重复 sync；1Hz 解析 30 键 yaml 开销可忽略）；start() 的
   Targets 参数与 mtime 字段删除；初始 sync 失败判定改为"解析失败"（空文件=0 变更=成功，
   修复空 hotparams.yaml 导致 watcher 不启动的潜伏 bug）
3. `UftFutuMmStrategy::on_tick`（:585 guard 之后）：`if (_hot_mgr.consumePendingApply())
   on_params_updated();`（recursive_mutex 嵌套安全，复用其 Targets 组装+日志）
   -- applyAll 从此只在 _cb_mtx 内执行（on_params_updated / on_tick 两入口同锁）

### 2. P0-2 做市报单纳入 ORDER_RATE 频控（FutuRiskMonitor/StrategyCoordinator）

现状：recordOrder 仅 3 调用点（RiskCoordinator.cpp:98 taker、ArbExecutionBridge.cpp:270/356
arb），MM 报单路径零计数 -> 最高频来源对频控失明。**实盘 on_entrust 仅失败时触发**
（TraderAdapter.cpp:1335，成功回报不存在），报告备选的 on_entrust 成功分支计数在实盘
恒为零，不可用。另：频控环 LockFreeRingBuffer 为 SPSC，现有 md 线程+arb 线程双生产者
已是违规（实盘 async arb 线程）。

**方案**：
1. `FutuRiskMonitor`：新增 `recordOrders(uint32_t n)` 批量接口；recordOrder/recordCancel/
   recordTrade/pruneRateWindows 四处加 SpinLockGuard（复用 _zombie_halt 的 atomic_flag
   模式，新 flag `_rate_lock`）-- 同时修复既有双生产者违规
2. `StrategyCoordinator`：processQuoting（:1179）与 requoteAfterFill（:1379）两处捕获
   refreshQuotes 返回值（实际挂单数），n>0 时 `_risk_monitor->recordOrders(n)`
   （handleObligation/Flexible/Bilateral 全部经 refreshQuotes 汇聚，天然全覆盖；
   requoteAfterFill 实盘跑 TdSpi 线程，由 1 的锁保护）
3. 保留 taker/arb 现有 3 处计数（与 MM 路径不相交，无重复计数）；
   closeout/liquidator **不计数**（紧急减仓路径不应被频控 HALT 卡死，设计决策）
4. StrategyCoordinator._risk_monitor 若未接线则补 setRiskMonitor（FutuModuleAssembler）

### 3. P0-4 MarketDataContext 装配接线（FutuComponentFactory/FutuModuleAssembler）

现状：createMarketDataContext 忽略参数（FutuComponentFactory.cpp:46-49），
setContract/setLargeTradeThreshold 全仓零调用 -> OrderBookStateTracker tick_size
恒为默认 0.2（EC 实际 0.5，depth_imbalance 距离权重系统性偏差 2.5 倍）、
大单阈值两套口径（tracker 10.0 vs 信号层配置 50.0）。

**方案**：
1. `createMarketDataContext(config, code, tick_size)`：工厂内解析
   modules.signalAggregator.signals.trade_flow.largeTradeThreshold（缺省 50.0，
   与信号层同一 yaml 源，单一口径），调 setContract(code,tick_size) +
   setLargeTradeThreshold(threshold)（后者顺带把 tick_size 灌入 TickTransactionInferer，
   TradeFlowTracker::setConfig:130-138 链路已通）；tick_size<=0 时 error+保留默认
2. `FutuModuleAssembler.cpp:472` 传 ci.code、ci.tick_size
3. `MarketDataContext::onTick` 首帧校验：setContract 未调用过（getCode().empty()）
   一次性 warn（"tick_size/大单阈值为默认值，数值不可信"）
4. SignalAggregator._ctx.tick_size = book.getTickSize()（SignalAggregator.h:226）
   自动获得正确值，无需另改

### 4. T1 ofi_component 搬运 + T5 方向对齐（StrategyCoordinator/QuotePolicyChain）

现状：AlphaResult 构造（StrategyCoordinator.cpp:931-934）漏填 ofi_component（恒 0）
-> toxic_side 永远 0 -> 毒性单边抑制从不生效（恒走双边抑制 else 分支）。
**耦合**：修 T1 后单边抑制开始生效，而 ToxicityPolicy（QuotePolicyChain.h:216-229）
当前映射为 toxic_side==1（激进买流）-> 停 **bid**——微结构上错边（知情买方吃的是我方
**ask**），修 T1 不修方向会比现状（双边全停）更糟。T5 全量裁决属 R2，但方向映射
必须与 T1 同车落地（报告 §6.2 建议：1=激进买流 -> 抑制 ask）。

**方案**：
1. `StrategyCoordinator.cpp:931-934`：补 `alpha_res.ofi_component =
   sig_ctx.alpha.ofi_component;`（SignalAggregator.h:339 已算好，纯搬运）
2. `QuotePolicyChain.h:216-229` 方向交换：toxic_side==1 -> `st.allow_ask=false`
   （"激进买流，ask 面临逆向选择"）；==-1 -> `st.allow_bid=false`；日志文案同步
3. SelfTradeCalibrator.h:94 注释矛盾（1=avoid sell vs cpp 反号）为死字段（零消费者，
   仅 sig_ctx.toxicity.toxic_side 死写），**本轮只改注释**标明语义待 R2 裁决
4. onSyntheticAlpha 路径（ToxicFlowDetector.cpp:79）同样漏填，但该路径整体为死代码
   （SyntheticSignalFusion hasAnySource() 恒 false，R3 清扫对象），本轮不动

### 5. A1 spread_impact 空价差符号修复（AsyncArbitrageExecutor）

现状：:443 短价差分支 `pi1 + (leg2_is_buy ? pi2 : -pi2)`，代数推导（空价差利润变化
= pi1 - pi2）与文件内注释均证明应为 **减号**；现状两腿损失互抵（b-a 而非 -(a+b)），
真实损失超阈的止损/平仓单被放行、净有利单可能被误拒。

**方案**：:443 `+` 改 `-`；:428-431 注释公式同步修正（"Profit = (leg1-orig1) -
(leg2-orig2)"）。注：回测 arb_close.enabled=false 且 minProfitThreshold 可能为 0，
该路径回测覆盖有限，以代数推导+代码评审为主（记入验证说明）。

### 6. 测试与验证

1. **TestUnits 新增**：
   - `test_hot_param_manager.cpp`：parseHotParamFile 合法值/越界跳过/NaN 跳过/未知键忽略/
     值 diff 置 pending、consumePendingApply 原子性
   - `test_toxicity_direction.cpp`：表驱动--ofi>0&imb>0 -> toxic_side==1；ofi<0&imb<0
     -> -1；方向交换后 ToxicityPolicy 抑制边正确（1->ask 停、-1->bid 停、0->双停）；
     warmup 门（vpin_ready 前 vpin=0）
   - P0-2/P0-4 用例并入上述或独立：recordOrders 批量+双线程计数一致性；
     setContract 后 getTickSize/getSpreadTicks/estimateLiquidity 数值正确性
2. **构建**：`make -j WtFutuCore` + TestUnits 全量（基线 55/57，2 个既有环境性失败）
3. **回测 A/B**（_ec_5d）：本轮 P0-4（tick_size 0.2->0.5）与 T1（毒性单边抑制生效）
   为**预期行为变化**，验证目标从"逐比特一致"改为：0 error/0 HALT/0 zombie/0 timeout、
   PnL 正常收敛、日志确认 tick_size=0.5 装配成功、毒性单边抑制方向抽样正确；
   成交数变化幅度记录在案（基线：当前部署版 .so 先跑一轮留存）
4. **部署**：Debug .so -> dist/WtBtFutu（§3 流程）；生产 Release+远程部署不在本轮，
   待回测行为变化评估后另行确认

**明确不做**：T2/T3/T4/T6/T7（毒性数值）、S2-S10、A2-A10 其余、R2-R4、P0-3 细粒度
契约、R5/R6 配置语义、watcher mtime 精度（已被值比对方案取代）。


### R2 实施记录（2026-08-22，已完成，回测验证通过）

修改清单（全部完成）：
1. **T2**：RealizedToxicity::updateCache 删除内部 ×_cfg.weight（加权归门面单次施加，
   原 realized 有效权重 0.4²=0.16）
2. **T6**：ToxicFlowDetector::setParams 对 pred ofi/trade 权重通道内归一（和=1）；
   combined 的 vpin/alpha 权重可配置（vpinWeight 键，默认 0.5）；
   ToxicityParams::fromVariant 加载期边界校验（adverse/vpin/weights 越界回落默认+warn）
3. **T3**：PredictiveToxicity 桶内 imbalance 按实际桶量归一（|buy-sell|/total∈[0,1]），
   vpin=窗口桶归一均值（经典 VPIN 口径，严格有界；原 |buy-sell|/(n×bucket_size)
   单边流可达 1.2 无界高估）
4. **T4**：ToxicFlowDetector 门面 is_toxic 恢复 OR 条件
   （toxic_score>adverse || vpin_ready && vpin>vpinThreshold；PredictiveToxicityResult
   新增 vpin_ready 字段）
5. **T7**：StrategyCoordinator 构造 TradeImbalanceResult 补 large_trade_ratio
   （trade_toxicity 从 0.5×|imb| 恢复满幅）
6. **S5**：OFISignalSource bid/ask_pressure 阶跃公式改线性互补 0.5×(1±ofi)
   （原代数恒等于 0/1 阶跃；暂无下游消费者，纯数学修复）
7. **S2**：LeadLagSignalSource _current_mid/_current_timestamp 构造初始化（UB 修复）
8. **S3**：OrderBookStateTracker::updateDerivedMetrics 单边盘口 mid/spread 清零
   （与策略层 C1 对齐；消费方均有 mid<=0 守卫）
9. **A2**：SpreadCalculator::getState 写 current_price=current_spread + Manager
   stored_state 同步 -- TrendFollowing 价格止损链路打通（原恒 0 死分支）
10. **A3**：StatisticalArbStrategy 删除 mspread_imbalance/volume_imbalance 死因子及
    weight_mspread/_adaptive_weight_mspread/mspread_return（输入字段全链路从未填充，
    4 因子归一；L2 管道就绪后按真实语义重建）
11. **A9**：残腿对冲单 Source::CLOSEOUT -> Source::HEDGING（不再污染 closeout 统计
    /享受 Fix4 REVERSIBLE 豁免）
12. **A10**：bridge 平仓单两处 skip 路径补 onArbSignalDropped 释放 B-3 in_flight
    （原止损重试 5s 内被 B4 防双发抑制）
13. **R2-附加**：extreme 信号判定门槛可配置（extremeSignalThreshold，默认 0.9；
    原硬编码 0.6 低于 OFI 归一器设计常态区 0.5-0.8，为常态触发路径）；
    toxic_side 归属与 pred is_toxic 解耦（extreme 兜底触发也可带方向）

测试：ToxicNormalization 6 用例（VPIN 有界/均衡为 0/权重归一/realized 无内权/
vpin 独立触发/方向解耦），全量 82/84（2 既有环境失败）。

**回测标定与验证（_ec_5d，4 交易日 × 3 合约）**：

| 指标 | 基线(B+③) | R1 | R2(0.65) | R2(0.75 定稿) |
|---|---|---|---|---|
| 成交笔数 | 22,268 | 5,104 | 14,482 | 18,792 |
| dynbalance(0612) | 414,417 | 83,625 | 279,139 | 268,421 |
| TOXIC 事件 | 74 | ~5,311 | 4,092(全双边) | 2,784(284 单边) |
| error/HALT/zombie | 0 | 0 | 0 | 0 |

- 触发成分：2,880 次落在 combined 0.65-0.8 区间（非 extreme）；vpin 独立仅 2 次；
  方向归属生效（of/imb 同号时单边抑制）
- **固化配置**：adverseThreshold=0.75, vpinThreshold=0.60, extremeSignalThreshold=0.9,
  vpinWeight=0.5（src 权威已同步，dist 已部署）
- **已知权衡**：毒性保护降低回测 PnL（268k vs 基线 414k，-35%）--基线的"半瞎"检测器
  （ofi 恒 0+realized 稀释+trade 压半）穿毒性流赚报价；保护的尾部风险价值回测不可见，
  属实盘 A/B 裁决项。事件 Episode 聚集的本质是 OFI 归一器将常态映射至 0.5-0.8
  （信号 LEVEL 被用作 DEVIATION 分数），根治属 R4 信号重设计
- 0.65→0.75 敏感度低（分数密集区 0.65-0.85），阈值非关键路径

**R1+R2 部署状态**：dist/WtBtFutu 已部署（Debug 回测版）。生产 Release+远程部署
待用户确认（建议观察毒性单边抑制实盘表现后再上）。

### R3 实施记录（2026-08-22，已完成，回测零回归验证通过）

修改清单（全部完成）：
1. **SyntheticSignalFusion 整体删除**（710 行 + ToxicFlowDetector 的 feed*/runFusionCycle/
   onSyntheticAlpha + UftFutuMmStrategy 每 tick 空转调用）；扩展性裁决与三条可取
   思想记录于 R4 方案（用户确认）
2. **半接线清理**：SpreadArbitrageManager 的 setQuotingCallback/_quoting_callback/
   shouldPauseQuoting 删（保留 getQuotingAdjustmentForLeg 观测模式--QuotePolicyChain
   在用）；FutuPortfolio shouldPauseQuoting/shouldReduceQuoting 删
3. **adaptiveParam 模块删除**：updateAdaptiveParams 空函数/调用/配置节/ModuleParams
   字段/use_adaptive_param 传播链全删
4. **CorrelationManager 假接口删**：getSpreadSignals(返回{})/hasSpreadOpportunity
   (硬编码ratio)/getCorrelationsFor/getAggregateDelta/removeContract；保留相关性
   计算核心（getCorrelation/getHedgeRatio 在用）
5. **arb 假配置清理**：7 个无消费 _default_* 读点+字段（halfLife/correlationWindow/
   minCorrelation/lookbackWindow/pt entryZ/maPeriod/breakout）+ mmEnhancementWeight/
   useHybridStrategy + arb_close 的 price_offset_ticks/upgrade_to_taker/
   StrategyOverride 结构 + yaml 同步删除
6. **双层置信度统一**：executor 硬编码 0.5 -> 读 Manager 同一配置（minSignalConfidence
   单键，yaml 0.3→0.5）；Manager 闸门在 B-3 门前置 NONE，无 in_flight 卡死风险
7. **死方法群**：computeIntent/combineSignals/testCointegration/canOpenPosition 转发/
   getRiskSummary/getActiveAlerts/checkConvergenceFailure/getAllowedPositionSize/
   calculatePortfolioRisk/calculateVaR(×2)/checkCorrelationBreak/PortfolioRiskSummary
   结构/StatisticalArb 自适应链(recordOutcome/updateAdaptiveWeights/_performance/
   FeaturePerformance) -- 均零调用（逐项 grep 复核）
8. **PerformanceMonitor 接线**：warn/critical 阈值此前注入无消费 -> checkThresholds
   (1s 节拍, p99 超阈分级告警, _log_interval_ms 限频, 挂 StrategyCoordinator);
   P999=99 错误别名删
9. **PerformanceAnalyzer 占位修正**：_start_time 首笔成交锚定（trading_time_sec
   恒 0 修复）；determineMarketCondition 显式标注占位（待 R4 RegimeTracker）;
   recordQuote 参数标记有意不用
10. **L2 入口明示 TODO**：onOrderQueue/onOrderDetail 空实现标注（含 S9 双通道
    口径前置条件）
11. **杂项**：BilateralQuoteStats _last_minute_units 死字段；RealizedToxicity
    decay_factor/_latest_book/_has_book_data/onBookAnalysis 死链（含 ToxicFlowDetector
    转发与 detectEnhancedToxicity）；**TickTransactionInferer 数值泄漏修复**
    （add 全量/prune 乘 confidence -> 记录原始量对称增减，InferenceRecord+volume 字段）；
    SpreadCalculator::onTick 声明未定义死接口删（链接地雷，测试引爆发现）；
    误删的 ToxicFlowDetector analyze/onTickVolume/onTrade/getAvgAdverseMove 声明恢复
12. **TestUnits 补缺**：test_v8_r3_coverage.cpp 4 用例（SpreadCalculator 配对/
    z-score/A2 current_price、TickInferer 泄漏回归/方向分类）

测试：全量 86/88（2 个既有环境失败）。

**回测验证（_ec_5d vs R2 基线）**：18,524 笔（-1.4%，mocker 噪声内）；TOXIC 2,784
（与 R2 完全一致）；0 error/HALT/zombie；dynbalance 297,846（+11% -- inferer 泄漏
修复属合法行为变化：large_trade_ratio 不再被泄漏膨胀的分母稀释，trade 毒性通道
恢复满幅；其余清扫项行为中性）。

**遗留**：arb 合约乘数死亡链（loadConfig 不读→onTick 丢弃→calculator 恒 1）--
涉 arb 数学变更，无测试护栏前不动，归 R4；S6 安慰剂配置（momentum window/
LeadLag lagMs/scale 注释漂移）未处理。

1. **SyntheticSignalFusion 整体删除**（710 行）：feedTickInference/feedBookSignal/
   addSelfTradeCalibration 零调用、hasAnySource() 恒 false、runFusionCycle 每 tick
   空转调用（UftFutuMmStrategy.cpp:571-573 一并删）；ToxicFlowDetector::onSyntheticAlpha
   同步删除（其唯一调用方）
2. **MarketMakingEnhancer 半接线清理**：`enhanceMarketMaking` 空开关移除（config 键
   保留兼容但注释标 deprecated）或接线 -- 评估后定
3. **adaptiveParam 空函数删除**（StrategyCoordinator.cpp:1354-1363 空体 + 配置键）
4. **CorrelationManager 套利假接口删除**：getSpreadSignals 返回 {}、
   hasSpreadOpportunity 硬编码等半成品方法（保留相关性计算的真实消费部分）
5. **arb 假配置群清理**：读入无消费的键（correlationWindow/minCorrelation/maPeriod/
   breakoutThreshold/halfLife/mmEnhancementWeight/useHybridStrategy/price_offset_ticks/
   upgrade_to_taker/strategy_overrides）--未消费键启动 warn 改为删除+注释
6. **双层置信度阈值统一**：Manager 放行 0.3 vs 执行器丢弃 0.5 -> 单一配置键
7. **死方法群清扫**：computeIntent/combineSignals/testCointegration/
   recordOutcome+updateAdaptiveWeights(注: recordOutcome 在 R2 已真实接线, 保留)/
   checkConvergenceFailure/getAllowedPositionSize/getRiskSummary/getActiveAlerts 等
   零调用（逐一 grep 复核后删）
8. **PerformanceMonitor 死接线**：warn/critical/logInterval 注入后无逻辑读取、
   Percentile 枚举 P999=99 修正
9. **PerformanceAnalyzer 占位指标**：determineMarketCondition 恒 NORMAL、
   _start_time 恒 0、recordQuote 忽略参数 -- 修正或删
10. **L2 数据入口明示 TODO**：onOrderQueue/onOrderDetail (void)data 补 TODO 注释
11. **杂项**：BilateralQuoteStats _last_minute_units、RealizedToxicity decay_factor/
    book 数据、TickTransactionInferer 内部累计链数值泄漏、ISignalCombiner+Registry
    等占位
### R4 实施记录（2026-08-22，4/6 项完成，回测零回归验证通过）

已完成：
1. **A8 EMERGENCY 接入 halt 通道**：UftFutuMmStrategy::handleRiskAlert 对
   EMERGENCY 级 (arb 组合回撤超 portfolio_stop_loss) 执行
   haltTrading(IRREVERSIBLE, -drawdown) + _spread_arb_manager->disable() --
   套利组合止损线从"假保险丝"(只日志+广播)变为真实动作; 收盘 closeout
   保持 Fix4 豁免; 线程上下文已注释 (arb 线程回调, 原子写)
2. **A5 arb 时钟统一**：AsyncArbitrageExecutor 新增 setReplayNowUs(策略每 tick
   注入 _exchange_time_ms×1000)，替换 5 处墙钟 (同步路径/arb 线程循环/orphan
   入队与老化 ×2)；SpreadArbitrageManager position_open_time 改用 _now_ms；
   首帧前 (atomic=0) 兜底墙钟。回测中 TIMEOUT_EXIT/maxDivergenceTime/orphan
   超时自此可复现触发
3. **_last_mid 单一属主**（R2 项）：策略壳 MidSlot 成员/预填/写入删除，
   Coordinator 为唯一属主 (processTick tc.mid 写入)，新增 getLastMid 访问器；
   消费方 (perf analyzer/on_transaction 方向分类/spread_at_trade/closeout mid/
   self-trade calibrator) 全部改经访问器，时序等价
4. **P0-3 细粒度模式正式废弃**：FUTU_CALLBACK_LOCK=0 编译开关删除，回调大锁
   无条件化；UftFutuMmStrategy.h/EventDispatcher.h/TradingState.h 注释同步。
   依据: 生产恒用大锁、=0 存在 ≥6 处假无锁、保留开关=保留误开启陷阱

验证：全量 TestUnits 86/88（2 既有环境失败）；_ec_5d 回测 vs R3 基线：
18,530 笔（+0.03% 噪声内）、TOXIC 2,784（完全一致）、0 error/HALT/zombie、
dynbalance 291,878（-2% 噪声内）。预期零回归达成（arb 回测不触发故 A8/A5
无路径、_last_mid 时序等价、开关默认即 1）。

### R4b 待执行（两大结构重构，需独立会话 + 全额 A/B 预算）

**1. SpreadArbitrageManager 拆分（约 1459 行 → 三组件）**：
- `ArbCalcEngine`：SpreadCalculatorManager + strategies（纯计算，无状态副作用）
- `ArbGatekeeper`：B-3 门/两族 in_flight/CloseIntent/超时队列/信号冷却
  （从 applyB3Gate/onArbSignalDropped/onArbOrderFilled/超时清理抽出）
- `LegExecutionFSM`：PENDING→LEG1_SENT→LEG2_SENT→HEDGING→DONE，
  撤单/拒单/部分成交为迁移事件（替代散落 executor/bridge/manager 三处的
  隐式状态拼凑；A6/A7 孤儿腿对冲与方向逻辑重复随之收敛）
- 前置：arb 乘数死亡链修复（loadConfig 读 multiplier → manager 传递 →
  calculator 消费）一并纳入；回测开启 arb_close 灰度路径获得覆盖
- 验证：TestUnits 新增 Gatekeeper/FSM 表驱动用例 + arb 开启配置的回测 A/B

**2. computeAlpha 拆解（SignalAggregator.h:502-679 上帝方法）**：
- ICRecorder（信号-收益配对）/RegimeTracker（两套滚动 MA deque 归还
  ICWeightTracker）/ConfidenceCalculator 三协作者
- 删除非 const getContext()（S4 双写者），毒性结果改 update() 显式入参
- 吸收 Fusion 三思想（见上）：per-source confidence 连续加权（ConfidenceCalculator）、
  源级方向命中率（IC 低延迟补充）、合成交易流（L2 缺失补偿备选管道）
- 验证：signals 层表驱动单测（权重归一/regime 因子/IC 配对）+ _ec_5d A/B
  （R4 版为基线，alpha 语义变化需逐项归因）

## 已完成方案（2026-08-22②，V8 诊断报告 R5 收尾轮，已验证）

**背景**：R1-R4 完成后对报告全量 60+ 条目逐条代码级复核。确认已修复且执行准确的：
P0-1/P0-2/P0-3(正式废弃)/P0-4、T1-T7、S2/S3/S5、A1/A2/A3/A5/A8/A9/A10/A14、
R2(_last_mid 单属主)。**复核发现的剩余未修项**即本轮范围（R4b 两大结构重构
按既定裁决留待独立会话）。

**修改清单**：
1. **R3-残余（PerformanceAnalyzer 时钟双域）**：recordTrade 时间戳 =
   date 合成格式(~2e13) vs onTickUpdate = actiontime(~9e7) → `now > trade_timestamp`
   恒假，30s 低流动性强制过期分支死代码。统一为 replay `_exchange_time_ms`
   （FutuRuntimeOps 写入侧 + UftFutuMmStrategy onTickUpdate 调用侧），
   顺带修复 trading_time_sec 量纲（合成格式 /1000 ≠ 秒）
2. **R5（FutuConfig 读取语义）**：readDouble/readUInt32 区分"键缺失/VT_Null/
   空串/类型错(Object/Array)"→ 回落默认值（原 asDouble 空串→0.0，protectTicks
   空值会静默关价格保护）；readBool 支持数值型（YAML `1`→true，原 asBoolean
   只认 true/yes 字符串）；已全量扫描现行 yaml，无语义漂移键
3. **R6（loader fail-fast）**：contracts 缺失/非数组/为空 → error+return false
   （原静默通过零合约空跑）；anchorCode 空或不在 contracts 列表 → error+false
4. **R7（spread_at_trade 硬编码 0.2）**：兜底改查 `_contract_infos` 配置的
   tick_size×2，查不到记 0（spreadCaptured 有 <=0 守卫返回 0，统计口径
   不再掺默认值噪声）
5. **R4（TscClock 四项缺陷）**：now() 加 lfence 序列化；calibrate() 增加
   invariant-TSC cpuid 探测（非 invariant → error 日志+返回 false，不再
   静默 0.4）；新增 maybeRecalibrate 低频重校准接口（调用方低频路径）
6. **A11（NaN 护栏）**：calculateVolatilityFeature 的 `hist_vol < 1e-10` 判据
   对 NaN 恒假（n==10 时 0/0 滑过）→ 改 `!(hist_vol >= 1e-10)`；
   estimateHalfLife 分母零方差守卫（denom<1e-12 → return 0）
7. **A12（closeout 冻结桥）**：bridge 早退条件 isCloseoutTriggered →
   isCloseoutFlattening（仅 DRAINING/ASSESSING/EXECUTING 活跃平仓期暂停），
   FAILED/COMPLETED 后孤儿腿对冲与 in_flight 清理解冻（原冻结至下一 session，
   closeout 失败回退时裸腿持续暴露）
8. **A13（1:N 方向冲突）**：getArbCloseDirection any-match → 全量遍历，
   方向一致返回该方向，冲突返回 kConflict(2)；ArbCloseSyncPolicy 对冲突
   双侧抑制（原无序遍历首个 intent 静默丢信息）
9. **R1（side_pause 语义）**：注释澄清两字段镜像同一合约级熔断器
   （行为本一致，声明误导）
10. **S6（安慰剂配置）**：momentum window 接线（calculateMomentum 改用最近
    min(window,128) 收益，原固定全缓冲 128）；LeadLag lagMs 全链删除
    （未实现的安慰剂键：yaml/loader/Config 三处）；scale_factor 注释漂移修正
    （:198 "default 10000" 实为 3000）。**注意：momentum window 生效是真实
    信号行为变化**（50 vs 128 窗口），回测 A/B 归因记录
11. **S10（vol 阈值 EC 不可达）**：VolatilitySignalSource 增加低频 vol 分布
    埋点（info 级，可配置间隔，默认关）→ 回测取 EC realized_vol 分布 →
    按 p95/p99 标定 elevated/extreme 写回 coordinator.yaml
12. **性能顺手项**：EventDispatcher::dispatch 空 listener 早退（报告 §5#6）；
    SIGNAL_DECOMP debug 日志 %50 降采样对齐 toxicity（§5#2）

**测试**：新增 test_v8_r5_fixes.cpp — FutuConfig 空值/类型/布尔语义、
loader fail-fast、A11 NaN 护栏、momentum window 生效、A13 冲突双侧、
PerformanceAnalyzer 同时钟超时分支生效

**验证**：make WtFutuCore+TestUnits（基线 86/88，2 既有环境失败）→
_ec_5d 回测 A/B vs R4 基线（18,530 笔/TOXIC 2,784/dynbalance 291,878）→
预期：A11/A12/A13/R1/R3/R4/R5/R6/R7 回测路径零行为变化；momentum window
生效为唯一预期信号变化，归因记录

### R5 实施记录（2026-08-22②，已完成，回测验证通过）

修改清单 1-12 全部落地（见上），S10 标定结果：
- **实测分布**（_ec_5d, 4 交易日 × 3 合约, statsLogInterval=20 采样 n=18,496）：
  pooled p50=0.000148 / p95=0.000484 / p99=0.001173 / p99.5=0.001660 /
  max=0.003660 — 证实旧阈值 0.002/0.004 超出 4 日最大值, 分档不可达
- **固化配置**：elevatedThreshold=0.0005(≈p95) / extremeThreshold=0.0017(≈p99.5)，
  src 权威+dist 已同步; 代码默认值 (SignalAggregator/VolatilitySignalSource) 同步;
  statsLogInterval 键保留 (默认 0 关闭, 标定工具)
- **生效证据**：回测日志 `[DIAG] shouldPause=true vol_tier=3 realized_vol=0.003193`
  — EXTREME 分档首次真实触发 (18 次/4 日, 尾部频率合理)

测试：新增 test_v8_r5_fixes.cpp 11 用例（FutuConfig 语义×2 / loader fail-fast×4 /
A11 NaN 护栏×2 / momentum window / 分析器时钟过期 / TscClock 校准）全过，
全量 97/99（2 个既有环境性失败，0 回归）。

**回测 A/B（_ec_5d vs R4 基线）**：

| 指标 | R4 基线 | R5 | 归因 |
|---|---|---|---|
| 成交笔数 | 18,530 | 17,788 (-4.0%) | S10 vol 闸门生效(±widen/pause)+momentum window 50 生效, 两项预期行为变化 |
| TOXIC 抑制 | 2,784 | 2,770 (-0.5%) | 毒性链路无变化 ✓ |
| dynbalance(0612) | 291,878 | 306,882 (+5.1%) | vol 高位拉宽减少逆向成交, 方向合理 |
| error/HALT/zombie | 0 | 0 | ✓ |

**部署状态**：dist/WtBtFutu 已部署（Debug 回测版，含 R1-R5 全部）。
生产 Release+远程部署待用户确认（A12 closeout 解冻与 A13 冲突双侧抑制
改变实盘边缘行为, 建议随 R1+R2 同批观察窗口评估）。

**明确不做（移交 R4b/后续）**：S4/S7/S8（computeAlpha 拆解）、A4-残余
（腿间配对双重标准）/A6/A7（LegExecutionFSM）、arb 乘数死亡链、S9
（TradeFlow 双通道，L2 前置）、§5 其余性能项、README v7.x 架构描述同步
（待 R4b 后一次性更新）

## 已完成方案（2026-08-22③→23，V8-R4b 功能性修复+适度重构，已验证）

**背景**：R1-R5 已提交（05dd475c）。用户裁决 R4b **不做过度拆分类/文件**，
功能性修复可评估执行。原 R4b 两大结构重构据此重切分为三档。

### A. 功能性修复（建议执行，均原位修改不新增文件）

**A-1 arb 乘数死亡链**（报告 §4 遗留）
- 证据链：loadConfig 不读 multiplier；bridge pushTick 硬编码 1.0
  （ArbExecutionBridge.cpp:50）；SpreadCalculator 无 setLegMultipliers、
  ctor 恒 1.0（SpreadCalculator.cpp:29）；SpreadPairConfig 默认 300 烟雾弹
  （SpreadArbitrageTypes.h:135）；setContractMultiplier 零调用
- 方案：①pair yaml 增 leg1Multiplier/leg2Multiplier（默认 1.0）loadConfig 读取；
  ②SpreadCalculator 增 setLegMultipliers，addSpreadPair 接线；
  ③SpreadPairConfig 默认 300→1.0；④删除每 tick multiplier 死参数链
  （ArbTickData/pushTick/两级 onTick 签名）与 setContractMultiplier/
  _contract_multipliers 死字段
- 影响：不配乘数恒 1.0 行为不变；z-score 对常量缩放不变 → 同品种跨期
  即使配上乘数也不改信号，仅修正 spread 绝对口径/跨品种 pair

**A-2 A6 孤儿腿对冲 fire-and-forget**
- 证据：bridge 回调 rejected/rate_limited/self_trade_blocked/price=0 仅日志
  （ArbExecutionBridge.cpp:318-345），executor 一次性移出 deferred
  （AsyncArbitrageExecutor.cpp:693 swap）；对冲量恒 leg1_qty（ratio≠1 错量）；
  ctx fallback 用 leg1 价下 leg2 单
- 方案：①hedge_qty 按 ratio 修正（leg1_qty×leg1_ratio/leg2_ratio，OrphanLeg
  增 ratio 字段）；②回调返回受理状态，拒绝/限流/无价时 leg 保留 deferred
  并记 retry_count，超上限（3 次）升级 error + 放弃（防死循环）；
  ③对冲定价统一由 bridge 回调取 leg2 盘口（executor 不再传 leg1_price 兜底）
- 风险：arb 路径回测无覆盖（arb_close 出厂 disabled），靠单测+实盘灰度

**A-3 A7 残余：双 in_flight 无差别清零**
- 证据：onArbSignalDropped 同时清 open+close 双 in_flight
  （SpreadArbitrageManager.cpp:983-995）；onLegCancelled 调它
  （ArbExecutionBridge.cpp:469-483）→ close 在途时 open 侧事件误清 close 闸门，
  部分成交+撤单场景提前放行
- 方案：释放语义按通道拆分 — onArbSignalDropped 只清 open in_flight；
  新增 onArbCloseDropped 清 close in_flight；onLegCancelled 按 pair 当前
  实际在途通道精确释放（各自检查 >0.5 才清）
- 报告 A7 的"平仓方向 3 处重复实现"属结构性，只做注释交叉引用不合并
  （合并需动 executor/bridge/manager 三处语义，收益低于风险）

**A-4 A4 残余：PairsTrading 私有历史配对双标准**
- 证据：calculator 已 fresh-pairing（SpreadCalculator.cpp:44-79），但
  PairsTradingStrategy::updatePrices 每次 manager update 都 push
  （state.leg1_price, state.leg2_price），任一腿 tick 进样本 → OLS beta 被
  陈旧价污染
- 方案：update() 按 state.last_update（calculator spread 样本时间戳）去重，
  未更新跳过 push（需确认 getState 透传 _last_update，不透则补字段）

**A-5 S8：归一化含未启用信号 + BOOK regime 硬编码**
- 证据：computeWeights 固定 5 entries（ICWeightTracker.h:394-462），禁用信号
  base weight 仍进 raw_sum → cap-after-normalize/调试占比失真；BOOK DEEP/THIN
  1.3/0.7 硬编码（:524-527）
- 方案：computeWeights 增 enabled_mask[5]（SignalAggregator 按 _sources 实际
  装配传入），禁用项 w=0 不进 raw_sum；BOOK 两因子进 Config
  （book_deep_factor=1.3/book_thin_factor=0.7 默认现值 → 行为零变化）
- 影响：当前配置 5 信号全启用 → 归一化结果不变，仅语义修正

**A-6 S4：SignalContext 双写者**
- 证据：StrategyCoordinator.cpp:879 非 const getContext() 直写 toxicity.*
  （:954-963）；非 const 重载全仓仅此一处消费
- 方案：SignalAggregator 增 updateToxicity(score, side, valid) 显式入口，
  coordinator 改调用，删除非 const getContext() 重载（纯搬运零行为变化）

### B. 结构改进（v2：按业务归属的适度重构，用户裁决"不完全放弃、避免过度拆分"）

实测现状：Manager 1324 行（R3 清扫后），分节已清晰（装配 :49-338 /
信号生成 :339-561 / B-3 门 :707-982 / in_flight 回调 :983-1118 /
B1 intent :1119-1190 / B5 过冲 :1191-1286 / B6 聚合 :1287-1324）；
computeAlpha 175 行 = 槽位提取 + IC 记录(:531-553) + regime MA 检测(:559-577)
+ 权重计算 + 聚合/fallback/confidence 五段。

**S-1 regime 检测归还 ICWeightTracker**（S7 核心，真层次修复）：
_mid_ma_short/long + MA 和 + MarketRegime::detect 调用移入 ICWeightTracker
（新接口 detectRegime(vol_percentile, mid, avg_depth)，内部维护 MA 状态）。
Layer2 输入与权重框架同层 — 这是业务归属修正而非机械拆分。同序同值，
零行为变化。

**S-2 IC 记录段随 S-1 同车迁移**：recordSignal/recordReturn/updateIC +
_mid_history_for_ic/_tick_counter 收拢为 ICWeightTracker::recordTick
(slot_vals, mid) 单一入口（同为权重框架内部簿记）。

**S-3 computeAlpha 线性化**：S-1/S-2 移出后剩 ~100 行已线性
（提取→权重→聚合→fallback→confidence），三协作者拆分不再需要。

**S-4 Manager 文件级二分**（同 class 两翻译单元，零 API 变化）：
loadConfig/initializeStrategy/addSpreadPair/removeSpreadPair（~300 行纯装配，
非热路径）→ SpreadArbitrageManagerInit.cpp；运行时文件 ~1000 行，
保留 B-1/B-3/B-5/B-6 分节。收益：装配与运行时分视口，风险：零（纯搬运）。

**S-5 in_flight 状态表注释**：Manager.h 两族 in_flight 字段处补全
写入/清理点全列表（替代 FSM 的可见性收益，无代码风险）。

**放弃项及理由**：LegExecutionFSM/ArbGatekeeper/ArbCalcEngine 类拆分 —
执行状态散布 executor/bridge/manager 是真实复杂度，但 arb 无回测覆盖，
搬移无法验证等价性；A-2/A-3 原位修复后残余风险可接受，待 arb_close
灰度开启有覆盖后再评估。ConfidenceCalculator 类不做（S-1/S-2 后
confidence 段仅 ~15 行）。

### C. 明确不做（v2 裁决）

- ArbCalcEngine/ArbGatekeeper/LegExecutionFSM 三组件拆分（见 B 放弃项理由）
- ICRecorder/ConfidenceCalculator 独立类（S-1/S-2 收拢后无残量）
- SyntheticSignalFusion 三思想吸收（per-source confidence 已有 ICWeightTracker
  等价物；其余随子系统已删）

### 测试与验证

- TestUnits 新增：A-1 乘数接线（computeSpread 含乘数）、A-2 拒单重试保留、
  A-3 双通道精确释放、A-4 配对去重样本数一致、A-5 禁用信号归一和=1
- S-1/S-2 迁移等价性：regime 序列/IC 更新节拍逐值比对（同输入同输出）
- 全量回归 + _ec_5d A/B 预期零行为变化（乘数恒 1/5 信号全启用/arb 回测
  disabled/A-6 纯搬运/S-1/S-2 同序同值/S-4 纯搬运），出现差异即归因
- A-2/A-3 实盘行为变化点：孤儿腿对冲会重试、in_flight 释放更精确 — 部署
  后观察窗口重点项

### R4b 实施记录（2026-08-23，已完成，回测零回归验证通过）

修改清单（全部完成）：
1. **A-1**：pair yaml 增 leg1Multiplier/leg2Multiplier（默认 1.0）；
   SpreadCalculator 增 setLegMultipliers 并在 addSpreadPair 接线；
   SpreadPairConfig 默认 300→1.0；删除每 tick multiplier 死参数链
   （ArbTickData/pushTick/两级 onTick 签名）+ onWtTick 死接口 +
   setContractMultiplier/_contract_multipliers 死字段
2. **A-2**：OrphanLeg 增 hedge_qty（计划 leg2 量，含 ratio）/retry_count/
   last_attempt；OrphanHedgeCallback 改返回受理状态、删 price 参数
   （定价全部归 bridge 的 leg2 盘口快照，executor 不再传 leg1 价兜底）；
   失败保留重试（500ms 节流、3 次上限后 error 放弃）；bridge 回调对
   rejected/rate_limited/self_trade_blocked/无盘口价返回 false；
   enqueueOrphanLeg/orphanLegsDeferredCount 测试接缝
3. **A-3**：onArbSignalDropped 增 is_close 按通道释放；新增 onArbLegCancelled
   只清实际在途（>0.5）通道；executor 6 处调用点传 is_close_signal(type)，
   bridge 3 处传 order.is_close，onLegCancelled 改新接口。
   **A-3b 误报记录**：曾疑 in_flight 超时比较 µs 对 ms 常量（1000×），
   复核 applyB3Gate 入口有 current_time_us/1000 换算，ms 域一致，无 bug，
   已回退改动 — 教训：改超时逻辑前先确认时间域换算点
4. **A-4**：PairsTradingStrategy::update 按 state.last_update 去重
   （calculator fresh-pairing 同标准）；priceSampleCount 测试钩子
5. **A-5**：computeWeights 增 enabled[5] 掩码（禁用信号 w=0 不进分母，
   consistency 同步排除）；BOOK regime 因子进 Config（book_deep_factor=1.3/
   book_thin_factor=0.7 默认现值）；Aggregator 装配末尾构建 _wtype_enabled
6. **A-6**：SignalAggregator 增 updateToxicity 显式入口，删除非 const
   getContext() 重载；coordinator 两分支改调用（else 分支现同步刷新
   score/side，消费方仅读 toxic_detected，行为中性）
7. **S-1/S-2**：AdaptiveWeightFramework 新增 processTick（IC 簿记+regime
   MA 检测+computeWeights 单入口）与 resetTickState；SignalAggregator 删除
   _tick_counter/_mid_history_for_ic/_mid_ma_* 成员，computeAlpha 175→~110 行
8. **S-3**：computeAlpha 经 S-1/S-2 收拢后已线性，无需进一步拆分
9. **S-4**：SpreadArbitrageManagerInit.cpp（同 class 第二翻译单元）=
   loadConfig/initializeStrategy/addSpreadPair/removeSpreadPair ~270 行纯装配；
   主文件 1089 行运行时
10. **S-5**：PairArbState 两族 in_flight 写入/清理点状态表注释；
    executor executeSignal 平仓方向 3 处实现交叉引用注释（A-7 残余收尾）

测试：test_v8_r4b.cpp 8 用例（乘数接线×2/对冲重试×2/通道精确释放/
配对去重/禁用归一/processTick 复位）全过；全量 105/107（2 既有环境失败）。

**回测 A/B（_ec_5d vs R5 基线）**：17,771 笔（-0.1% 噪声内）、TOXIC 2,770
（完全一致）、dynbalance 306,882（逐比特一致）、0 error/HALT/zombie —
预期零行为变化达成。

**部署状态**：dist/WtBtFutu 已部署（Debug 回测版）。A-2/A-3 改变 arb 实盘
边缘行为（对冲重试/闸门精确释放），arb_close 灰度开启时需重点观察。

## 已知外部限制（框架层，禁止越界修复）

- **WtBtCore/HftMocker.cpp 回测不可复现**：`splitVolume()` 用 `srand(time(NULL))`
  随机拆分成交量、`genRand()` 用 `srand(getCurMin())`、`makeLocalOrderID()` 用墙钟播种
  → 同配置两次运行成交拆分序列不同，策略轨迹有随机性（幅度随交易频率放大）。
  评估策略表现需接受该噪声或多次运行取均值。
- `AsyncArbitrageExecutor` orphan leg 超时用 `steady_clock`（罕见路径，同理不可复现）。
- `WtUftEngine::on_session_end` 生产不触发（WtUftTicker.cpp:183）。双边统计不能依赖
  框架 session_end 做收盘 flush；策略侧已绕开：section-break + closeout TRIGGERED
  定点 flush + 周期 live 输出 + 当日文件 seed 重启续算。
- **柜台账户资源类拒单缺少分类通道（2026-08-17 事故，待框架修复）**：
  CTP 50=平今仓位不足 / 51=平昨仓位不足 / 31=资金不足，在多策略共享账户下是常态
  （账户级可平量与保证金被其他策略持仓/挂单占用）。框架 `on_entrust` 只传
  success+message 文本，无错误类型字段 → 策略层只能通用计数，达到阈值即 HALT
  误杀。且框架 ActionPolicy 的平今/平昨映射基于自身持仓明细，与柜台账户级校验
  错位（账户持仓被其他策略挂单冻结时可平量 < 明细）。正确修复点：
  TraderCTP/TraderAdapter 对该类拒单做分类标记（或触发持仓/资金重查后降级），
  不应让策略层解析柜台错误码/文本。
- **成交审计账与事件流不一致（2026-08-17 发现，待框架排查）**：
  实盘 trades.csv 出现同秒重复记录（SELL×2-4）且 BUY 方向大量丢失，
  与 on_trade 事件流（本地持仓驱动，自洽）矛盾；疑似框架 CSV 落地路径问题。
- **stra_cancel_all 的 code 匹配口径回测/实盘不一致（2026-08-21 发现，禁止越界修复）**：
  实盘 `TraderAdapter::cancelAll(stdCode)` 用 `getFullCode()`（"SHFE.ao2609" 两段式,
  WTSContractInfo.hpp:184）匹配入参，策略传 stdCode（"SHFE.ao.ao2609" 三段式）**永不匹配、
  静默全不撤**；回测 `UftMocker::stra_cancel_all` 用 `_code`（stdCode）匹配能撤到。
  策略层调用 stra_cancel_all 兜底时必须传 fullCode 格式或空串（全撤）。

## 框架层已打补丁（越界修改记录，2026-08-03，GUI 监控接入需要）

- **`src/WtUftCore/UftStraContext.cpp`**（2026-08-21，B+ 期间顺手修改，本次补记）：
  `DATA_SIZE_STEP` 8000 -> 200000。原因：做市场景单日订单/成交/明细量可达数万，
  8000 步长的 mmap 块（order/trade/round/detail 四个 .membin 文件）频繁扩容重映射。
  内存影响：四块各 ~12-17MB 磁盘（mmap 惰性分页，RSS 按触碰量计）。
  上游修复（按量自适应扩容）后可还原。
- **`src/WtUftRunner/WtUftRunner.cpp`**：`config()` 尾部补 `initEvtNotifier()` 调用。
  原因：框架缺陷--`initEvtNotifier()` 定义存在但从未被调用（对比 WtRunner.cpp:241 /
  WtRtRunner.cpp:654 均有调用），导致 config.yaml 的 `notifier` 段被静默忽略，
  EventNotifier 不发布任何 MQ 事件，WtMonSvr GUI 无法接收实时订单/成交/日志。
  上游修复后可还原。重建流程：`dist/WtRunnerFutu/rebuild_release.sh`。
- **`dist/WtRunnerFutu/libWtMsgQue.so`**：EventNotifier 按 CWD 优先查找 MQ 模块，
  需将该库置于 runner 工作目录（从 dist/bin 拷贝）。

## 当前方案（2026-08-23，去大锁（_cb_mtx）低延迟等价设计，V9 诊断产出，待用户确认后执行）

### 背景

- `_cb_mtx`（UftFutuMmStrategy.h recursive_mutex）串行化全部 13 个回调入口。
  其真实代价不是平均开销（回测单线程恒无竞争 ~20ns），而是**尾延迟**：
  实盘 on_trade 到达时若 MdSpi 正在 processTick（refreshQuotes 锁内发 CTP 单
  可达 µs~ms 级），TdSpi 整笔成交处理被阻塞到 tick 结束，劣化 p99/p999 的
  quote→fill 与 fill→requote 时延。
- 目标：**消除跨线程阻塞、快路径零新增开销、回测行为逐比特不变**。
- 教训引用：V8-R4/P0-3 删除 FUTU_CALLBACK_LOCK=0 细粒度开关——失败根因是
  没有跨线程写点清单就动手（≥6 处假无锁）。本方案第一交付物即写点清单。

### 设计原则（属主化 + 快照 + 投递，而非撒胡椒面加锁）

1. **每个可变对象唯一属主线程**：非属主要么不碰、要么经快照读
   （atomic/seqlock/generation 门控拷贝）、要么投递命令给属主线程。
2. **快路径零新增成本**：热路径不新增锁竞争点与 atomic RMW；读侧只允许纯 load
   （acq/rel 在 x86 免费），写侧只在低频路径。
3. **慢操作不阻塞快线程**：罕见重操作封装为命令，在属主线程检查点执行。

盘点结论：90% 跨线程保护已由各模块自带小锁承担（quoter/tracker 自旋锁、
bridge/orchestrator _lock、arb manager 三域 atomic_flag、GLFT seqlock、
MidSlot 原子、CachedQuote 小锁、TradingState CAS），_cb_mtx 是冗余兜底层；
真正裸奔的只有 RiskMonitor halt 域（P1-1 现状真竞态——handleRiskAlert 是唯一绕开
_cb_mtx 的写者）；_closeout_state 当前全部读写点在持 _cb_mtx 回调内
（2026-08-23 复核裁定非现状 bug），拆锁后需随 WS-A 一并收编。

### 跨线程写点清单（现状盘点）

| 共享状态 | 写者/线程 | 现有保护 | 缺口 |
|---|---|---|---|
| FutuQuoter 槽位 | Md: refresh/cancelAll；Td: onOrder/onTrade/onEntrustAck | 自带全方法 RecursiveSpinLock | 无 |
| UnifiedOrderTracker | Md: checkAutoCancel；Td: track/untrack/recordFill | 自带锁 + generation 号 | 无 |
| SpreadOptimizer 参数 | tick 线程 drain 写 / Md 读 | F20 seqlock | 无 |
| ArbExecutionBridge / CloseoutOrchestrator | Md: onTick；Td: fill/order 事件 | 各自 _lock | 无 |
| RiskMonitor 频控环/zombie 闩锁/告警节流 | Md+Td 双写 | _rate_lock 等 atomic_flag | 无 |
| **RiskMonitor halt 域**（_halt_category/_halt_timestamp/_halt_pnl_snapshot/_recovery_count/_was_loss_triggered） | Md(checkRisk)、Td(onEntrust/onChannelLost)、**arb 线程(handleRiskAlert，唯一绕开 _cb_mtx 的写者)** | **裸奔 = P1-1 现状真 UB** | WS-A |
| RiskMonitor closeout 状态（_closeout_state） | Md(CloseoutTrigger/Orchestrator.onTick)、Td(onOrderEvent) | 全部调用方在持 _cb_mtx 回调内（复核裁定：非现状 bug） | 拆锁前置，并入 WS-A |
| FutuPortfolio | Md: markToMarket/读快照；Td: onTradeFill/setShadow/resync | 全方法 RecursiveSpinLock（FutuPortfolio.h:339，复核实测） | 无正确性缺口；WS-B 仅作可选延迟优化 |
| Coordinator CachedQuote / _last_mid | Md 写 / Td 读 | RecursiveSpinLock / MidSlot 原子 | 无 |
| 杂项 map（_halt_quoting_state=Md 专属 / _last_requote_ms=Td 专属 / _last_bilateral_log_ms=Md 专属） | 各自单线程 | 无需 | 保持归属即可 |

### 修改清单（六个工作流，按依赖顺序）

1. **WS-A 前置修复：停机域原子化（即 V9 报告 P1-1/P1-2）**
   - `FutuRiskMonitor.h/cpp`：`_halt_category` 改 `std::atomic<RiskCategory>`；
     `_halt_timestamp/_halt_pnl_snapshot/_recovery_count/_was_loss_triggered/
     _closeout_state(retry_count 等)` 收进一个 `atomic_flag` 临界区
     （复用 `_rate_lock` 成熟模式）；`getHaltCategory/checkAndRecover/canRecover`
     读侧经同一临界区或原子 load。
   - 硬前置理由：否则拆大锁后该域从"大锁兜底"变"真竞态"。

2. **WS-B Portfolio 快照优化【复核降级：可选项，非正确性前置】**
   - 复核修正：Portfolio 已是全方法 RecursiveSpinLock（getTotalDelta/
     getContractSnapshot 等均持锁，FutuPortfolio.h:339 实测），去大锁后不产生
     正确性缺口；"写者只有 TdSpi"契约不成立（markToMarket=Md 写价格），实为
     双写者分字段。
   - 若做，范围收缩为：(a) cache-line 拆分 markToMarket 价格域与记账域（消伪共享，
     低风险）；(b) seqlock 化 getContractSnapshot——收益存疑：无竞争自旋锁 ~20ns vs
     seqlock 两次 acquire load 差距小，且 getTotalDelta 内含 refreshAggregates
     重计算，写中重试复杂度高。默认不做，留性能轮评估。

3. **WS-C TradingState 单写者收敛（启用既有 C10 EventDispatcher 计划）**
   - 低频处置类转移（closeout 进入、zombie halt、MARKET/TOXICITY 进入等）
     改为事件投递 → SPSC ring → tick 线程唯一 TradingStateWriter 执行 CAS 转移。
   - 恢复类语义敏感转移（resumeFromRisk/unblock 系列）保留原线程内 CAS 直写白名单。
   - 完成 EventDispatcher.h 注释中"15 写点/6 类 → 1 集中写者"的既定收敛。

4. **WS-D 成交路径保持同步（不迁移！）**
   - B+ 事件驱动补挂 requoteAfterFill 是延迟敏感特性，留在 TdSpi 同步执行；
     它已全部依赖小锁（quoter/CachedQuote/tracker/RiskMonitor 只读检查），
     去大锁后天然无阻塞。仅审计其触达的 coordinator 单线程 map 归属不被破坏。

5. **WS-E 罕见重操作命令化**
   - on_channel_ready/on_channel_lost/on_session_begin/on_session_end/
     zombie 升级兜底 cancelAll 这类跨模块序列不再要求与大锁互斥：
     封装为 PendingCommand，入队（atomic flag + 小 ring）；
    - 在 processTick 顶部与 processTradeFill 入口各加检查点
      `drainPendingCommands()`：一次原子 load + 分支，无命令时 ~1ns；
    - 【复核补充·必选】行情静默期兜底：tick/fill 双停时检查点不触发，命令化后的
      channel_ready 序列会无限滞留（现状=TdSpi 内联立即执行）。三选一：
      (a) 检查点扩展至全部回调入口（断连场景 on_order/on_entrust/on_channel 本就
          高频到达，覆盖面足够）；(b) 复用框架 RtTicker 定时线程低频 drain（~500ms）；
      (c) 命令入队即记超时戳，processTick 惰性检查 + on_channel_lost 即刻自 drain。
      默认取 (a)+(c)：零新增线程，静默期由订单/通道回调天然驱动。
   - 通道恢复序列从"TdSpi 内联长事务"变为"下一 tick 边界执行"，
     MdSpi/TdSpi 不再互相等待；会话切换（RtTicker 线程）同理收编。
   - 语义注意：channel_ready 后的首 tick 前 drain 保证同步先于报价恢复，
     时序与现状等价（现状也是恢复完成后才 resume）。
   - 【收官修订 2026-08-23⑥】"发布者自身即 drain 点"统一策略不成立：
     Session 命令在实盘由 WtUftTicker 线程发布，自 drain 会触达 Md 属主无锁
     状态（coordinator resetSession/quote_chain）。已落地为**属主域拆分**：
     Channel=Td 域（发布者自 drain + Td 检查点兜底），Session=Md 域
     （实盘 ticker 只投递、on_tick 消费；回测单线程 tid 相等自 drain）。
     详见文末"收官实施记录"。

6. **WS-F 渐进开关与移除**
   - 编译开关两态 `FUTU_CB_LOCK=big|none`（措辞修正：原"三态"笔误，实际两态；
     默认 big 保底与历史基线一致；本次 unlike P0-3 之处：先有写点清单+每步 TSAN）；
   - 验收标准追加【复核补充】：PerformanceMonitor 增加 refreshQuotes 持锁时长埋点
     （p99 持锁时长 + TdSpi onOrder 因该锁阻塞次数）——去大锁后 quoter 自旋锁从
     "冗余兜底"变真实竞争点（Md 持锁穿越引擎发单 vs Td onOrder），切 none 前
     必须量化确认无恶化，必要时将 stra_* 发单移出临界区（决策锁内/发送锁外）；
   - 全部 WS-A~E 合入并验证通过后，删 big 分支与 _cb_mtx 成员（一次性提交）。

### 热路径成本预算（证明"不影响低延迟"）

| 位置 | 新增成本 | 说明 |
|---|---|---|
| on_tick 入口命令检查点 | 1 次 atomic load（~1ns） | 无命令零分支预测损失 |
| on_trade 入口命令检查点 | 同上 | |
| TradingState 事件投递 | 仅低频处置路径，SPSC push ~10ns | 快路径状态查询仍是纯 atomic load |
| 停机域读 | atomic load / flag 临界区（低频） | checkAndRecover 每 tick 一次可接受 |
| 【复核补充】quoter 自旋锁竞争显性化 | 非新增成本，为风险项 | 去大锁后 Md refreshQuotes(持锁穿越引擎调用) 与 Td onOrder 直接竞争；以 WS-F 持锁埋点验收 |

（复核删行说明：原"Portfolio seqlock 快照读"行随 WS-B 降级为可选而移除——
现状全方法递归锁保持不变。）

净效果：tick-to-quote 主链路**零新增锁、零新增 RMW**；
TdSpi fill 路径不再等待 MdSpi（消除最大尾延迟源）。

### 明确不做（反模式）

1. 不把 13 个回调 MPMC 队列化——破坏 tick-to-quote 同步语义与回测可复现性，
   且引入 µs 级排队延迟，属负优化。
2. 不做逐变量散锁（P0-3 失败路径重演）——只按属主域收编。
3. 不引入 lock-free hash map 全家桶——现有 wt_hashmap 单属主化即可。
4. 不在本轮动 arb 线程内部结构（R4b 另行处理）。

### 实施顺序与验证

1. WS-A 单独合入：TestUnits 双线程 hammer 用例（4 线程并发 halt/recover/closeout
   转移 ×10^6 次，断言无 torn state）；_ec_5d 回测零回归。
2. （WS-B 已降级可选：默认跳过；若启用，seqlock 单测 + 回测逐比特 A/B。）
3. WS-C/E 合入：TSAN 构建跑 TestUnits 全量 + 回测；事件时序用例
   （channel_lost→cancelAll→ready→resume 序列断言）。
4. WS-F 切 none：回测逐比特 A/B（必须与 big 完全一致）→ 生产灰度：
   先观察一个交易日 p99/p999 quote-to-fill/fill-to-requote 分布对比再固化删除。
5. 每一步均遵守本文件 §1 流程：方案先行、用户确认后动手。

### 回退策略

WS-F 之前任意步骤发现行为差异：编译开关切回 big 即回到当前基线；
WS-F 删除 _cb_mtx 的提交单独成 commit，revert 即完全回退。

## V9 诊断报告（2026-08-23，只读诊断未改代码；诊断基线=R4b 提交 68da2ea8 之前的工作树，已按同日独立复核修订——凡 [复核] 标记处为准）

**诊断方式**：逐文件深读约 30 个核心源文件（StrategyCoordinator/FutuQuoter/
QuotePolicyChain/FutuRiskMonitor/RiskCoordinator/UnifiedOrderTracker/
FutuRuntimeOps/UftFutuMmStrategy/ArbExecutionBridge/CloseoutOrchestrator/
SignalAggregator/SpreadOptimizer/AsyncArbitrageExecutor/TscClock/LockFreeQueue
等）+ 配置/文档交叉核对。已对照 R1-R5 已修复项，只报告仍存在的新发现。
本节为 V9 全量存档；其中可修项已提炼为下方"去大锁方案"与 R6 待执行清单。

### 一、总体评价

| 维度 | 评价 |
|---|---|
| 架构分层 | ★★★★☆ 壳层/装配(wireDeps+validateDeps fail-fast)/协调(PolicyChain)/领域 五层清晰 |
| 并发安全 | ★★★☆☆ 回调大锁+per-module 自旋锁体系成型；风控停机状态域仍有成片无同步跨线程共享 |
| 报价/风控分离 | ★★★★☆ delta(策略)/position(风控) 语义边界落地且有单测护栏 |
| 执行链路 | ★★★★☆ B+ 订单槽状态机严谨 |
| 套利子系统 | ★★★★☆ [复核修订] R4b(A1-A6/S1-S5)已落地（乘数链修复/单写者恢复/文件级二分）；残余=组件级 FSM 收敛评估 |
| 文档 | ★★☆☆☆ README 仍写 v7.7/1228行（实际 v8-R5/1513行） |

**[复核修订]**：R1-R5/R4b 后无 P0 级新发现；剩余问题集中在 ①风控 halt 域线程
同步缺口(P1-1，唯一现状真竞态) ②去大锁工程(独立方案见上节) ③文档失真
④热路径少量隐性开销。

### 二、Bug 修复建议（按严重度）

#### P1 级

1. **P1-1 FutuRiskMonitor 停机状态域跨线程无同步（数据竞争 UB）**
   - 证据：FutuRiskMonitor.h:578-588 —— `_halt_category`(enum)/`_halt_timestamp`/
     `_recovery_count`/`_halt_pnl_snapshot`/`_was_loss_triggered` 全部非原子。
   - 写者三线程：①MdSpi RiskCoordinator::checkRisk→haltTrading(RiskCoordinator.cpp:196)
     ②TdSpi onEntrust(FutuRuntimeOps.cpp:876)/onChannelLost(:1007)
     ③arb 线程 handleRiskAlert EMERGENCY(UftFutuMmStrategy.cpp:205-207，
     注释自称"原子写"但 FutuRiskMonitor.cpp:465 实为裸赋值)。
   - 读者：MdSpi checkAndRecover/canRecover(cpp:528/:538/:605)。
   - 影响：IRREVERSIBLE 标志可能被并发读到中间态 → 日亏线被自动恢复绕过。
     这是系统最后一道保险丝。修法：atomic 化 + atomic_flag 临界区（=去大锁 WS-A）。
2. **[复核改判] P1-2 CloseoutSubInfo —— 非现状 bug，改为去大锁前置修复项**
   - 复核核实：transitionCloseoutSub/markCloseout*/checkCloseoutRetry/resetCloseout
     的全部调用方（CloseoutTrigger←processTick、Orchestrator.onTick/onOrderEvent/
     executeHedge/finalizeAtSessionEnd、resetCloseout←onSessionBegin）均在持
     _cb_mtx 回调内，当前读写全串行、无竞态。原报告列 P1"数据竞争 UB"高估。
   - 正确定位：_cb_mtx 移除后即成真竞态 → 并入 WS-A 同一临界区收编。
3. **[复核降级 P2] CoordinatorConfig::_raw_variant 悬垂指针隐患（防未来误用）**
   - StrategyCoordinator.h:188 保存 WTSVariant* 裸指针供装配期 **8 处**使用
     （FutuComponentFactory×5 + FutuModuleAssembler×3；原文"9 处"系计数误差），
     均在 variant 存活期内，当前无实际悬垂。属防御性整洁项：
     wireDeps() 尾部置空并注释 init-only。

#### P2 级

| # | 位置 | 问题 | 建议 |
|---|---|---|---|
| P2-1 | StrategyCoordinator.cpp:557 | 夜盘恢复分支裸调 _risk_monitor->getCloseoutSubInfo() 无空指针守卫（同函数其他分支均有），风格不一致 | 补守卫 |
| P2-2 | StrategyCoordinator.cpp:910-912 | qphase==MARKET 期间每 tick 重发 cancelAll(ctx)，首轮后全部 pendingCancel 仍空转抢 quoter 自旋锁 | 边沿触发（参照 LIMIT-TOUCH _touch_active 模式） |
| P2-3 | RiskCoordinator.cpp:83-84 | taker 减仓单用 Source::CLOSEOUT——污染 closeout 统计口径且隐式落入 Fix4 REVERSIBLE 豁免集合；A9 刚为孤儿腿修过同类冒用 | 新增 Source::RISK_REDUCE，排查 closeout inflight 守卫误数 |
| P2-4 | UftFutuMmStrategy.cpp:693-694 | on_transaction 方向分类 last_mid==0 时默认 isBuy=true——开盘首帧系统性偏差污染 trade_flow/毒性通道 | [复核改进] tick-rule 回退（与上一笔成交价比较）或用 last valid mid，保留样本优于丢弃 |
| P2-5 | ArbExecutionBridge.cpp:350-363 | orphan 对冲 fallback 直调 stra_enter_long/short 绕过 Router/Tracker——完全不受账本管理的裸单地雷路径 | router 为空应 error+放弃（validateDeps 已保证非空，确为死代码，删除合理） |
| P2-6 | StrategyCoordinator.cpp:1378 | requoteAfterFill 用缓存 tick 的 upper/lower/best 与最新 mid 拼接。[复核精化] 期货涨跌停价盘内不变，upper/lower 失真影响≈0；真正有影响的是 best_bid/ask 陈旧 | 重算时刷新 best_bid/ask（P2 上限，仍值得修） |
| P2-7 | StrategyCoordinator.cpp:961-971 | [部分已修] S4 双写者已由 R4b-A6 updateToxicity 显式入口修复；剩余仅注释漂移（"不复位锁存…永久锁死"悬于每 tick 置 false 的代码上方，与行为矛盾） | 只修注释 |

#### P3 整洁度

- FutuQuoter 构造函数给自己上锁（FutuQuoter.cpp:20）无意义；
  getBidLevelsMut/getAskLevelsMut/getLevelByOrder、UnifiedOrderTracker::getOrders()
  （返回内部 vector 引用的逃生口）全项目零调用——删除。
- StrategyCoordinator.cpp:297-299 spreadOptimizer 配置节读取体为空 if 块残留。
- updateSignals MARKET 诊断日志块(:891-904)取墙钟 TimeUtils::getLocalTimeNow()，
  回测中应统一 replay 时钟。

### 三、五大子系统分层评估

1. **做市报价 ✅ 清晰度高**：GLFT(seqlock 参数)→QuotePolicyChain 六策略对象→
   FutuQuoter 三路径。遗留：handleObligationQuote/handleFlexibleQuote sticky 判定
   取 order_ids[0]（FutuQuoter.cpp:340/:447/:486）而路径 A 已修为尾向扫描——依赖
   "B1/B2 槽内 pendingCancel 同质性"这一未成文隐式不变量才成立，建议补注释固化
   或提取公共扫描函数三路径共用（A4-残余的 quoter 版）。RiskWidenPolicy tickSoft
   与 onHardWiden 存在文档承认的同 tick 覆盖顺序依赖，建议 chain.run 显式保证顺序。
2. **风控逻辑 ✅ 结构好 ❌ 同步缺口**：PreTradeDecision 双层/SideFillBreaker/zombie
   闩锁/closeout 状态机各司其职。遗留：P1-1/P1-2 为最大缺口；ERROR(qphase) 与
   RiskMonitor._trading_halted 双轨暂停语义重叠（onEntrust 阈值同时设两者），
   恢复路径需同时照顾两套状态（FutuRuntimeOps.cpp:343-396 vs RiskCoordinator.cpp:
   114-149 各一套），建议 R4b 后做"暂停源单一化"梳理。
3. **交易执行 ✅**：B+ 槽状态机+Router 来源标记+finalizeOrder 幂等清理闭环。
   遗留：quoter 自旋锁持锁穿越引擎调用（已文档化取舍），建议 PerformanceMonitor
   增加 refreshQuotes 持锁时长埋点量化。
4. **信号因子 ✅ 明显改善**：slot 表驱动+vol 阈值实测标定。[复核更新] S4 双写者
   已由 R4b-A6 修复（updateToxicity 显式入口）；S-1/S-2 regime/IC 簿记已归还
   ICWeightTracker。遗留：computeAlpha 职责进一步拆解；handleLeadLagPush static
   调试计数器残留。
5. **[复核修订] 套利子系统 ✅ 结构债大幅缓解**：R4b-A1 乘数死亡链已修（pushTick
   现 3 参，pair multiplier 装配进 calculator）；A-2 孤儿腿对冲有限重试；A-3 双
   in_flight 按通道精确释放；A-4 fresh-pairing 去重；S-4 Manager 文件级二分
   （1324→1089 行+Init TU）。残余：LegExecutionFSM 组件级收敛（隐式状态散落
   executor/bridge/manager 三处）留后续评估。

### 四、性能优化建议（增量、不改变行为）

1. **双边统计文件 I/O 移出 MdSpi 热路径**：appendBilateralLine 每次 open/write/close
   同步落盘（StrategyCoordinator.cpp:85-101），周期输出+section break 都在行情线程内，
   改内存缓冲+独立落盘线程。
2. TickContext/QuotePolicyContext 每 tick std::string 构造+哈希查找——合约数固定，
   装配期建 code→index 表，TickContext 存 uint8_t code_idx。
3. coordinator 四张 string-key map（_last_quote_params/_halt_quoting_state/
   _last_requote_ms/_last_bilateral_log_ms）同理可用合约下标数组替代。
4. P2-2 MARKET 每 tick cancelAll 修复本身即热路径减负。
5. 保持既有优点勿退化：F5 计时门控/F8 exp 上提/generation 门控快照/FixedString24
   SPSC/alignas(64)/rdtsc 埋点。
6. 验证手段：_ec_5d_perf.yaml + TscClock p99 埋点直接 A/B（预期 p99 持平或微降，
   行为零回归）。

### 五、功能完善建议

1. L2 数据管道（S9 前置）：先在回测验证 depth_imbalance 与 transaction 推断的
   相关性，再决定是否替换 TickTransactionInferer 推断口径。
2. PerformanceAnalyzer.determineMarketCondition 占位等 R4b RegimeTracker，避免重复建设。
3. 持续推动框架侧 CTP 50/51/31 拒单分类通道；orderErrorThreshold=1000 临时参数记得回收。
4. 测试补缺：QuotePolicyChain 六策略无表驱动单测（LimitPrice L0-L3/ColdStart skew
   符号/FillRetreat 取保守价）；P1-1 修复应附双线程 hammer 测试。

### 六、落地路线图

| 批次 | 内容 | 验证 |
|---|---|---|
| R6-a 小步快跑 | P1-1 停机域原子化+_closeout_state 收编(=去大锁 WS-A)；P2-1/2/4/7；P3 清扫 | TestUnits 并发用例；_ec_5d 零回归 |
| R6-b | P2-3 Source::RISK_REDUCE；P2-5/6；[复核提级]双边统计异步落盘 | 回测+taker/closeout 统计口径抽查 |
| R6-c 性能 | code_idx 化四张 map([复核]收益占比极小维持低优)、MARKET 边沿撤销 | _ec_5d_perf p99 A/B |
| 去大锁专项 | 见上一节 WS-A~F（WS-B 可选、WS-E 含静默期兜底） | TSAN+hammer+逐比特 A/B+灰度 p99/p999 |
| 后续 | README/docs 一次性重写（v7.x 描述早已过期）；computeAlpha/LegFSM 组件级收敛评估 | 文档评审+A/B |

> 以上任何一项动手前需先将实施方案写入本文件并经用户确认（§1 流程）。
> 去大锁专项的完整设计见上一节"去大锁（_cb_mtx）低延迟等价设计"。

### 七、复核记录（2026-08-23②）

**背景**：V9 报告完成后引入独立复核；同期工作树推进至 R4b 提交 68da2ea8
（02:12，A1-A6/S1-S5），故报告部分发现相对 HEAD 过时——这是双方分歧根源，
以 HEAD 为准逐条裁定如下：

| 复核论断 | 裁定 | 关键证据 |
|---|---|---|
| 乘数死亡链已修复，报告失实 | ✅ 成立 | pushTick 现 3 参(ArbExecutionBridge.cpp:50)；commit msg"A-1 乘数死亡链…删死参数链"。报告 3 处相关表述已全部更正 |
| P1-2 非现状 bug，属拆锁前置 | ✅ 成立 | transitionCloseoutSub 全部调用方逐一核验均在持 _cb_mtx 回调内；已改判并入 WS-A |
| P1-3 降级 P2/P3 | ✅ 成立 | 使用点实为 8 处（原记 9 系误差），全在装配期 variant 存活窗口 |
| P1-1 最重发现 | ✅ 双方一致 | handleRiskAlert(:191) 经 setAlertCallback 由 arb 线程直调、不在大锁内；haltTrading 裸赋值而 :204 注释谎称"原子写"——唯一绕锁写者，现状真 UB |
| P2-1/2/3/4/5、P3 各项仍在 HEAD | ✅ 逐项复现 | :556 裸调 / :909 每 tick cancelAll / RiskCoordinator:83-84 冒用 CLOSEOUT / :694 isBuy 默认 true / bridge:358,360 fallback 裸单 / 空if块 / quoter 构造自锁 / order_ids[0]×3 |
| P2-6 高估 | ✅ 精化采纳 | 涨跌停价盘内不变，影响收窄至 best_bid/ask 陈旧 |
| P2-4 改 tick-rule 回退 | ✅ 采纳 | 保 trade_flow 样本优于丢弃 |
| WS-B 前提偏差 | ✅ 重要修正 | FutuPortfolio.h:339 全方法 RecursiveSpinLock；降为可选优化，契约描述更正为双写者分字段 |
| WS-E 行情静默期盲区 | ✅ 最有价值发现 | 检查点仅 tick/fill 两入口，双停期命令滞留 vs 现状内联立即执行；已补 (a)检查点扩展+(c)超时惰性 drain 兜底 |
| WS-F"三态"措辞/成本表缺项 | ✅ 采纳 | 两态措辞修正；quoter 自旋锁竞争显性化纳入切 none 验收标准 |

**复核自身遗漏的补正（2 条）**：
1. P2-7 前半 S4 双写者亦已被 R4b-A6 修复（复核仅指出注释问题，未标 S4 已修）；
2. 复核称报告"两处声称未修"，实为四处（乘数链×3 + S4×1）；且 R4b 已使本文件
   原"R4b 待执行"范围部分完成（S-4 文件级二分、S-1/S-2 归还 ICWeightTracker，
   TestUnits 基线升至 105/107），路线图已相应刷新。

**总评**：复核整体准确率约九成，可采信；上文所有 [复核] 标记处即其落地结果。

## 当前方案实施记录（2026-08-23③，R6-a 全量 + R6-b 三项 + 去大锁 WS-A，已编译验证）

用户确认后执行。按方案既定顺序完成第一步里程碑：

### 已落地修改清单

1. **WS-A（=V9-P1-1/P1-2）FutuRiskMonitor halt/closeout 状态域收编**
   - `_halt_category` → `std::atomic<RiskCategory>`（getHaltCategory/haltTrading/
     resumeTrading/canRecover/checkAndRecover/resetDaily/clearIrreversible 全部改
     load/store acq_rel）
   - 新增 `mutable RecursiveSpinLock _halt_domain_lock`，保护 `_halt_timestamp/
     _pause_timestamp/_last_recovery_check/_recovery_count/_halt_pnl_snapshot/
     _was_loss_triggered/_closeout_state`；递归锁覆盖嵌套链（checkAndRecover→
     resumeTrading/canRecover、mark*→transitionCloseoutSub、resetSession→resetDaily）
   - `getCloseoutSubInfo()` 改**按值返回锁内拷贝**（原返回内部引用，拆大锁后即撕裂
     视图）；调用方 CloseoutTrigger.cpp:82 const& 绑定临时量兼容、CloseoutOrchestrator
     :188 / StrategyCoordinator:557 字段读取兼容，零改动
   - isCloseoutTriggered/Completed/Flattening/getCloseoutSub 头文件内联体改锁内读

2. **P2-1** StrategyCoordinator 夜盘恢复分支补 `_risk_monitor &&` 守卫
3. **P2-2** MARKET 暂停边沿触发：新增 `_market_pause_active` 成员，仅进入暂停首 tick
   cancelAll 一次；DIAG 日志时间基准同步换 replay 时钟（tc.timestamp）
4. **P2-4** on_transaction 死代码删除：`last_mid/isBuy` 计算后零消费（方向分类由
   MarketDataContext::onTransaction 内部独立完成）——比复核建议的 tick-rule 回退更优，
   直接消除"last_mid==0 默认 isBuy=true"偏差源（复核建议基于其被消费的假设，实测否）
5. **P2-7** 毒性注释勘误（S4 本体已由 R4b-A6 修复，剩余仅注释与行为相反）
6. **P2-3** `Source::RISK_REDUCE`（OrderTypes.h=3）：taker 减仓单脱离 CLOSEOUT 口径；
   OrderRouter ctor 补两处预分配；RiskCoordinator HALT 清扫清单追加 RISK_REDUCE
7. **P2-5** ArbExecutionBridge orphan 对冲 ctx 直调 fallback 删除（不可达死分支 →
   error+return false 触发 executor 重试）
8. **P2-6** requoteAfterFill best_bid/ask 改用 portfolio 快照最新盘口刷新
   （upper/lower 保留缓存——涨跌停价盘内不变，V8 复核裁定）
9. **P3 清扫** FutuQuoter ctor 自我加锁删；getBidLevelsMut/getAskLevelsMut/
   getLevelByOrder/UnifiedOrderTracker::getOrders()×2 引用逃生口删（全项目零调用逐项
   复核含 TestUnits）；loadConfigFromVariant 空 if 壳清理

### 验证（已完成）

- 编译：WtFutuCore + TestUnits 零 error（仅既有 ftime deprecation 警告）
- TestUnits 新增 test_v9_r6a.cpp 6 用例全过：
  ConcurrentHammerNoCrash（4 线程 ×20000 次 halt/resume/closeout 转移+双读者快照域断言）
  /CloseoutInfoIsValueCopy/IrreversibleSemanticsPreserved/AutoClearIrreversibleOnResetGated/
  RecoveryCooldownRespected/RiskReduceDistinctValues
- 全量 TestUnits **111/113**（基线 105/107 + 新增 6；仅 test_session.test_allday 与
  test_shm.test_sharehelper 两个既有环境性失败，零回归）
- 回测冒烟 configbt_v5：EXIT=0、0 HALT/0 zombie、session end Delta=0、
  资金曲线正常收敛（closeprofit/dynbalance 量级正常）

### 待办移交（下一步）

1. **去大锁 WS-C/E/F**：TradingState 单写者收敛(EventDispatcher)、罕见重操作命令化
   （含静默期兜底 (a)+(c)）、两态编译开关 big|none —— 按 §1 需独立会话+TSAN 全套验证
2. **R6-b 余项**：双边统计异步落盘（复用 LockFreeQueue+drainTdSpiLogs 模式）
3. **_ec_5d 全量回测 A/B**：本轮全部改动按构造行为中性，v5 冒烟已过；正式零回归
   结论需 _ec_5d 一轮（预期成交笔数落在 mocker 噪声带内）
4. 生产部署仍按 §5 流程另行执行

## 当前方案实施记录②（2026-08-23④，WS-E/F + R6-b 余项 + _ec_5d 全量 A/B，已验证）

### 修改清单

1. **WS-E 罕见重操作命令化（含静默期兜底）**
   - UftFutuMmStrategy 新增 PendingCommand{ChannelReady/ChannelLost/SessionBegin/
     SessionEnd} 通道：post(锁+push+release 置标志) → 单飞 claim(exchange) 消费；
   - 发布者自身即 drain 点 => 行情静默期无滞留（V9 复核盲区消除，无需新增线程）；
     on_tick/on_trade/on_order/on_entrust(on_entrust 复用 _main_ctx) 四入口加检查点，
     快速路径一次 acquire load ~1ns；
   - 四个回调改经通道：大锁默认下 post 后同回调内 claim 必然成功 => 与旧内联逐比特一致。
2. **WS-F 两态开关脚手架**：`FUTU_CB_LOCK_BIG`(默认1)=big；=none 需先过验收
   （TSAN 全量+_ec_5d 逐比特+灰度 p99/p999+refreshQuotes 持锁埋点），宏注释载明。
3. **R6-b 双边统计落盘移出热路径**：周期 live 行改 `_bilateral_io_backlog` 内存积压
   （仅 MdSpi 触碰），section-break/收盘 flush 定点排空（安静期一次性写盘）；
   控制台日志保持实时。权衡：崩溃丢失 ≤一个积压周期的周期行（义务/section 行不受影响）。

### 验证

- 编译零 error；TestUnits 全量 111/113（同基线，零回归）
- 回测 v5 冒烟 + _ec_5d 全量均 EXIT=0、0 HALT/0 zombie：
  | 指标 | R5 基线 | 本轮 |
  |---|---|---|
  | 成交笔数 | 17,788 | 17,939 (+0.8% 噪声内) |
  | dynbalance(0612) | 306,882 | 305,016 (-0.6% 噪声内) |

### 遗留（去大锁收官清单）

- **WS-C TradingState 单写者收敛**（EventDispatcher 投递低频处置转移）：独立会话执行；
- 切 `FUTU_CB_LOCK_BIG=0` 的完整验收流程（TSAN 构建/埋点量化/逐比特 A/B/灰度）
  按去大锁方案节执行后方可删除 _cb_mtx。

## 收官审计记录（2026-08-23⑤，none 模式就绪度核查）

**已修复（切 none 硬阻断 -1）**：`AsyncArbitrageExecutor._oid_to_pair` 加
`_oid_pair_lock`（RecursiveSpinLock）——P0-3 注释"main-thread-only"与事实不符：
tagOrderPair 写于 MdSpi(bridge.onTick)、consumePairTag/onOrderFinalized 读删于 TdSpi，
大锁时代被掩盖。修复后 v5 冒烟 EXIT=0/0 HALT、资金与前轮逐值一致。

**EventDispatcher.subscribe**：全项目零调用，非阻断。

**切 none 剩余硬阻断清单（下一会话机械执行）**：
1. SelfTradeCalibrator：无任何锁成员，而 onTick(Md)/recordFill(Td)/getFillRetreat
   (Md+Td) 跨线程触碰内部 map —— 需按 UnifiedOrderTracker 模式加 RecursiveSpinLock；
2. PerformanceMonitor 裸计数器逐一核对原子化（recordFillReceived/recordQuoteToFill/
   recordTickProcessed 等 Md/Td 双写点）；
3. 以上完成后：`FUTU_CB_LOCK_BIG=0` 构建 → TestUnits 全量 → _ec_5d 逐比特 A/B →
   refreshQuotes 持锁埋点量化 → 生产灰度 p99/p999 → 删除 _cb_mtx（单独 commit）。

## 收官实施记录（2026-08-23⑥，阻断清零 + WS-E 属主域修订，已验证）

复核会话（外部审计）补录了收官审计的 3 个漏项，与既定 2 项一并执行完毕。

### 修改清单

1. **SelfTradeCalibrator 跨线程收编**（硬阻断①）：新增 `mutable RecursiveSpinLock
   _lock`，全部公开方法收编（recordFill=Td / onTick,decayCalibration=Md /
   getFillRetreat=Md+Td 双调用点同一把锁）。字段级 atomic 不覆盖
   `_contract_states`(wt_hashmap operator[] 结构性插入)/`_retreat_states`(std::map)/
   RingBuffer，此前依赖 _cb_mtx 兜底。叶子锁无锁序风险。
   注：`getFillHistory` 返回内部指针的逃生口保留但全项目零调用，锁在返回后释放，
   属已知残余风险（如未来启用须改值返回）。
2. **PerformanceMonitor 计数器原子化**（硬阻断②）：ThroughputStats 17 字段全部
   `std::atomic<uint64_t>`（record* Md/Td 双写点）；显式拷贝构造/赋值支持
   getThroughputStats 值快照（该接口零外部调用，顺带简化为整体拷贝）。
   各延迟 RingBuffer 按类型单写者（TICK_TO_QUOTE/SIGNAL_TO_ORDER=Md，其余=Td），
   无需动；`_last_threshold_log_ms` 核实为 Md 单写，不处理。
3. **`_blocked_contracts` 删除**（漏项③）：写-only 死字段（全项目仅 3 处
   clear()、零读者），ticker/Td/channel 三线程并发 clear 在拆大锁后是形式 UB，
   删除优于加锁。封锁语义由 TradingState long_blocked/short_blocked 承担。
4. **`_violations_buf` 通道恢复局部化**（漏项②）：onChannelReady 恢复序列改局部
   `std::vector<RiskViolation>`——拆大锁后该序列可能由 MdSpi 检查点 drain 执行，
   与 Td(processTradeFill/onEntrust) 共享即竞态。成员缓冲现严格 Td 专属，
   头部注释同步更正（原注释"仅 TdSpi 路径"对 channel 恢复序列不成立）。
5. **WS-E 属主域拆分**（漏项①，设计修订）：原"发布者自身即 drain 点"统一策略
   在 none 模式下会让 Session 命令在 WtUftTicker 线程内联执行 onSessionBegin/End
   → 触达 coordinator resetSession/quote_chain 等 Md 属主无锁状态（真竞态）。
   修订为按域分派：
   - Channel 类（Td 域）：发布者即 TdSpi 线程，两种模式均自 drain；Td 检查点
     (on_trade/on_order/on_entrust) 兜底。
   - Session 类（Md 域）：实盘 ticker 线程只投递，仅 on_tick (Md) 检查点消费
     （session 切换后必有 tick，无静默期滞留）；回测单线程发布者即 Md 线程
     （`_md_tid` 首个 on_tick 捕获，tid 相等自 drain）→ 与 big 逐比特一致。
   - big 模式两域均保持 post 后同回调 drain ⇒ 与历史内联逐比特一致（不变式）。
   - drain 重写：域分拣留队（修复分拣后自旋标志死循环隐患）+ RAII 释放单飞标志
     （修复命令执行抛异常导致 `_cmd_executing` 永久卡死）。
   - 已知行为差异（仅 none 模式）：实盘 SessionEnd 从收盘时刻内联改为次日首 tick
     边界执行（cancelAll/arb stop/统计报告延迟——收盘后无活动订单与行情， benign）；
     回测末日 SessionEnd 后有 tick 流入故不受影响。
6. **P2-2 收尾**：`_market_pause_active` 纳入 resetSession 复位（跨会话残留
   边界情形，无害但保持干净）。

### 验证（全部独立执行）

- 编译：WtFutuCore + TestUnits 零 error；none 模式 TU 级 `-DFUTU_CB_LOCK_BIG=0`
  -fsyntax-only 通过。
- TestUnits **113/115**（新增 CalibratorClosing.ConcurrentFillTickNoCrash +
  PerfMonClosing.ConcurrentCountersNoLostUpdate 双线程 hammer 全过；仅
  test_session.test_allday / test_shm.test_sharehelper 两个既有环境性失败，零回归）。
- 回测 `_breaker_retreat_test`：**big 模式四张 CSV（closes/funds/positions/trades）
  与基线逐字节一致**；**none 模式（独立 build 目录全量 -DFUTU_CB_LOCK_BIG=0 构建）
  四张 CSV 与 big 基线逐字节一致**——属主域拆分在回测路径行为等价直接得证。

### 切 none 剩余流程（验收性质，无已知代码阻断）

1. TSAN 构建跑 TestUnits 全量（RecursiveSpinLock 基于 atomic_flag acq/rel，
   TSAN 可正确识别 happens-before，无标注误报风险）；
2. `_ec_5d` 全量逐比特 A/B（big vs none）；
3. refreshQuotes 持锁时长埋点量化（quoter 自旋锁竞争显性化验收）；
4. 生产灰度 p99/p999 观察一个交易日 → 删除 `_cb_mtx`（单独 commit，revert 即回退）。

## 当前方案（2026-08-24，热参数加固：边界表对齐 + applyAll 交叉复查 + 启动漂移摘要，用户已确认执行）

**背景**：配置一致性分析确认——26 个热参数的稳态权威是 hotparams.yaml（实盘 ~1s 内覆盖
config/coordinator 同名键；重启时共享内存旧值经 initial applyAll 立即生效，期间对 config.yaml
同名键的修改无效，UftFutuMmStrategy.cpp:374-386 注释明示）；回测不跑 watcher、热参完全不生效。
三处误配风险：
① **边界口径不一致**——loader error 级 baseSpread(0,20]/baseQty(0,100]/levelStep(0,100]/
maxDelta(0,1e8]（FutuConfigLoader.cpp:210/227/258/263），而热参边界表 base_spread{0,1000}/
base_qty{0,100000}/level_step{0,1000}/max_delta{0,1e6} 全部更宽；protect_ticks 热路径允许
0=静默关闭价格软保护；sticky_threshold 允许 0=每 tick churn 风暴；
② **交叉校验只跑启动期一次**（权重和±0.1 / maxDelta<maxPosition / full_side_depth≥obligationMinQty
/ min≤max spread_mult），热更路径零复查；
③ **漂移无感知**——hotparams 与 config 漂移后，回测验证的参数组合≠实盘运行组合（watcher 仅实盘
启动），排障易看错文件。

**修改清单**：

1. `FutuHotParamManager.cpp` 边界表收紧（原则：loader error 级→热路径拒收；warn 口径保持宽松
   交给交叉复查；逐行注释标明来源）：
   - base_spread {0.5,20}（validator checkRange [0.5,20]；原 {0,1000}）
   - base_qty {1e-6,100}（loader error (0,100]；原 {0,100000}）
   - level_step {1e-6,100}（loader error (0,100]；原 {0,1000}）
   - max_delta {1e-6,1e8}（loader error (0,1e8]；0=静默关组合 skew+WIDEN util 分母；原 {0,1e6}）
   - phi {0.01,2.0}（validator [0.01,2]；原 {0.0001,1} 上界过紧下界过松）
   - delta_skew_threshold {0,0.9}（validator [0,0.9]；原 {0,1}）
   - sticky_threshold {0.01,10}（loader warn (0,10]；0=churn 风暴防呆；原 {0,1e6}）
   - protect_ticks {0.5,1e4}（<半 tick 软保护无意义；关闭应走 price_protection 开关；原 {0,1e4}）
   - 其余（权重×5/alpha_sensitivity/improve_retreat_ratio/spread_mult 等）保持宽松。
2. `FutuHotParamManager.h/.cpp` 新增可测纯函数 `crossCheckIssues(HotCrossCheckInput)`
   （返回 warn 级问题字符串列表）+ `applyAll` 末尾组装输入调用并逐条 warn "[HOTPARAM-CHECK]"。
   检查项：五路权重和偏离 1.0 超 ±0.1；full_side_depth（与 loader 同公式）< obligationMinQty；
   portfolio_max_delta > 任一合约 maxPosition（语义边界口径，经 portfolio 快照取 max_position，
   applyAll 低频路径拷贝可接受）；min_spread_mult > max_spread_mult（GLFT clamp 区间倒置）；
   confidence_weight_min > confidence_weight_max（插值反向）。
3. `FutuHotParamManager.h/.cpp` 新增静态纯函数 `collectDriftLines`（-1=文件不可载，>=0=差异键数，
   输出人类可读差异行）+ 成员 `logDriftSummary(filepath, strategy_id)`；
   `UftFutuMmStrategy::on_init` 在 initial applyAll 之后**无条件**调用（回测也打印——watcher 不跑、
   热参不生效，差异键即回测/实盘行为分叉点）。差异键逐条 warn "[HOTPARAM-DRIFT]
   'x' config_default=A hotparams=B"，汇总一条含生效语义说明。
4. `src/TestUnits/test_hot_param_manager.cpp`：RejectsOutOfRangeValues 用例 phi:2.0 改 phi:3.0
   （新边界下 2.0 合法入界，3.0 才拒收）。
5. 新增 `src/TestUnits/test_hot_param_hardening.cpp`（CMake GLOB 自动收录）：边界收紧表驱动
   （拒收/放行两侧：base_spread 30/0.4 拒 3.5 收；protect_ticks 0/0.3 拒 0.5/1 收；
   sticky_threshold 0 拒 0.02 收；phi 0.005 拒 2.0 收；max_delta 0/1e9 拒 50 收；
   delta_skew_threshold 0.95 拒 0.9 收；base_qty 150 拒）+ crossCheckIssues 五检查项正反例 +
   collectDriftLines 差异计数与不可载文件返回 -1。
6. `README.md` §4.17 热参数小节补三道加固描述；§6 配置体系补"热参与配置文件一致性约定"
   （26 键稳态以 hotparams.yaml 为准、config 为出厂基线、回测不加载热参）。

**明确不做**：不改任何 yaml 数值（当前 hotparams 与 config 一致）；不给 watcher 加 config 文件
重载能力；交叉复查不做阻断/拒应用（保持 warn 级，避免热调参被误卡死）。

**验证**：make WtFutuCore + TestUnits 全量（基线 113/115，2 个既有环境性失败）→ 新用例全过 →
回测冒烟 configbt_v5（EXIT=0/0 HALT/资金收敛；预期日志出现 drift check passed 0/26 且
0 条 HOTPARAM-CHECK 告警 = 行为中性证明）。

### 实施记录（2026-08-24，已完成，验证通过）

修改清单 1-6 全部落地。验证结果：

- 编译：WtFutuCore + TestUnits 零 error（仅既有 ftime deprecation 警告）。
- TestUnits：**129/131**（基线 113/115 + 新增 test_hot_param_hardening 16 用例；仅
  test_session.test_allday / test_shm.test_sharehelper 两个既有环境性失败，零回归）。
  注意事项：CMake `file(GLOB)` 在 configure 期求值，新增测试文件后需重跑 `cmake .` 再 make
  （本轮首次构建即因漏此步误报"零新增用例"）。中途修正：DetectsSoftLimitAboveHardCap 子用例
  预期写错——实现语义为"超过任一合约硬顶即告警"（保守正确，与 per-contract 校验口径一致），
  测试已按此修正。
- 回测冒烟 configbt_v5：EXIT=0、0 HALT/0 zombie、session end Delta=0、`HOTPARAM-CHECK` 0 条
  （应用值通过全部交叉复查 = 行为中性证明）。
- **漂移检测上线即发现真实漂移**（机制价值的直接证据）：dist/WtBtFutu 副本
  `[HOTPARAM-DRIFT] 'base_spread' config_default=2.5 hotparams=2.0 / 'base_qty' 3.0 vs 2.0 /
  'max_delta' 50 vs 30` ——configbt_v5.yaml 为旧参数组合而 dist/hotparams.yaml 与 src 权威一致。
  此前该漂移完全不可见；正对应"回测不吃热参 ⇒ 差异键=回测/实盘行为分叉点"的告警语义。
  该差异属回测配置副本固有状态（v5 冒烟用），不调整；正式回归用 _ec_5d 时注意观察同类输出。
- 部署：新 libWtFutuCore.so 已 cp 至 dist/WtBtFutu/uft/（首轮冒烟曾跑旧库，日志无 drift 行
  即为此故，二次部署后复验通过）。

## 当前方案（2026-08-24②，业务(GLFT)/风控复核修复包，用户已确认全部问题，方案待确认后执行）

**背景**：应用户要求对 GLFT 业务链与风控链做全面复核（SpreadOptimizer.cpp 逐行 +
RiskCoordinator/RiskMonitor/QuotePolicyChain/Assembler 消费点交叉验证），产出 P1/P2/P4、
R1-R6、M1-M4、B1-B5 共 15 项发现，用户裁决全部接受。按风险与行为影响切四个批次，
每批独立 commit + 独立验证，便于回退与归因。

---

### 批次 A：纯清理 / 零行为（先合）

**A1 (R2) 删除 checkSoftLimits 死方法**
- `FutuRiskMonitor.h:344` 声明、`.cpp:341-357` 定义全删（零调用已 grep 复核；
  WIDEN 软响应唯一活路径 = RiskCoordinator.tickSoft :180-183）
- `README.md` §4.11 流程图行"checkSoftLimits / RiskWidenPolicy.tickSoft"勘误为仅 tickSoft

**A2 (P2) SpreadOptimizer EMA 段整洁化**
- 删 `prev` 死变量(:91)；消除 `new_smoothed` 自赋值绕行(:95/:99)
- 首 tick 魔数 `<0.5`(:102) 改显式成员 `bool _mult_initialized{false}`（h 加字段）

**A3 (M4) fastPow 负底数护栏**
- `x < 0 → return 0.0` + 注释（当前两条调用路径均有 ≥0 守卫 :271/:295，防御未来新调用点 NaN）

**A4 (R4/R6/B1/B3/B4 文档联动，无代码行为)**
- `coordinator.yaml`：toxicityDetector 节注记"三层响应联动"（GLFT 加宽 toxicity_spread_factor /
  本节 is_toxic 停边+cooloffMs / GLFT pause mult≥max×0.9）；selfTradeCalibrator 节注记
  retreat_ticks 与 quoting.improveRetreatRatio 为两个无关机制
- `QuotePolicyChain.h` 头注释：补 chain.run 固定顺序保证说明 + "风控响应与业务调整混链"边界声明
- `SpreadOptimizer.h` GLFTParams 注释：phi 实际角色=波动率加价系数（库存厌恶由 delta_skew_* 承担）

### 批次 B：新配置键（默认值=现行为，回测预期逐比特一致）

**B1 (R1) sticky_threshold 一参两用拆分**
- `UnifiedOrderTracker.h`：cfg 字段改名 `stale_extension_ticks`（默认 2.0 = 现 sticky(1.0)×2 保行为）；
  删死接口 exceedsStickyThreshold/checkPriceDeviation（零调用已复核）；cpp:459 消费点同步更名
- `FutuModuleAssembler.cpp:301`：改读新键 `modules.autoCancel.staleExtensionTicks`（缺省 2.0）
- `coordinator.yaml` autoCancel 节加键+注释；QuoterConfig.sticky_threshold 语义收窄为"顶单黏性"专用

**B2 (M1) 合约级 maxDelta 纳入热参（26→27）**
- `FutuHotParamManager.h`：枚举尾部追加 `HP_CONTRACT_MAX_DELTA`（保持既有索引稳定；名字寻址共享内存安全）；
  paramNames/hot_defaults/bounds 表同步 + static_assert 自适应
- `.cpp` registerParams 默认取 anchor 合约 ci.max_delta；applyAll 经新增
  `FutuPortfolio::setContractMaxDelta(double)`（锁内遍历置 ContractState.contract_max_delta）
  ——下游 PreTradeDecision/computeInventoryStrategyInputs 读快照自动生效
- crossCheck 输入加 contract_max_delta，复用"软限>硬顶 warn"检查
- `hotparams.yaml` 加 `contract_max_delta: 30`（=现配 → 零行为）；README 全部"26 参数"表述更新

**B3 (M2) signal 层窗口可配化**
- SignalAggregatorConfig 加 rolling_window(500)/rolling_interval(20)/ic_update_interval(50)，
  fromVariant 读 model.*，构造透传 RollingScaleTracker/AdaptiveWeightFramework::Config
- PerformanceAnalyzerConfig 加 adverse_eval_ticks(10)/adverse_timeout_ms(30000)；
  createPerformanceAnalyzer 读 modules.performanceAnalyzer.*（yaml 新节）

**B4 (P4) 毒性加宽门槛参数化**
- GLFTParams.toxicity_min_score 默认 0.05，fromVariant 键 toxicityMinScore；:56 硬编码改参数

**B5 (M3) alpha 时效老化（默认关）**
- coordinator signalAggregator 加 `alphaMaxAgeMs`（默认 0=关闭保行为）；
  update() 内 book timestamp 可得时，age 超限 → alpha.valid=false+confidence=0

### 批次 C：行为变化（需 _ec_5d A/B 归因）

**C1 (P1) 组合 skew 量纲统一到 ticks**
- `computePortfolioDeltaSkew(totalDelta)` → 签名加 half_spread_ticks，返回 ×half_spread_ticks
  （half≤0 返 0）；调用点 :146 同步；权重语义注释固化为"两维同量纲(ticks)，权重=稳定相对力度"
- 行为影响：组合分量随 spread 宽度伸缩（此前恒 ~0-2 tick 绝对值）——宽价差环境下组合 skew 变强
- 新增 `src/TestUnits/test_skew_dimensionality.cpp`：同 util 跨 half_spread 线性比例 /
  权重相加后 clamp 不越界 / 阈值死区不变 / 与 contract 分量相对关系符合"contract 主导"注释

**C2 pending 投影口径对齐（skew 用已实现口径）**
- 语义固化："skew=已实现库存响应；force_obligation/qty 衰减=前瞻含在途"（义务需要提前性，skew 不需要）
- `PortfolioContext`(SpreadOptimizer.h:155) 加 contract_realized_delta_util(+valid)；
  StrategyCoordinator processQuoting 从 tc.cs(delta()/contract_max_delta) 填充
- SpreadOptimizer :136(cross_authorized)/:144(contract skew) 切换 realized 口径（穿越授权同步收紧，
  注释说明理由：主动减仓动作不应被未成交挂单预授权）
- force_obligation 路径（computeInventoryStrategyInputs→StrategyInputs）不动
- 测试扩展 test_skew_dimensionality.cpp：pending 高位时 realized skew 不变、obligation 输入仍含 pending

### 批次 D：恢复路径单一化（B2 双轨，独立会话式验证）

**D1 ERROR 单轨化**
- `FutuRuntimeOps::onEntrust` 阈值分支：删除 haltTrading 调用，仅 setQuotingPhase(ERROR)+cancelAll+计数
  （ERROR 的指数退避自探恢复 handleQuotingAutoResume 不变；halted/recovery_budget 不再被该路径消耗）
- 消费方矩阵审计（实施时落档）：requoteAfterFill(!isTradingHalted 但 canQuote=false 双保险仍闭)、
  processTradeFill 四道闸（qphase!=RISK_HALTED 时 halt 恢复机制自然跳过）、
  RiskCoordinator 已-halt 分支（halted 未设不触发）
- 契约单测：ERROR 态下 canQuote()==false 且 isTradingHalted()==false（固化单轨语义）
- 回测 v5+_ec_5d 零回归预期（mocker 下 error 路径几乎不触发）

---

### 验证与提交纪律

| 批次 | 单测 | 回测判据 |
|---|---|---|
| A | 全量基线（129/131+新增） | v5 冒烟 CSV 逐比特一致（无行为路径） |
| B | 热参新键 bounds/drift 用例扩展 | v5 冒烟逐比特一致（新键默认=旧值） |
| C | test_skew_dimensionality 全过 | v5 冒烟 0 HALT/zombie/error；_ec_5d 全量 A/B 归因记录（skew 组成变化属预期，成交笔数/dynbalance 变化须解释） |
| D | 契约用例 | v5 + _ec_5d 零回归 |

每批独立 commit；C 批部署顺序 = 先回测归因评审再谈生产。

**明确不做**：GLFT 结构重构（phi 更名等语义保留原键名）；B1/B3/B4 的结构性拆链（维持既有裁决）；
hotparams 共享内存旧布局迁移（27 键按名注册天然兼容）。

### 实施记录（2026-08-24②，A/B/C/D 四批全部完成，验证通过，4 commits）

| 批次 | commit | 验证 |
|---|---|---|
| 热参加固(前任务补记) | 0930d76d | TestUnits 129 过; v5 冒烟健康 |
| A 清理 | fd237a68 | 129 过; v5×3 轮健康 |
| B+C 配置面+skew修正 | fd060419 | **140→137 过**(含 SkewDimensionality 6 用例); v5 冒烟; _ec_5d A/B |
| D ERROR 单轨化 | d963a44e | **140 过**(+ErrorSingleTrack 3 契约用例); v5 冒烟 Delta=0 |

**_ec_5d A/B 归因记录（C 批行为变化评估）**：
- trades: B/C 版 17,794 vs preBC 对照 17,942（差 -0.8%，mocker 噪声带 ±1.5% 内）
- is_toxic 事件：两二进制**完全相同 =983** —— B/C 对毒性链路零影响 ✓
- dynbalance(0612): 322,653；HALT/zombie/breaker 全程 0；session end Delta=0 ×3
- C1/C2 在本数据集上成交/资金影响落入噪声带（skew 差异未放大为可观测行为差）

**重要勘误（验证方法论）**：
1. **UFT 回测不存在逐比特判据**——`UftMocker.cpp:34` extern 复用 HftMocker 的 splitVolume
   （每次调用 srand(time(NULL)) 重播种），成交拆分序列随真实墙钟变化。此前"已知外部限制"
   只记录了 HftMocker 路径，实际 UFT 同样中招。v5/_ec_5d 的 CSV md5 对比一律无效，
   判据改为统计带（笔数 ±1.5%）+ 健康度（0 HALT/zombie/error、Delta=0、资金收敛）。
2. **策略类 debug 日志路由在 Runner.log 非 Strategy_uft.log**（[TOXIC]/SIGNAL_DECOMP/
   HOTPARAM-* 均在 Runner.log），计数时勿看错文件。
3. TOXIC 计数与 R5 簿记(2,770)的差异（现 983）：经 preBC 对照二进制证明与本修复包无关，
   源头在 R6 系已提交变更（最可疑=P2-4 on_transaction 死代码删除改变 trade_flow 输入），
   留待独立归因，不阻塞本包。

**遗留移交**：
- C1/C2 改变 GLFT skew 数学——生产部署前建议再积累一轮 _ec_5d 观察窗口（本轮单次 A/B 通过）
- TOXIC 2770→983 归因（R6 系回归排查）
- README §4.11 已随 A 批勘误 checkSoftLimits；热参表述已全部更新为 27

## 文档重写记录（2026-08-24③，docs-only 零代码）

应用户要求"模块文档尽可能详细（可对照逐行检查代码）+ 配置全字段说明（含热更）"：
- `README.md` 重写为 840 行代码级架构手册：五层架构/线程模型与锁层级/单 tick 数据流/
  20 个模块小节全部带 `文件:行号` 锚点（GLFT 公式逐步/风控闸门公式表/checkRisk 十步/
  onEntrust D1 后形态/B+ 槽状态机/热参链路 ASCII 图等）；§5 设计裁决十条；§8 限制勘误九条
- 新增 `CONFIG.md` 464 行：加载链路总览；主配置(replayer/env/uft/strategy.params 含
  contracts/quoting/portfolio/risk.frequency/closeout/order_control/performance 全字段)、
  coordinator.yaml 六节、hotparams.yaml 27 参数总表+热更链路专节+注意事项、logcfgbt、
  spread_arbitrage.yaml、死键清单(6 项)、排障速查表
- 素材来源：4 个并行研究代理（配置体系/编排层成功，定价与风控两代理多次空返回后改由
  主会话定向 grep+精读 SpreadOptimizer.cpp 全文/TradingState.h 全文补齐）
- 行号基于当时 HEAD；文档尾注声明漂移免责。零源码改动，无需回测验证。
