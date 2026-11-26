# WtOptionCore 运行诊断报告（2026-11-24）【v2 重写版】

> **修订说明**：v1 版（commit `da445846`）经复核发现多处改变结论的实质错误与关键遗漏（见第 10 节更正清单），且当时尚未执行 22:01 夜盘重启验证。本版基于 11-24 全天日志复查 + 22:01 重启后实盘验证 **全文重写**，v1 中的结论一律以本版为准。
>
> **数据来源**：
> - 生产 `dist/WtRunnerOption/Logs/Runner_2026-11-24.log`（单实例连续时间线 00:33:04 → 23:3x，含 22:01 重启）
> - `Logs/Trader/simnow_2026-11-24.log`（CTP 通道/报单/成交/持仓）
> - `Logs/Strategy/option_mm_ag_2026-11-24.log`（策略分类日志）
> - `common/contracts.json`、`config.yaml`、`tdtraders.yaml`、`generated/hot_params.json`
> - UI/middleware WS 快照（8002 / 8710 实测）
> - 仓库源码逐条对照（src/WtOptionCore、src/WtCore/TraderAdapter.cpp、src/TraderCTP/TraderCTP.cpp、src/WtCore/HftStraBaseCtx.cpp）
>
> **方法**：错误/警告模式分类计数 → 分时分布 → 逐代码归因 → 源码语义核实。所有数字均以 `grep -ac` 全量计数复核。

---

## 0. 结论速览

| # | 问题 | 一句话结论 | 严重度 |
|---|------|-----------|--------|
| P0-1 | 交易通道重连卡死 | 10:46:05 断线后当秒登录成功，但卡死在结算确认查询，Disabled 持续到 22:01 人工重启。**SimNow 仿真柜台特有，生产柜台不存在；不改框架层** | P0 |
| P0-2 | **期权做市当日全灭**（本轮新定位） | SimNow 资金不足(error 31) → 每个合约 3 次拒单即永久闩锁 REJECT-ONLY；日内无释放路径，session-begin 重置在 OQM 创建前执行形同虚设 → **00:52:35 起全市场零期权报价，日盘 09:00–15:00 完全没有做市** | P0 |
| P0-3 | 期货对冲风暴 | 隔夜 01:00–09:00 每小时 ~7,400 条 `not in trading time`（59,280 条）；日盘 09:00–10:46 对冲尝试 223 次×10 手几乎全部 error-31 拒绝 | P0 |
| P0-4 | PositionGuard 假背离 | 框架对每合约先 long 后 short 两次回调，guard 首帧单边初始化 → 第二边到达即误判背离并禁用；`reconcile()` 无调用点 | P0 |

**运营层根因**：SimNow 账户资金耗尽（error 31 全天 474 次），做市的 buy 侧和对冲腿全部被拒；叠加策略侧多个"一次性闩锁、无日内释放"的保护机制，把资金问题放大成了全天报价/对冲瘫痪。

---

## 1. 时间线（状态机与关键事件）

| 时间 | 事件 |
|------|------|
| 00:20:14 | 进程启动（前一晚进程在 00:27 因 SIGSEGV 死循环被人工处置），Trader 登录成功 |
| 00:27 | SIGSEGV 崩溃死循环 → 杀进程、截断 3.7GB 日志 |
| 00:33:04 | 重启成功；session begin 20261124；Normal；**52 个 cu2612 期权 grid 创建**（当日到期） |
| 00:33:08–00:59 | 夜盘做市：报价迅速衰减（见 P0-2），00:52:35 后零期权报价 |
| 00:59:31 | SessionStopped / OFF（夜盘收盘） |
| 01:00–09:00 | 隔夜对冲风暴：59,280 条 `not in trading time`（P0-3） |
| 09:00:35 | session_resume / ON；**CTG 恢复但期权报价仍为零**（P0-2） |
| 09:00–10:01 | 日盘：期权报价 0 笔；期货对冲 141 次×10 手（error 31） |
| 10:01:27–10:01:57 | `cu2612.C.94000` 报价 30 秒（当日唯一期权成交窗口），5 次拒单后闩锁 |
| 10:14:35 / 10:30:08 | 10:15 休市停 / 复盘 |
| 10:46:05 | `Trading channel disconnected: 8193` → 当秒 Login succeed → 卡死在结算确认（P0-1）→ Disabled |
| 11:29 / 14:59 / 15:00 | 三次恢复转移均被 arbiter 拒绝（Disabled 优先级） |
| 15:00:01 | session_end：**遗留 763 手 / 54 合约**（`enable_auto_close: false`，配置关闭非失效） |
| 15:00–21:00 | 死时段，通道仍死 |
| 21:00:13 | B4 定时器触发 `CTG::resumeTrading`，但通道已死；**该交易日的 session-begin 事件未投递**（P1-6） |
| 22:01:23 | 人工重启（PID 2840926）；session begin 20261125；通道 22:01:27 ready；22:01:34 Normal/ON |
| 22:01–23:3x | 202701 正常做市（1,172 条期权信号）；REJECT-ONLY 衰减重启（22:03–22:46 又闩锁 5 个合约）；22:12:21 P.84000 触发 riskmon 撤单上限被永久排除 |

---

## 2. P0-1 交易通道重连卡死（10:46 → 22:01）

**现象**
- `10:46:05 Trading channel disconnected: 8193` → **同秒** `Login succeed` + `Querying confirming state of settlement data...` → 之后无任何下文。
- 全天 `Trading channel ready` 仅 2 次（00:20、00:33 重启 + 22:01 重启各一次属新进程）；16:00 / 19:57 又有 4097 断线，19:54 / 19:57 登录失败。
- Disabled 状态下 11:29、14:59、15:00 的恢复转移全部被 `TradingStateArbiter` 拒绝，进程自身无任何自愈。

**代码定位**
- `TraderCTP.cpp:783` 登录后发起 `queryConfirm()`（结算确认查询）；`:842` `OnRspSettlementInfoConfirm` 回调；`:1693` `ReqSettlementInfoConfirm` 请求。
- **整个登录→确认→ready 流程无超时、无重试 watchdog**：SimNow 对重连后的结算确认请求不回应时，状态机永久停在 `Querying...`。
- 框架侧 `TraderAdapter` 只有 `channel_lost → Disabled` 单向转移，无重连恢复路径。

**影响**：10:46 → 22:01 全部交易能力丧失（收盘 flatten 14:59 被拒、15:00 遗留 763 手无法处置）。

**处置结论（2026-11-25 已决策）**：确认请求挂起是 **SimNow 仿真柜台特有问题**，实际生产柜台不存在——**不改框架层 TraderCTP**（2026-11-25 曾实现的 watchdog 方案已撤销）。运营侧缓解：SimNow 环境下的部署/重启固定放在死时段（01:00–08:30 / 15:15–20:30），避免盘中重启触发重连挂起；若 SimNow 盘中再现 8193 断线且登录后无 `Trading channel initialized`，按人工重启预案处理（重启时排除列表/guard/OQM 闩锁一并归零，反而是干净的恢复路径）。

---

## 3. P0-2 期权做市当日全灭【本轮新定位，v1 未发现】

### 3.1 现象（日志硬数据）

期权（`cu[0-9]{4}.[CP].*`）分时统计：

| 小时 | Signal of | Order notified | Trade notified |
|------|-----------|----------------|----------------|
| 00（夜盘 00:33–00:59） | 473 | 1,589 | 104 |
| **09（日盘开盘小时）** | **0** | **0** | **0** |
| 10 | 14（全部 10:01:27–10:01:56） | 35 | 3 |
| 11 | 0 | 2（迟到回报） | 1 |
| **10:02–10:46 / 11–15** | **0** | **0** | **0** |
| 22（重启后） | 1,172 | 4,536 | 86 |

- `QuoteStatistics session 20261124 summary: orders=1227 fills=133 cancels=1106 rejects=1215 bilateralRatio=4.3%`——对照 11-23 `orders=8593 fills=496 bilateralRatio=0%`，活动量再降 86%。
- **00:45–00:52 全市场只剩 `SHFE.cu2701.C.102000` 一个合约在报价**（109 条信号，buy/sell 各 1 手循环），00:52:35 它进入 REJECT-ONLY 后，夜盘剩余时间、整个日盘 09:00–15:00，期权报价为零。

### 3.2 证据链（闩锁级联）

REJECT-ONLY 进入事件（`OQM: <code> REJECT-ONLY entered (entrust rejects>=3)`)：

```
00:33:12–00:33:27  15 个合约（cu2701 C.84000~C.100000、P.84000~P.94000 + cu2612×3）一次姓闩锁
00:33–00:52        其余活跃合约被 cancel-stuck 冻结(13) / PositionGuard 禁用(7) 逐步清场
00:45–00:52        仅剩 C.102000（持多 9 手，卖平+回购循环）
00:52:35           C.102000 第 3 次 entrust 拒单 → REJECT-ONLY → 全市场归零
09:00:35           session_resume 只翻转 CTG enabled，OQM 闩锁全部原样保留 → 仍为零
10:01:27–10:01:56  cu2612.C.94000（唯一未闩锁的活跃合约）报价 30 秒
10:01:57           该合约第 5 次拒单 → REJECT-ONLY → 再次归零（直至 10:46 通道死亡）
22:01:23           重启 = 新进程 + 新交易日 session begin → 计数器天然归零 → 做市立即恢复（1,172 条信号）
22:03–22:46        资金仍不足，同样的衰减重启：5 个合约先后闩锁（C.88000/P.86000/C.94000/C.84000/C.100000）
```

22:01 重启后立刻恢复、且恢复后再次出现同样的逐合约闩锁——双向反证闩锁机制是日盘归零的唯一策略侧原因。

### 3.3 代码机制

- **拒单即计数，3 次永久闩锁**：`OptionQuoteManager.cpp:590`（`m_numReject++`）→ `:186-195`（`reject_max_new_orders=3`，进入 REJECT-ONLY，`return 0`）。`m_numReject` **没有任何日内递减/释放路径**，仅 `resetCounters()`（`:712`）清零。
- **`resetCounters` 唯一调用点是 session-begin**：`OptionTradingGrid.cpp:452-465 onSessionBegin()` ← `HftOptionStrategy.cpp:2837`。但本部署时序下 **session-begin 先于 OTD/OQM 创建**（00:33:04 session begin vs 00:33:08 后 OTD 才建；22:01:23 vs 22:01:24+），且全天 **`counters reset (session begin)` 日志 0 条**——重置形同虚设；交易日中途重启更是由设计注释明示"不重置"（`OptionQuoteManager.h:177`）。
- **日盘 resume 不触碰闩锁**：B4 会话调度 `ControllableTradingGrid.cpp:149-158 resumeTrading()` 只回调 `m_sessionStopCb(false)` 翻转 `ctx->enabled`，不经过任何 OQM 状态清理。09:00:35 `Session resume: curSec=32400 lag=0s` 日志证实执行了，但无效果。
- **拒单来源是 CTP error 31（资金不足）**：SimNow 账户保证金被遗留库存（15:00 达 763 手/54 合约 + cu.2612 期货 long 82/short 101）占满，任何 OPEN 方向报单即刻被拒。00 时段 299 次、09 时 104 次、10 时 81 次、22 时 14 次（全天 placing failed 498 = 474×31 + 13×51 + 8×156 + 3×50）。

### 3.4 因果链总结

```
资金耗尽(error 31)
  └─> 每个报价合约 3 次拒单
        └─> REJECT-ONLY 永久闩锁（无日内释放）
              └─> cancel-stuck 冻结 / PositionGuard 禁用 加速清场
                    └─> 00:52:35 最后一个合约闩锁
                          └─> 日盘 09:00 resume 不重置 → 09:00–15:00 零期权报价
                                └─> 当日期权成交仅 108 笔/约 108 手，做市业务实质停摆
```

**注意**：22:01 重启后资金问题并未解决（22 时段仍有 14 次 error-31 + 50/51 平仓类失败），同样的闩锁衰减已在夜盘重现（22:03–22:46 已 5 个合约）。**若不处理资金或闩锁释放，20261125 日盘将复现全灭。**

---

## 4. P0-3 期货对冲风暴（隔夜 59,280 条 + 日盘对冲全部失败）

- **隔夜（01:00–09:00）**：59,280 条 `not in trading time`（01–08 时各 ~7,400，09 时 74）。状态实为 SessionStopped/OFF 但 `channelReady=true`，`checkDeltaHedge` 调用点（`HftOptionStrategy.cpp:1929` 附近，P1 审计改动的周期性检查）无交易时段门控；`TraderAdapter` 在时段检查处静默 `return` 不产生 reject 事件，`fireLeg` 因 ids 为空也不打日志，形成 1Hz×2 腿×10 手的纯日志风暴。
- **日盘（09:00–10:46）**：DeltaHedger fired 141（09 时）+ 82（10 时）= 223 次 ×10 手，几乎全部 error-31 拒绝（资金不足），`HedgeCoordinator: reject streak` 持续攀升。全天 fired 336 次 × 10 手 = 3,351 手尝试，期货实际成交仅 271 手（cu.2612 191 + cu.2701 80）。
- **重启后（22 时段）**：fired 仅 1 次（22:01:36 `SHFE.cu.2701 SELL 10@93300 delta_excess=45.59`），但 22:02–22:10 仍有 14 次 placing failed（31/50/51 混合），`reject streak -> 14`。backoff（`OptionHedgeCoordinator.cpp:47-52`，2^streak 封顶 32×）限制了频率，但风暴模式仍在。

**根因**：`delta_hedge.max_order_size=10`（生产 config `:109`）+ 无时段门控 + 账户无资金 + 失败不回熔断。

---

## 5. P0-4 PositionGuard per-side 假背离

**框架行为**：`WtCore/TraderAdapter.cpp:1680-1681` 对每个合约先 `on_position(long)` 后 `on_position(short)` **两次**回调。

**策略行为**：`HftOptionStrategy.cpp:3086-3090` 每次回调都取 `_positionBook.get(code).net()` 喂 guard——第一次回调时 short 侧还是旧值/空，net 是"半个快照"。

**Guard 行为**：`PositionGuard.cpp:70-82` 首次回调即用该单边 net 初始化；第二次边到达 → diff 突现 → 误判背离 → `TRADING DISABLED`。

**日志佐证**：
- 00:33 重启后：66 条 DISCREPANCY / 32 次 DISABLED（`internal=0 broker=-150/-23/-29/...`，全部为带双向持仓的合约）。
- 22:01 重启后：24 init / 21 disabled / 31 DISCREPANCY；22:47:24 `internal=0 broker=-210`（cu.2701 期货净仓 -210）。
- 09 时虽 1,794 条 DISCREPANCY 但 0 次 DISABLED——非重启时段快照两边已齐，证实假背离只在"快照边界"产生。
- 全天 `PositionGuard DISCREPANCY` 6,512 条、`TRADING DISABLED` 54 次（00:32 / 10:1 / 22:21）、`skipping updateOrders` 51,209 条（09 时 20,046 条、7 个代码；22:01 后 5 个代码 P.90000/92000/96000/98000/100000 持续刷屏）。

**未接线的保护**：`PositionGuard::reconcile()`（`:145-151`）**无任何调用点**；Config 的 `clampLimit/undoWindow` 从配置读取链路不存在（默认构造 0）。`m_disabled` 仅在后续快照 diff≤tolerance 时自动恢复，单边短仓合约永远不满足。

---

## 6. P1 问题清单

### P1-1 P.84000 被 riskmon 永久排除 + 策略不处理空订单
- 22:12:21 `[simnow] SHFE.cu2701.P.84000 cancel 470 times totaly, beyond boundary 470 times, adding to excluding list`（`tdtraders.yaml: cancel_total_limits: 470`；`TraderAdapter.cpp:549-552`）。
- 该合约撤单率 90.9%（22 时段 cancels=915 / orders=1007），重启后仍持续报价 → 排除后策略收到空 `OrderIDs` 不处理，22:12–22:31 刷 1,467 条 `is forbidden to trade`（`HftStraBaseCtx.cpp:226/247/290/310`）。
- 22:12 起 511 条 `Signal of` 白打。

### P1-2 202612 已到期但配置未清理
- `contracts.json`：cu2612 期权 `expiredate=20261124`；config `expiries.202612.enable: true`。
- 00:33 属交易日 11-24 夜盘：创建 52 个 grid、报价 36 次（最后 10:01:56）、10:10:16 `MarketReadiness BLOCKED`。
- 22:01 属交易日 11-25：grid 创建 0 个、UI `expiries` 无 202612——但**仍订阅 53 个 2612 合约**（无行情纯浪费）；`hot_params.json` 含 `targ_delta:202612` / `sprd_corr:202612` 陈旧键。
- cu.2612 期货本体（expiredate 20261215）仍活着并带 82L/101S 持仓。

### P1-3 SimNow 资金耗尽（运营层根因）
- error 31 全天 474 次；资金被遗留库存占用；realized PnL -105,460；快照 `limitsBreached=true`（vega -17,692 vs 10,000，delta 607 vs 1,000）。
- 这是 P0-2/P0-3 的共同触发器。

### P1-4 平今/平昨方向错配（error 50/51）
- 22:03–22:10 多次 `close long, qty: 1` 失败：error 50（`cu2701P84000 closetoday long`）/ error 51（`close long`）= 平仓方向/平仓量校验失败，说明内部持仓视图与 SHFE 今仓/昨仓语义不匹配（SimNow 持仓均为今仓但用了平昨方向，或平仓量已被前次尝试消耗）。

### P1-5 撤单超时冻结
- `instrument frozen`（cancel ack lost after 3 retries）14 次：00:33–00:52 13 次 + 22:12 P.84000 1 次。`m_cancelStuck` 在 `onReject` 找到对应 localid 时自动清除（`OptionQuoteManager.cpp:575-583`），无"全天冻结"证据（更正 v1）。
- `cancel ack lost` 14 次。

### P1-6 session-begin 事件在 Disabled 期间未投递
- 15:00:01 session_end 正常；但 21:00（交易日 20261125 开始）**无 `session begin` 日志**，只有 B4 定时器的 `CTG::resumeTrading`。通道死亡期间引擎未投递会话事件 → 即使通道恢复也不会执行 `onSessionBegin`（OQM 计数器/guard 状态清理）。与 P0-2 的"重置失效"互为放大。

### P1-7 其他
- `GvvVolCurve::eval solve failed -> fallback ATM` 108 条（09 时 55 条最多）。
- `OptionRisk::setHedgePosition: unknown hedge code SHFE.cu2701.*` debug 刷屏（OptionRisk 只认识期货腿，期权持仓走 `rd->setPosition`；无害但噪声）。
- `late fill` 20、`app discard signal` 6、`max_vega` 突破 4 次（00:33/00:44/22:01×2）。
- max_delta 全天 0 次（11-23 23:32 曾 1082.88 > 1000；处置决策仍待定）。

---

## 7. 全天计数总表（grep 全量复核）

| 计数项 | 值 | 备注 |
|--------|-----|------|
| PositionGuard DISCREPANCY | 6,512 | 00:917 / 01-08 各 240-270 / 09:1,794 / 10:1,017 / 22:1,270 / 23:176 |
| TRADING DISABLED due to position | 54 | 00:32 / 10:1 / 22:21 |
| OQM skipping updateOrders | 51,209 | 09 时 20,046（7 代码）/ 22:01 后 5 代码持续 |
| DeltaHedger fired | 336 | 00:112 / 09:141 / 10:82 / 22:1 |
| not in trading time | 59,280 | 01-08 各 ~7,400 / 09:74 |
| Order placing failed | 498 | 474×31 + 13×51 + 8×156 + 3×50 |
| error-31 分时 | 00:299 / 09:104 / 10:81 / 22:14 | |
| REJECT-ONLY entered | 22 | 00:33×15 / 00:52×1 / 10:01×1 / 22 时段×5 |
| instrument frozen | 14 | 00:33–00:52×13 + 22:12×1 |
| cancel ack lost | 14 | |
| max_vega 突破 | 4 | 00:33 / 00:44 / 22:01×2 |
| max_delta 突破 | 0 | 11-23 23:32 为 1,082.88 |
| late fill / app discard | 20 / 6 | |
| GvvVolCurve solve failed | 108 | |
| Trading channel ready | 2 | 00:20、22:01（00:33 为新进程） |
| 期权成交 | 00:104 / 10:3 / 11:1 / 22:86 | 日盘仅 4 笔 |
| 全天期货成交 | 475 手 | 对冲尝试 3,351 手 |

---

## 8. UI / middleware 验证（22:47 实测）

- 8002（middleware）与 8710（UiBridge）均在监听；WS `/ws` 握手 → `hello` → `snapshot` 正常。
- 快照：`state.trading=Normal / reason=recovery_first_underlying_tick / enabled=true / tdate=20261125`；202701 `quoting=true books=42/42 DTE=30`，202702–04 `quoting=false`；202612 不在列表。
- **未决差异**：
  - `params.schema` 仍暴露 5 个 `wgt_*` alpha 权重（与"alpha 权重不进面板"决议不符）；
  - `hot_params.json` `tdate=0` 且含 202612 陈旧键（持久化回放源不干净）。

---

## 9. 段错误（11-24 00:27 SIGSEGV）状态

- 无原始 core dump，且历史报告把 `doOrder / Signal of buying` 拼进原生 `stra_quote` 链；当前源码证明 quote 不走这条 buy/sell 日志路径，原栈/相邻日志至少一项不可靠。
- 19:08 开启 202702 测试未复现，且当时无真实报单证据，不能用于排除故障。
- 2026-11-26 补做 Debug+ASan 实盘配置运行：发现 `thosttraderapi_se.so` 退出时 new[]/delete mismatch，栈不经过策略或 TraderAdapter，定向屏蔽后继续检查。
- 代码审计新增两个**确定缺陷**：`TraderAdapter::quote/doEntrust` 对 contract/API 未判空；`WtKVCache::get()` 解锁后返回 mmap 内地址，扩容可使其悬空，且已有 key 覆盖无锁。两者分别补安全失败与值快照/统一锁回归。
- **历史归因仍不冒充已证实**：上述缺陷是否就是 00:27 根因，缺全线程 core 无法反推。B4 的工程闭环口径是修复可证明缺陷、保留 sanitizer 证据和取证要求，而不是把“未复现”写成根因消失。

---

## 10. 对 v1 报告（da445846）的更正清单

| # | v1 结论 | 复核事实 |
|---|---------|----------|
| 1 | "日盘 09:00–15:00 正常做市（947,550 ticks）" | **错**。947,550 是行情 tick 数。期权报价自 00:52:35 全灭，日盘仅有 10:01 `C.94000` 30 秒（3 笔成交）+ 11 时 1 笔迟到成交。session summary orders=1227 / fills=133 / bilateral 4.3% |
| 2 | "09:00–10:00 无期权报价原因未定位" | **已定位**：REJECT-ONLY 永久闩锁链（P0-2），00:45–00:52 仅剩 C.102000、00:52:35 闩锁为直接证据 |
| 3 | "状态整夜 Normal" | 错。01:00–09:00 为 SessionStopped/OFF；00:59:31、10:14:35、11:29 停/复时间线见第 1 节 |
| 4 | "对冲风暴 10 手来源不明" | 生产 config `delta_hedge.max_order_size: 10`（`:109`），`DeltaHedger fired ... 10` 日志确证 |
| 5 | "max_qsize 默认 5" | **来源已定位，但 v1/v2 都不完整**：pricer 内层默认确为 1，策略 `ExpiryConfig` 与 YAML 加载缺省却为 5；202702 未配置该键，经 EXPIRYENABLE 热开启后 `setMaxPosQty(...,5,...)` 覆盖内层 1，平坦持仓报价即 5 手。本轮统一缺省为 1、补齐生产显式配置并输出有效值 |
| 6 | "763 手是 auto-close 失效" | 是 `enable_auto_close: false` 配置关闭，非失效 |
| 7 | "10:46 断线当秒自愈" | 卡死至 22:01 人工重启；`Trading channel ready` 全天仅 2 次 |
| 8 | "13 合约全天冻结" | 无全天冻结证据：`m_cancelStuck` 有 onReject 自动清除路径；frozen 事件 14 条集中在 00:33–00:52 + 22:12 |
| 9 | "`Signal of` 证明 10:02 报价停止" | 日盘 09:00 起就无期权报价；10:01 是 2612 到期合约尾声，10:01:57 闩锁 |
| 10 | "202612 重启加载失败" | 非加载失败：合约已到期（expiredate=20261124，属交易日 11-24），22:01 无行情故 grid 0 个；00:33 属 11-24 夜盘故创建 52 个 |
| 11 | "Order placing failed 1,032" | 498（474×31 + 13×51 + 8×156 + 3×50） |
| 12 | "REJECT-ONLY 17 / max_vega 2 / late fill 13 / app discard 4" | 22 / 4 / 20 / 6 |
| 13 | "每合约两条 OQM 报价日志可证明报价活动" | OQM 正常路径无逐笔日志；报价活动须以 `Signal of`/`Order notified`/QuoteStatistics 度量 |

---

## 11. 当前风险敞口与遗留持仓（截至 23:30）

- 进程 PID 2840926（22:01:34 起），Normal/ON，202701 做市中。
- 遗留持仓：15:00 为 763 手/54 合约；22:01 快照 23 合约/649 手；cu.2612 期货 long 82 / short 101。
- `risk.limitsBreached=true`：vega -17,692（限 10,000）、delta 607。
- REJECT-ONLY 夜盘衰减进行中（22:03–22:46 已 5 合约）；P.84000 仍被排除；HedgeCoordinator reject streak 14。
- 尾盘风险：若资金不解决，20261125 日盘期权做市将复现全灭；若通道再断，P0-1 卡死路径将再次吃掉整个交易日。

---

## 12. 处置结论与决策记录

**代码修复（2026-11-25 授权 1–5，其中 1 已撤销，2–5 已实施并于 00:44 部署上线）**
1. ~~TraderCTP 重连 watchdog~~：**已撤销**——结算确认挂起是 SimNow 仿真柜台特有问题，生产柜台不存在；框架层 TraderCTP 不动（P0-1 节附运营缓解措施）。
2. ✅ **PositionGuard 快照边界**：guard 只在 short 边回调喂入 + 边界 `reset()`（解除初始化，下一帧快照重建基准）。首次部署（00:35）误用 `reconcile()`——把 423 个全新 guard 的基准戳成 broker=0，首个真实快照即 24+ 合约误判禁用（00:35:22）；00:44 修正版部署后 **DISCREPANCY/DISABLED 均为 0**，首帧 init 54 条正常。另修 `onFill` 未初始化时忽略（快照前成交不误判）。
3. ✅ **checkDeltaHedge 时段门控**：`channelReady && !channelDown && enabled && !sessionStopped`（P0-3）。
4. ✅ **REJECT-ONLY 释放**：`reject_release_sec=300s`（上线 5 分钟内已观测到 `REJECT-ONLY released after 300s` 自动恢复）+ channel_ready 补跑 OQM 计数器重置（154 条 reset 日志，修复 11-24 全天为 0 的时序缺陷）。
5. ✅ **空 `OrderIDs` 退避**：`executeQuoteIds/executeOrderIds` 空 ids 按代码退避 60s，恢复时清空。

**第二轮修复（2026-11-25 01:05 用户提出，01:05 部署，PID 2892914）**
6. ✅ **fwd/gvv 未就绪不算理论价、不报价**：原 FAST 路径注释"we do not need fit ready to quote"——首拟合发布前理论价用默认/引导 vol 计算且直接报价。改为 `isValuesReady()`（forwardReady && fitReady）门控 `CompositeOptionPricer` FAST/SLOW 与 `OptionPricer2::computeValue` 入口；UI theo 输出加 `isPriced` 防护（drought 期间不显示陈旧理论价）。拟合管线只用市场 mid/IV，不受影响（启动 1s 出拟合 v1 实证）。
7. ✅ **多层挂单 = 真实后端状态**：`max_side_orders` 只解析从未强制执行 + `wait_for_cancels=false` 撤单未确认即发新单 → 同边可滞留多笔 live。修复：`updateSide` 三个发送点前置 `activeSideCount >= max_side_orders` 闸（cancelPending 计入；撤单不受闸限，防 excess-size 死锁）。max_side_orders=1 → 每边串行：撤→确认→发。UI 无需改动。

**配置/运营（已执行）**
1. ✅ 202612：服务器 config `enable: false`（备份 `config.yaml.bak-20261125`），00:35 重启已生效（`Expiry config loaded: 202612 enable=false`）。
2. 平仓清单：`analysis/closeout-20261125.md`（723 手/24 合约，按 SHFE 平今/平昨分列；P.84000 须手工平）——待人工执行。
3. max_delta 1000：维持不动（平仓后自然回落）。
4. 部署记录：`libWtOptionCore.so`（sha256 前 16 位 `b97e4629788c40db`）于 2026-11-25 00:44 部署并重启（PID 2887359）；旧版备份 `hft/libWtOptionCore.so.bak-20261125-fixp0{,-r1}`。注意：平仓清单快照（00:12）之后系统又跑了约半小时，实际平仓前应重新核对柜台持仓。

**UI 待办（与本次故障无直接因果）**
- 移除 `params.schema` 中 5 个 `wgt_*`；检查 `hot_params.json tdate=0` 回放源。

---

## 13. 附录：证据索引（可复现）

```bash
L=Logs/Runner_2026-11-24.log; T=Logs/Trader/simnow_2026-11-24.log
# P0-1
grep -a "Trading channel disconnected\|Login succeed\|Querying confirming state\|Trading channel ready" $T
# P0-2 闩锁级联
grep -a "REJECT-ONLY entered" $L
grep -a "Signal of" $L | strings | grep -E "cu[0-9]{4}\.[CP]\." | grep -aE "^\[2026\.11\.24 00:5[0-2]"
grep -ac "counters reset (session begin)" $L        # = 0
grep -a "QuoteStatistics session" $L
# P0-3
grep -a "not in trading time" $T | grep -aoE "^\[2026\.11\.24 [0-9]{2}" | sort | uniq -c
grep -a "DeltaHedger fired" $L
# P0-4
grep -a "PositionGuard DISCREPANCY\|TRADING DISABLED due to position" $L
# P1-1
grep -a "beyond boundary\|is forbidden to trade" $L $T
```
