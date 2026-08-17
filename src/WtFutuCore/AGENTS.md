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

## 框架层已打补丁（越界修改记录，2026-08-03，GUI 监控接入需要）

- **`src/WtUftRunner/WtUftRunner.cpp`**：`config()` 尾部补 `initEvtNotifier()` 调用。
  原因：框架缺陷——`initEvtNotifier()` 定义存在但从未被调用（对比 WtRunner.cpp:241 /
  WtRtRunner.cpp:654 均有调用），导致 config.yaml 的 `notifier` 段被静默忽略，
  EventNotifier 不发布任何 MQ 事件，WtMonSvr GUI 无法接收实时订单/成交/日志。
  上游修复后可还原。重建流程：`dist/WtRunnerFutu/rebuild_release.sh`。
- **`dist/WtRunnerFutu/libWtMsgQue.so`**：EventNotifier 按 CWD 优先查找 MQ 模块，
  需将该库置于 runner 工作目录（从 dist/bin 拷贝）。
