# WtFutuCore 生产运行巡检与业务逻辑诊断

> **巡检时间**：2026-10-18 12:30 CST（周日，无交易）
> **对象**：`ubuntu@<prod-host>:/home/ubuntu/projects/wondertrader/dist`，PID `3015760`
> **方式**：SSH 只读巡检（进程/日志/配置/产物），未启动、停止或修改任何生产进程与文件
> **最新交易日**：2026-10-19 夜盘（2026-10-16 20:40 启动，最后日志 2026-10-17 03:00）
> **结论口径**：`P0` 资金/仓位风险；`P1` 高风险或系统性缺陷；`P2` 可观测性/治理

---

## 1. 结论摘要

1. **进程健康，但“活着”不等于“在工作”**：进程已连续运行 1 天 15 小时，CPU 17%、内存约 1.2 GB、磁盘 48%；但 10-16 开盘后出现 **09:02:29–09:12:10 约 10 分钟交易通道未就绪**（结算确认卡住），期间完全无法报价。
2. **收盘平仓是当前最严重的业务缺陷**：10-16 14:58 `INE.ec2611` 平仓连续 10 次 FAK 零成交，`CLOSEOUT_FAILED`，随后 **263 行 `Max retries (10) exhausted` 日志刷屏**；至 03:00 通道断开时仍持有 `EC2611=5 / EC2612=-12`。且该现象 **10-13 至 10-16 每天都发生**（10–17 次 `CLOSEOUT_FAILED`、97–313 行 max-retry 日志）。
3. **开盘僵尸单熔断**：09:00:41 三个合约因 `zombie-cancel-latch` 被 `HALT_QUOTING`，根因是启动时从 `order.membin` 载入了上一交易日的旧单，开盘撤单无 ack → B+ 重试 → zombie。该问题与 09:02 通道断开叠加，放大了开盘停摆。
4. **ARB 告警严重刷屏且疑似失真**：10-16 共 903 条 `ARB_RISK`，其中 672 条为 `Correlation breakdown detected`，值恒为 `0.0000/thr=0.3000`；这些只是 WARNING 外发，不触发 any 闸门，属于“假保险丝 + 噪声”。
5. **审计/监控链路不可信**：`reconcile_journals.py` 默认只对 AO 账本、且用全部策略的成交日志做对比，EC 的“MISMATCH”是工具口径问题；EC 的 `trades.csv` 在夜盘重启后仅剩表头，`funds.csv` 停留在 9 月 29 日，**没有可用的当日 PnL**。
6. **临时配置仍在生产**：`orderErrorThreshold: 1000` 仍是 2026-08-17 的临时值，注释写明“框架分类修复后调回”，至今未回调；这会显著延迟订单错误熔断。
7. **生产运行的是旧版插件**：远端 `libWtFutuCore.so` md5 `2b0480bb…`，不是本轮“策略插件边界”构建（`8c470edf…`）；本轮改动尚未部署，符合“未触碰生产”的约定。

**一句话**：策略的报价与做市主链路在正常工作，但 **“开盘清理、收盘平仓、风控告警、审计对账”四个边缘链路都存在系统性缺陷**，其中收盘平仓失败会直接把仓位留过夜。

---

## 2. 运行态快照

| 指标 | 观测值 |
|---|---|
| 主机 | `VM-0-9-ubuntu`，Ubuntu，CST |
| 进程 | `dist/WtUftRunner -c dist/config.yaml -l dist/logcfg.yaml` |
| PID | `3015760`，启动于 2026-10-16 20:40:04 |
| 运行时长 | 1 天 15 小时 50 分（巡检时） |
| CPU / 内存 | 17.0% / 0.8%（约 1.2 GB） |
| 负载 | 0.16 / 0.22 / 0.26 |
| 磁盘 | 72 G / 158 G，48% |
| 日志体积 | `Logs/` 约 32 GB，112 个文件 |
| 插件 | `uft/libWtFutuCore.so` md5 `2b0480bb40c5249abd05a3885d30bf0f` |
| 配置 | `config.yaml` 2026-10-13；`coordinator.yaml` / `hotparams.yaml` 2026-09-19 |
| 最新日志 | `Runner_2026-10-17.log` 最后一行 2026-10-17 03:00:08 |
| 交易状态 | 周日无交易；最后一笔夜盘属于交易日 20261019 |

> 说明：2026-10-18 为周日，`Logs/` 无当日交易日志；`generated/outputs/.../trades.csv` 被每分钟 cron `convert_trades.py` 重写，EC 仅剩表头（夜盘无 EC 成交，属正常）。

---

## 3. 近五个交易日运行指标

| 日期（日志） | 成交笔数 | cancel ack timeout | zombie halt 行 | CLOSEOUT_FAILED | Max retries 行 | ARB_RISK | TRADING_HALTED |
|---|---:|---:|---:|---:|---:|---:|---:|
| 2026-10-13 | 670 | 4,044 | 95 | 17 | 313 | 854 | 14 |
| 2026-10-14 | 703 | 4,165 | 101 | 10 | 292 | 988 | 16 |
| 2026-10-15 | 556 | 2,057 | 41 | 10 | 97 | 872 | 16 |
| 2026-10-16 | 724 | 3,482 | 69 | 10 | 263 | 903 | 10 |
| 2026-10-17（仅夜盘 00–03） | 9 | 0 | 0 | 0 | 0 | 68 | 4 |

**判断**：

- 平仓失败、撤单超时、zombie 熔断不是偶发，而是 **每个交易日都出现**；
- 10-15 指标明显较好（2,057 超时、41 zombie、97 max-retry），说明与当日行情/开盘状态相关，存在可优化空间；
- 10-17 夜盘本身干净，4 次 `TRADING_HALTED` 均发生在 03:00 通道断开（收盘正常事件），不是风控误触发。

**10-16 成交分布**（`Trade notified`）：

| 合约 | 全日成交 | 日盘 09–14 |
|---|---:|---:|
| `INE.ec2611` | 82 | 82 |
| `INE.ec2612` | 108 | 108 |
| `SHFE.ao2611` | 307 | 217 |
| `SHFE.ao2612` | 227 | 144 |

EC 无夜盘，夜盘成交全部为 AO；因此 EC 的 `trades.csv` 在 20:40 重启后为空是预期现象，但 **日盘 EC 成交在重启时被覆盖**，导致事后审计只能依赖 `trade.membin`。

---

## 4. 事件复盘

### 4.1 开盘：旧单清理 → zombie 熔断 → 10 分钟暗管

日志时间线（`Runner_2026-10-16.log`）：

```text
08:40:04  进程启动，加载本地 position/order/trade/round membin
09:00:30  UnifiedOrderTracker: cancel ack timeout (1000ms), retry #1 ...（多笔）
09:00:41  INE.ec2612 HALT_QUOTING [zombie-cancel-latch]: net position -3
09:00:41  INE.ec2611 HALT_QUOTING [zombie-cancel-latch]: net position 2
09:00:41  SHFE.ao2612 HALT_QUOTING [zombie-cancel-latch]: net position -2
09:02:29  Trading channel disconnected: 8193
09:02:29  Login succeed, trading date: 20261016
09:03:00  [CHANNEL] trading channel not ready for 30s — pipeline dark
09:08:00  [CHANNEL] trading channel not ready for 330s — pipeline dark
09:12:10  Trading channel ready
09:12:10  AO: channel ready with 5 zombie orders -> engine cancelAll sweep
09:12:10  EC: channel ready with 9 zombie orders -> engine cancelAll sweep
09:12:10  Trading resumed after channel ready
```

**根因链**：

1. UFT 上下文启动时从 `generated/outputs/<id>/order.membin` 恢复上一交易日的订单；这些订单在交易所侧已经失效。
2. 09:00 开盘后 `UnifiedOrderTracker` 对这些旧单发起撤单；柜台不回 ack（订单已不存在），1 秒后触发 `cancel ack timeout` 重试。
3. B+ 重试达到阈值后升级为 zombie，`FutuRiskMonitor` 对合约加 `zombie-cancel-latch`，直接 `HALT_QUOTING`。
4. 与此同时柜台通道在 09:02:29 断开（8193），重新登录后卡在结算确认，直到 09:12:10 才 ready；策略侧只能等待，期间无报价。
5. `channel_watchdog.sh` 未重启进程（进程仍活着），只输出了 `WD-MD-DOWN` 告警；且该告警来自 **Parser 日志静默**，而 Parser 日志本来就不按 tick 打日志，属于误报。

**业务影响**：开盘 12 分钟无报价；3 个合约被 zombie 闩锁；9 个旧单被 engine 全撤清扫。对于一个做市策略，开盘是最重要的流动性窗口之一。

**可修点（纯策略侧）**：

- 启动/新交易日时，**不要把上一交易日的订单喂给 B+ 撤单重试**：按 `place_time`/`trading_day` 将旧单标记为 stale/terminal，直接清槽；或只做一次 `stra_cancel_all` 清扫，不进入 zombie 升级。
- 在通道 ready 前设置“启动宽限期”（例如前 N 个 tick 不因撤单超时触发 zombie）。
- `on_channel_ready` 已有 zombie 清扫逻辑（`FutuRuntimeOps.cpp`），但它发生在 09:12，已晚于开盘。

### 4.2 收盘：FAK 平仓反复零成交 → 仓位过夜

日志时间线（`Runner_2026-10-16.log`）：

```text
14:58:05  EC CLOSEOUT: starting CloseoutExecutor (code=INE.ec2611, delta=-9.00, hedge_ratio=1.00)
14:58:03  [CLOSEOUT] Order rejected (zero fill) ... localid=1245802942, marking FAILED for retry
14:58:10  EC CLOSEOUT: starting CloseoutExecutor (code=INE.ec2611, delta=-8.00)
14:58:12  zero fill ... retry
...
14:58:38  [RISK] CLOSEOUT_FAILED: retries exhausted: 10/10, manual intervention required
14:58:39  [CLOSEOUT] Max retries (10) exhausted, manual intervention required   ← 之后每 tick 重复
...
14:59:59  [CLOSEOUT] Max retries (10) exhausted, manual intervention required
03:00:08  Position snapshot on channel lost: INE.ec2611 pos=5, INE.ec2612 pos=-12
```

**关键事实**：

- 平仓目标是 **anchor delta**：`delta=-9` 表示组合净 delta 为 -9，通过买 `ec2611` 把 delta 压到 0；不是把两个合约都平到 0。日志中的 `delta` 从 -9 → -8 → -7 说明有少量成交，但最终仍有 `EC2611=5 / EC2612=-12`（净 delta -7）残留。
- 配置：`closeout.minutesBefore: 2`、`maxRetries: 10`、`retryIntervalMs: 2000`、`useFak: true`、`sweepThresholdMs: 5000`。
- FAK + `PASSIVE` 价格：卖出价为 `ask - tick`；当价差大于 1 tick 时并不穿过买一，FAK 立即撤销 → 零成交。这正是日志中的“zero fill”。
- `CloseoutExecutor` 本来有价格追逐：`_consecutive_zero_fills >= 2` 时强制 `SWEEP`（`CloseoutExecutor.cpp:303-305`），但 **每次 FAILED 后 `CloseoutOrchestrator` 调用 `executor->reset()`，随后 `start()` 又把 `_consecutive_zero_fills` 清零**，价格追逐永远从第 0 次开始，无法升级到 sweep。
- `FutuRiskMonitor::checkCloseoutRetry` 在 `retry_count >= max_retries` 时 **每次调用都打一条 error**（`FutuRiskMonitor.cpp:1050-1055`），而该函数在 FAILED 状态每 tick 被调用，造成 263 行刷屏。
- 平仓失败后没有强制 halt/更强动作，只有一次 `CLOSEOUT_FAILED` 广播；15:00 收盘后进程继续存活，仓位带到下一交易日。

**可修点（纯策略侧）**：

1. `CloseoutExecutor::start()` 区分“首次启动”和“失败重试”：重试时保留 `_consecutive_zero_fills`、`_remaining`、`_prev_round_*`；或 `CloseoutOrchestrator` 在 FAILED/RETRYING 不要 `reset()`，只重新进入 DRAINING。
2. FAK 零成交时 **立即升档到 SWEEP/对手价**，不要等 2 次；或者平仓阶段不用 FAK，改用可撤限价 + 价格追逐。
3. 达到 max retries 后进入**终态**（如 `FAILED_FINAL`），只广播一次 critical、停止每 tick 日志，并对该合约/策略执行 halt 或下一 session 强制平仓。
4. 明确平仓目标：若目标是“平掉全部持仓”，当前只交易 anchor 的设计会留下对冲腿；若目标是“delta 中性”，需要增加 **gross exposure/margin** 约束，避免只平 delta 但 gross 仍为 17 手。
5. 加入收盘后仓位告警：session end 时若 `|position| > 阈值`，必须 error + 外部告警，而不是仅 log。

### 4.3 ARB 告警：903 条/日，672 条相关性命中，值恒为 0

`Runner_2026-10-16.log`：

```text
ARB_RISK total 903: type=2 (CORRELATION_BREAK) 672; type=0 197; type=4 (DIVERGENCE) 34
sample: [ARB_RISK] pair=ao1112 level=1 type=2 val=0.0000/thr=0.3000: Correlation breakdown detected (throttled 60000ms)
sample: [ARB_RISK] pair=ao1112 level=1 type=4 val=10.8473/thr=5.0000: Spread divergence high
```

- `level=1` 是 `WARNING`；`UftFutuMmStrategy::handleRiskAlert` 只对 `EMERGENCY` 执行 halt + disable arb，WARNING 仅转发/日志。因此这 672 条 **不构成任何交易约束**，是“假保险丝”。
- `val=0.0000` 持续出现，说明 `SpreadCalculator::_correlation` 被置 0（方差 < 1e-10 或样本不足）而不是真实的“相关性破裂”。`is_active` 闸门能挡住预热期，但没有挡“有效样本但方差为 0”的情况。
- `type=4` 的 `zscore` 6–10 超过阈值 5，但同样只 WARNING；如果这是设计中的风控条件，应接入 `applyB3Gate` 或 `disable`；如果不打算接入，应降级/限流，避免 900 条/日噪声。

### 4.4 审计、对账与 PnL

- `reconcile_journals.py` 默认账本路径是 `generated/outputs/futu_mm_ao_live/trade.membin`；cron 只传 Runner 日志，未传 EC 账本，因此 **EC 根本没有被自动对账**。
- 脚本的 `parse_log` 解析所有 `[trader_ctp] Trade notified`，不按策略/合约过滤；用 EC 账本跑时会把 AO 的成交也算进去，产生“EC 545 行 vs 676 笔”的假 MISMATCH。10-16 的 `journal(545,1027) vs log(676,1699)` 正是这个口径问题，不能据此判定 EC 账实不符。
- EC 的 `trades.csv` 只有表头（夜盘无 EC 成交 + 20:40 重启覆盖日盘文件）；`funds.csv` 最后更新 2026-09-29；`bilateral_stats` 只有 AO 夜盘（EC 无夜盘）。因此 **没有可信的当日/近五日 PnL**，只有成交笔数、持仓快照和逐 tick 日志。
- `Logs/` 已 32 GB，无轮转/保留策略；`channel_watchdog.log` 单文件 680 KB/日、`WD-MD-DOWN` 3,504 条，属于噪声。

### 4.5 监控误报

`channel_watchdog.log` 在 10-16 23:00–23:58 每 2 分钟输出：

```text
WD-MD-DOWN Parser log silent 11876s (.../Logs/Parser/parser_ctp_2026-10-16.log)
```

此时 AO 夜盘仍在成交（23:59 仍有 `BILATERAL_STATS`），说明 **Parser 日志静默 ≠ 行情中断**。watchdog 以 Parser 日志 mtime 作为 MD 健康信号，造成 3,504 条/日误报；同时真正 09:02–09:12 的“交易通道未就绪”没有被它重启，因为进程还活着。

---

## 5. 业务逻辑问题清单

### P0

| ID | 问题 | 证据 | 影响 | 建议 |
|---|---|---|---|---|
| F-P0-1 | 收盘平仓 FAK 零成交后无法升档，重试重置价格追逐 | `CloseoutExecutor.cpp:22-50`（start 清零）、`:303-305`（zero-fill→SWEEP）、`CloseoutOrchestrator.cpp:240-246`（FAILED reset）、`:332`（每次 start） | 仓位过夜，10-13~10-16 每日复现 | 保留 retry 状态；零成交立即 SWEEP；终态 halt |
| F-P0-2 | 平仓失败后每 tick 刷 `Max retries`，无终态 | `FutuRiskMonitor.cpp:1050-1055`；10-16 263 行 | 日志/IO 压力，掩盖真实告警 | 只报一次，进入 FAILED_FINAL，停止重试 |
| F-P0-3 | 平仓只做 anchor delta，gross 仓位不过夜约束缺失 | `CloseoutOrchestrator.cpp:332`、`CloseoutExecutor` anchor 注释；10-16 收盘 `EC2611=5 / EC2612=-12` | delta 中性但 gross 17 手、保证金/跳空风险 | 明确“平全部”或“平 delta + gross 上限”，收盘校验 |
| F-P0-4 | 启动加载上一交易日旧单，开盘撤单超时→zombie 熔断 | 09:00:30 timeout、09:00:41 zombie、09:12:10 5/9 zombie sweep | 开盘停摆，旧单风暴 | 新交易日/启动时 stale 单直接清槽，不进 B+ |

### P1

| ID | 问题 | 证据 | 影响 | 建议 |
|---|---|---|---|---|
| F-P1-1 | 通道 09:02–09:12 未就绪，策略无升级动作 | `[CHANNEL] ... 330s`、09:12:10 ready | 开盘 10 分钟无报价 | 策略侧超时告警/请求重启；watchdog 改用通道状态 |
| F-P1-2 | ARB 相关性告警值恒 0，仅 WARNING，无闸门 | 672 条/日 `val=0.0000/thr=0.3000`；`SpreadRiskManager.cpp:166-178` | 900 条/日噪声；风险条件不生效 | 增加 correlation_valid；要么接线 gate，要么限流 |
| F-P1-3 | `orderErrorThreshold: 1000` 临时值仍在生产 | `config.yaml:95/197`（注释“框架修复后调回”） | 订单错误熔断严重滞后 | 框架分类修复后回调；加配置审计 |
| F-P1-4 | 对账/PNL 不可信 | `reconcile_journals.py` 默认 AO、无过滤；EC cron 缺失；`funds.csv` 9-29 | 无法做日终核对/绩效评估 | 按策略传账本+按合约过滤；补 EC cron；日终 PnL |
| F-P1-5 | watchdog 以 Parser 日志静默判 MD 故障 | 3,504 条 `WD-MD-DOWN`，而 AO 仍在成交 | 误报淹没真告警 | 改用 tick 计数/通道状态；去重 |
| F-P1-6 | 撤单超时/重试量大（2k–4k/日） | 10-13~16 表；13:37/14:50/14:56 峰值 | CTP 流控、zombie 风险 | 降低撤单频率/TTL，批量撤单，收盘前限流 |

### P2

| ID | 问题 | 建议 |
|---|---|---|
| F-P2-1 | `Logs/` 32 GB 无保留策略 | 按日/周轮转，保留 N 天，压缩归档 |
| F-P2-2 | `bilateral_stats` 夜盘仅 AO、EC 日盘才有 | 明确各品种交易时段；跨时段汇总 |
| F-P2-3 | 日志中大量 debug 逐 tick（`[QUOTE]`/`[REQUOTE]`） | 生产降级/采样，保留 KPI |
| F-P2-4 | 配置 mtime 分散（config 10-13、coordinator/hotparams 09-19） | 版本化配置，部署时记录 md5 |

---

## 6. 修复建议（仅策略侧，按优先级）

> 遵循既定原则：不改 `WtCore/WtUftCore/WtRunner/WtRunner/Includes` 等框架文件；以下改动均在 `src/WtFutuCore/` 内。

### 6.1 收盘平仓（F-P0-1/2/3）

1. `CloseoutExecutor::start()` 拆成 `startNew()` 与 `resumeRetry()`：
   - `startNew` 清空 `_consecutive_zero_fills/_remaining/_rounds`；
   - `resumeRetry` 只重置 `_inflight_qty`，保留 zero-fill 计数与剩余量。
2. `CloseoutOrchestrator::onTick` 的 FAILED 分支不要 `executor->reset()`；在 RETRYING 时走 `resumeRetry`。
3. `CloseoutExecutor::run`：FAK 零成交第一次就把 `tier` 提到 `SWEEP`（或 `VERY_AGGRESSIVE`），不要等 2 次。
4. `checkCloseoutRetry` 达到 `max_retries` 后：
   - 置 `CloseoutSub::FAILED_FINAL`（新增枚举）；
   - 只广播一次 `CLOSEOUT_FAILED`（critical）；
   - 对该合约 `haltQuoting`，并在 `on_session_begin` 前禁止恢复；
   - `session_end` 时若仍有仓位，error + `EventNotifier` 告警。
5. 若产品要求“收盘全平”，将 `flatten_all_legs` 作为配置项：对 `contracts` 逐个下平仓单，而不是只交易 anchor；若坚持 delta 中性，增加 `maxGrossPosition` 并在收盘检查。

### 6.2 开盘旧单（F-P0-4）

1. `UftFutuMmStrategy::on_init` 或首个 `on_tick` 中，遍历 `UnifiedOrderTracker`：
   - `place_time` 早于当前交易日/当前 session 的订单，标记 stale 并直接 `untrack`（不进入 cancel 重试）。
2. 若必须撤销旧单，调用一次 `stra_cancel_all("")`（引擎全撤）作为清扫，不把结果喂给 B+。
3. 增加“启动宽限期”：`on_channel_ready` 后前 N 秒/前 N 个 tick 不因 cancel timeout 升级 zombie。
4. 在 `on_session_begin` 记录 `loaded_stale_orders`，KPI/告警可观测。

### 6.3 通道与监控（F-P1-1/5）

1. 策略侧：`on_channel_lost` 记录 `channel_lost_ts`；在 `on_tick`/timer 中若 `!channelReady && now-lost > 180s`，输出 `error` 并 `EventNotifier` 告警；提供“请求进程重启”的运维钩子（或由外部 watchdog 读取该日志）。
2. `channel_watchdog.sh` 不要用 Parser 日志 mtime；改用 `Runner` 日志中的 `[CHANNEL]`/`trading channel ready` 或 `tickCount` 增量；对 `WD-MD-DOWN` 做 10 分钟去重。
3. 开盘 09:00–09:15 单独告警窗口：若 `channel not ready` 超过 60s，立即通知。

### 6.4 ARB 告警（F-P1-2）

1. `SpreadCalculator::computeCorrelationAndBeta` 增加 `_correlation_valid`：`valid_count >= min_samples && var1/var2 >= 1e-10`；`SpreadState` 暴露该标志。
2. `SpreadRiskManager::updateAlerts` 仅在 `state.is_active && state.correlation_valid` 时产生 `CORRELATION_BREAK`。
3. 如果相关性是实盘风控条件，在 `applyB3Gate` 中拒绝新开仓；否则把 WARNING 降为 debug/限流（例如 5 分钟一条）。
4. 对 `type=4` 的 `zscore` 告警同理：要么接线 gate，要么降噪。

### 6.5 对账/PnL（F-P1-4）

1. `reconcile_journals.py`：
   - 必须显式传 `--journal <path>` 与 `--instruments SHFE.ao,INE.ec`；
   - `parse_log` 先按 `instrument` 过滤，再按 `td` 汇总；
   - cron 增加 EC 账本（`futu_mm_ec_live/trade.membin`）。
2. 每交易日 `session_end` 输出 `funds/positions/trades` 快照到按日期归档目录，不覆盖前一交易日。
3. `trades.csv` 改为追加/按日文件，`convert_trades.py` 从 `trade.membin` 读 `_size` 并保留历史。
4. 增加 `daily_pnl.csv`（策略、合约、已实现/未实现、手续费、持仓）。

### 6.6 配置与日志（F-P1-3/F-P2）

- 恢复 `orderErrorThreshold` 到合理值（如 20–50），并在 `AGENTS.md` 记录“临时值已回调”。
- 日志轮转：`Logs/` 保留 7–14 天，压缩归档；`channel_watchdog.log` 去重。
- 生产日志级别：`[QUOTE]/[REQUOTE]` 降为 debug 并按 N 条采样；保留 `[RISK]/[CLOSEOUT]/[CHANNEL]/[KPI]`。

---

## 7. 验证与部署建议

1. 以上均为策略侧改动，先在 `src/build_all` Debug 编译 + TestUnits；再用 `dist/WtBtFutu` 跑 `_ec_5d` 回测（预期成交/PnL 在 mocker 噪声内）。
2. 平仓/开盘逻辑回测无法完全覆盖（mocker 撤单必成功、FAK 必成交），必须：
   - 单元测试注入“撤单无 ack / FAK 零成交 / 旧单加载”；
   - 实盘小流量/仿真环境观察 1–2 个交易日；
   - 部署前备份远端 `.so`（现有 `.bak_*` 习惯）。
3. 远端当前 `.so` 为 `2b0480bb`；本轮插件边界构建 `8c470edf` 未部署。建议先完成 F-P0 修复，再一次性部署，避免只带“边界改动”的版本进入生产。
4. 监控：把 `CLOSEOUT_FAILED`、`CHANNEL not ready > 60s`、`zombie halt`、`orderErrorThreshold` 命中做成独立告警；不要依赖 `WD-MD-DOWN`。

---

## 8. 附录：巡检命令（只读）

```bash
ssh ubuntu@<host> 'ps -eo pid,lstart,etime,pcpu,pmem,args | grep -E "WtUftRunner|WtRunner"'
ssh ubuntu@<host> 'readlink /proc/<pid>/cwd; md5sum <dist>/uft/libWtFutuCore.so'
ssh ubuntu@<host> 'grep -ac "cancel ack timeout" <dist>/Logs/Runner_2026-10-16.log'
ssh ubuntu@<host> 'grep -a "CLOSEOUT\\|zombie-cancel-latch\\|CHANNEL" <dist>/Logs/Runner_2026-10-16.log | tail'
ssh ubuntu@<host> 'python3 <dist>/monitor/reconcile_journals.py ...'
```

> 本报告只做只读巡检，未修改远端任何文件/进程/配置；所有时间均为 CST，日志时间为交易所/主机本地时间。
