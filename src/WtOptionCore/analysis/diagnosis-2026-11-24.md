# WtOptionCore 运行诊断报告（2026-11-24）

> 数据来源：生产环境 `dist/WtRunnerOption/Logs/Runner_2026-11-24.log`（32MB，单实例连续时间线 00:33:04→20:30，经 NUL 空洞/截断完整性校验）、`Logs/Strategy/option_mm_ag_2026-11-24.log`、`generated/*`，与仓库源码逐条对照。
> 方法：错误/警告模式分类计数 → 时间分布 → 源码语义核实。本报告已经过两轮复核，修正记录见第 5 节。

## 1. 总体评估

- 进程/行情/快照全天健康；日盘 09:00–15:00 正常做市（947,550 ticks；session 停/复时间线全部正确：09:00:35 开、10:14:35/11:29:31 停、13:30:09 复、14:59:32 收）。
- 全天 error 65,769 / warning 41,392，**约 99% 集中于本文档列出的 13 个问题**；其中 4 个为设计级缺陷。
- 当前（20:30）状态：19:57 集群式 4097 踢线后 Disabled / channel down（收盘死时段，属预期）。

## 2. 问题清单

### P0-1【设计缺陷】收盘后期货腿卖单风暴（59,280 条 error，01:00–09:00）

- **现象**：01:00 夜盘收盘后至 09:00，每小时约 7,400 条 `[simnow] Selling SHFE.cu.2612/cu.2701 of quantity 10, not in trading time`（1Hz×2 合约，持续 8 小时，仅卖）。
- **已确证根因**：
  1. 策略侧无交易时段门控的定时路径持续产出订单意图（`checkDeltaHedge` 等在 `HftOptionStrategy.cpp:1929` 处于 ungated 状态；P1 审计改动的假设"!channelReady 会拦住"不成立——SimNow 收盘后 TCP 仍在，拒单发生在交易所层）；
  2. **框架"not in trading time"拒单不产生任何事件**（`TraderAdapter.cpp:665-676/965-969`，`return ret` 发生在 `updateUndone`/委托创建之前）→ 策略层 hedge reject-streak、OQM reject-retry 两道退避都收不到反馈 → 无限重试；
  3. 01:00:01 `TradingStateArbiter: ignore downgrade SessionStopped -> Normal (reason=close_flatten_disarmed)`——收盘平仓解除边界吞掉了 session 停止降级，状态整夜 Normal / enabled=true，无门控的 pricer 定时重算（`HftOptionStrategy.cpp:1978`）持续供给报价意图。
- **开盘洪峰（确证）**：09:00:35–09:01:04 连续 `Opening short of quantity 10` 真实报单并成交（09:01:02 框架成交流水 `Target position updated: 0 -> -10` 证实）——隔夜积压意图在开盘一次性释放 20+ 手期货卖单。
- **未解疑点（重要）**：卖出数量恒为 **10 手**，但所有已审计路径上限均为 2（`delta_hedge.max_order_size=2`、`expiries[].max_pos_fut=2`，期货报价尺寸公式 `CompositeOptionPricer.cpp:1170-1171` 在持仓 -130 时 ask=0 只允回补）。**静态分析无法解释 10 手来源**，存在未被发现的尺寸通路；需运行期插桩（`executeOrderIds`/`sendSingle` 记录 qty 与调用栈）收网。
- **归属修正**：`DeltaHedgeExecutor::decide()` 有 maxOrderSize 钳制（`DeltaHedgeExecutor.h:67`），且 `HftStraBaseCtx::do_set_position`（`HftStraBaseCtx.cpp:781`）只是成交记账——因此"对冲执行器整夜未开火/开火"无法用该日志证明或否定；发射器 ∈ {期货腿 OQM 报价路径, hedge fireLeg}，两者都缺时段门控与拒单反馈。

### P0-2【设计缺陷】启动恢复致 13 个主力合约被冻结一整天

- **现象**：00:33:20–00:52:37，13 合约（2612 的 C96000/C98000/P96000 + 2701 的 10 个行权价）`cancel ack lost after 3 retries, cancelAll sweep issued; instrument frozen`。
- **根因**：重启恢复扫除的 cancelAll 在 SimNow 夜间链路无 ack；3 次重试后冻结，且**冻结无超时、无会话级解冻**（`OptionQuoteManager.cpp:818` "frozen until a terminal callback arrives"，terminal 永不到来）。
- **影响**：两个做市主力月份 13 个行权价全天静默。

### P0-3【待收网】段错误（今晨 00:27:15，栈与机理）

- 崩溃链（addr2line 确证）：`worker_loop → drainPendingQuotes → OQM::updateOrders → updateSide/sendSingle → sendQuote → [OTD λ] → [OrderGateway λ] → executeQuoteIds → stra_quote(+0xd4=TraderAdapter::quote 调用返回地址) → doOrder → "Signal of buying/selling cu2702.P.98000 ×5" → SEGV`。unwind 终止于 stra_quote，实际故障点在更深的框架交易引擎路径，**需 core dump 定论**。
- **触发链（复核后更新）**：服务端 config 含 **5 个月份配置**（2702–2704 `enable:false`，**缺 `underlyingCode`**、仅有 `hedgeCode`，定价用全局回退）。昨 00:27 夜盘进行中操作员开启 202702 → pricer 按默认 `max_qsize=5` 报价（与崩溃报单 qty=5 吻合）→ 10 秒后段错误。今日 19:08 复测同一开启命令生效（未崩，因市场已收盘无真实报单）。**"开启配置不全的月份"是已验证的可复现前置条件**。
- **同族实锤缺陷（源码审查发现，未证实即崩溃点）**：
  - `TraderAdapter::buy/sell`：`getContract()` 返回 NULL 时 `cInfo->getCommInfo()` 直接解引用（`TraderAdapter.cpp:666-667`、sell ~960）；`commInfo->getSessionInfo()` 亦无检查；
  - `HftStraBaseCtx::stra_sell`：`commInfo->canShort()` 裸解引用（`HftStraBaseCtx.cpp:263` 附近）。

### P1-4【配置+对账缺陷】PositionGuard 永久背离，全天演化从未自愈

- **现象**：`DISCREPANCY` 5,703 次、`TRADING DISABLED` 33 次、`skipping updateOrders` 40,411 条；典型 `internal=0 broker=-130`（2,030 次，cu2701 期货）。
- **根因（复核修正版）**：
  1. **config 无 `positionGuard` 段** → `clampLimit=0`（钳制禁用）、`undoWindowSec=0`（乐观调整禁用）——任何启动偏差**永久告警+禁用，零自愈**；
  2. 首帧快照竞态：首帧 0/缺失 → guard 以 0 初始化 → 后续 broker −130 永久背离（00:33:05 首帧即爆发）；
  3. **背离全天持续演化、从未解决**（修正前版"09:00 后自然停止"的错误说法）：09:01–10:43 持续以新数值报警（09:01 达 41 次/小时，最后 10:43:37 `internal=4 broker=-12 diff=16`）；被熔断合约全天被跳过。
- **影响**：期货对冲腿与大量期权合约永久退出做市 + 日志洪泛。

### P1-5【结构性风险】风险限额 vs 遗留持仓

- `max_vega` 熔断 2 次（00:33/00:44，13,800 / −10,344 vs 10,000）；昨夜已演示 `max_delta` 熔断。
- **N2（新）**：`15:00:01 error: session_end non-zero gross position 763 lots across 54 codes`——auto-close 未覆盖/失效，763 手隔夜裸露。
- 治本：平遗留持仓；过渡：调 `riskLimits`（运营决策）。

### N1【高】GVV 求根日失败 ≥6.5 万次，翼部定价持续劣化

- 节流日志内部计数：1,000（00:43）→ 21,000（09:12）→ 41,000（09:34）→ 61,000（10:33）→ **65,000（末日）**；失败点集中在近 ATM 翼区（|x|=0.07~0.21）。
- 影响：`GvvVolCurve::eval` 失败回退 ATM vol（`fallback ATM`）——大部分行权价的 theo 实为平直 vol 报价，做市报价质量隐性劣化（也与 qmode/REJECT-ONLY 判定联动）。

### N4【高】EXPIRYENABLE 对配置不全月份开放（段错误前置条件）

- 见 P0-3。19:08 实测 `EXPIRYENABLE,202702,on -> ok` 生效（前版报告曾错误称"会拒绝"）。
- 建议：UI 对"配置不全（缺 underlyingCode）"月份加风险提示或后端直接拒开。

### N3【中】框架 NULL 解引用族

见 P0-3 源码缺陷列表。属于确定性代码级缺陷，无论是否为本次崩溃点都应修复。

### N5【中】MarketReadiness 阻断

- `202612 BLOCKED (insufficient valid points)` 10:10——做市主力月 theo 报价中断窗口；`202704` 09:01/09:18/09:49 反复阻断。

### P2-6【运营】资金不足拒单 1,032 次（GBK 解码确证）

- 分布：00 时 483（启动 27 分钟爆发）、09 时 312、10 时 237；另有"平今仓位不足"6 次。账户在遗留持仓保证金下实质耗尽。

### P2-7【环境】SimNow 连接质量

- 4097 集群式踢线（19:57:27 两秒 8 次）、MD 8193 ×7、channel_lost 10:46/16:00/19:57（10:46 当秒即重登成功，恢复机制有效）。

### 其他（低）

- REJECT-ONLY 进入 17 次（00:33 启动窗口 8 合约），**代码有退出路径**（`OptionQuoteManager.cpp:196/719-720`）但今日 0 条退出记录，是否生效无法从日志确认；
- OQM late fill ×13、cancel-stuck ×13、`app discard signal 17`（SIGCHLD）×4；
- `hot_params.json` 的 `tdate=0`（启动时序小瑕疵）；
- 运维记录：对运行中 WtRunner 的日志 `: >` 截断曾造成混乱，本次证据已验证最终日志无 NUL 洞、时间线连续；以后清理应先停进程或换文件。

## 3. 今日正常面（记录）

日盘 session 时间线精确；热参数链路工作正常（UI/REST 修改均落盘回放）；09:00 后市场自动恢复做市；10:46 通道瞬断当秒自愈；UI/middleware 全天无事故。

## 4. 证据摘录（关键计数）

| 模式 | 计数 | 时间窗 |
|---|---|---|
| not in trading time（卖） | 59,280 | 01:00–08:59 每小时 ~7,400 |
| PositionGuard skipping | 40,411 | 全天 |
| PositionGuard DISCREPANCY | 5,703 | 00:33 首帧起，全天演化 |
| CTP 资金不足 | 1,032 | 00 时 483 / 09 时 312 / 10 时 237 |
| GVV eval 失败（内部计数） | ≥65,000 | 00:43–末日 |
| OQM frozen | 13 | 00:33–00:52 |
| REJECT-ONLY 进入 | 17 | 00:33 为主 |
| max_vega 熔断 | 2 | 00:33/00:44 |
| 4097 踢线 | 8+ | 19:57:27 |

## 5. 复核修正记录（本报告相对前版的主要更正）

1. **P0-1 归因**：主使组件不能用 "Target position updated" 日志排除（该日志是 `do_set_position` 成交记账，非开火记录）；决定性证据是 decide() 的 2 手钳制 vs 实际 10 手。"10 手来源"列为未解疑点。
2. **P1-4**：删除"09:00 后自然停止/巧合对齐"的错误结论——背离以新数值持续全天（最后 10:43:37）。
3. **GVV**：日失败计数从"6.1 万"修正为"≥6.5 万"（末日节流计数 65,000）。
4. **EXPIRYENABLE 202702**：从"会拒绝（未配置）"更正为"生效（config 有 5 个月份）"，并据此修正段错误触发链。
5. **REJECT-ONLY**：从"无退出"更正为"代码有退出路径但今日无退出记录"。

## 6. 行动项（优先级）

| # | 事项 | 性质 |
|---|---|---|
| 1 | 明早 08:55 盘前重启（重置冻结/REJECT-ONLY/PositionGuard） | 运维 |
| 2 | 处置遗留 763 手持仓（治本 P1-4/P1-5/P2-6/N2） | 运营决策 |
| 3 | P0-1：时段门控 + 框架交易时段拒单补事件/同错误连续退避 | 设计缺陷 |
| 4 | P0-2：冻结态会话级解冻 | 设计缺陷 |
| 5 | core dump + `executeOrderIds` qty 插桩（收网 P0-3 与 10 手之谜） | 取证 |
| 6 | 修复框架 NULL 解引用族（N3） | 代码缺陷 |
| 7 | config 补 `positionGuard`（clamp/undoWindow）或启动 broker 强制对账 | 配置/对账 |
| 8 | GVV 求根失败治理（翼部定价质量） | 定价质量 |
| 9 | UI/后端：配置不全月份拒开 EXPIRYENABLE | 交互防护 |
