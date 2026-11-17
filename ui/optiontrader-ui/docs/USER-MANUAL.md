# WtOption 监控 UI 操作功能手册

> 适用版本：P1–P3 全量交付（2026-11-17）。设计文档见
> `src/WtOptionCore/analysis/ui-migration-design-2026-11-16.md`。

---

## 1. 接入方式

### 1.1 公网直连（推荐，服务器已配置）

```
首次（种 cookie）：  http://129.211.5.54:8002/?token=<运维告知的 token>
之后（直接收藏）：  http://129.211.5.54:8002
```

- token 仅首次需要；cookie 有效期 7 天，过期后再带一次 token 即可。
- 无 token 直接打开会显示 `unauthorized`（HTTP 403），属正常拦截。

### 1.2 SSH 隧道（备用，不经过公网）

```bash
ssh -N -o ServerAliveInterval=30 -L 8002:127.0.0.1:8002 ubuntu@129.211.5.54
# 浏览器 http://localhost:8002
```

注意：服务器已启用 token 认证，走隧道也需先访问
`http://localhost:8002/?token=<token>` 一次。

### 1.3 「disconnected」排查（按序）

| 现象 | 原因 | 处理 |
|---|---|---|
| 页面能开但顶部红色 disconnected | 旧标签页（隧道时代打开）停留在失效 origin，或 cookie 未种 | **关闭所有旧标签页**，用 §1.1 首次 URL 重新打开 |
| 浏览器显示 unauthorized 文字 | 未带 token 且 cookie 缺失 | 访问 `/?token=<token>` |
| 首次 URL 也打不开（转圈/超时） | 云安全组未放行 TCP 8002 或本地断网 | 控制台检查安全组；`curl http://129.211.5.54:8002/` 应返回 403 |
| 曾正常，稍后变 disconnected | 网络抖动 | 前端每 3 秒自动重连，通常自愈；长期不治按第一条处理 |

---

## 2. 界面总览

```
┌────────────────────────────────────────────────────────────┐
│ 状态横幅（交易状态色块/灯/session倒计时/快照龄期）            │
│ 希腊条 + PnL 条（组合希腊值 / 当前/已实现/成交额）              │
│ 健康卡（连接/批延迟/AEP计数/drop计数/TPS/报单统计）           │
│ 到期选择(F1–F8) │ 到期摘要(fwd/atmVol/books/fit) │ 对冲腿     │
├────────────────────────────────────────────────────────────┤
│ [Grid] [Trades] [Analysis]                                  │
├────────────────────────────────────────────────────────────┤
│ 命令面板（Refresh/STOP/RESUME/PANIC/qmode/Hot params/日志）   │
└────────────────────────────────────────────────────────────┘
```

数据流：策略进程（1Hz 全量快照 + 事件增量）→ middleware → 浏览器。
订单/成交事件实时推送；快照为权威全量，事件丢失可由下一帧自愈。

---

## 3. 顶部状态区

### 3.1 状态横幅

| 元素 | 含义 |
|---|---|
| 色块大字 | TradingState：`Normal` 绿 / `Recovering` 黄 / `Panic` 红 / `SessionStopped`、`Disabled` 灰 |
| (reason) | 进入该状态的原因（如 `session_stop`、`hotcmd_stop`） |
| channel 灯 | 交易通道（TD）就绪状态 |
| underlying 灯 | 标的行情有效（收到有效 tick） |
| PANICKED 徽标 | 风控熔断中 |
| session stop in m:ss | 距本节收盘撤单线的倒计时（无本节时隐藏） |
| 右侧 age | 快照龄期（middleware 注入）；**>3s 变红** 意味着数据链路异常 |

### 3.2 希腊 / PnL 条

- 组合希腊：Δ Γ ν νTW Θ（`OptionRisk` 权威聚合；无数据时显示 `-`）。
- PnL：当前浮动 / 已实现 / 成交额；红绿着色。
- `LIMITS BREACHED` 红色脉冲徽标 = 风控限位熔断（Greeks/日内亏损/保证金）。

### 3.3 健康卡（三列）

| 字段 | 含义 / 告警阈值 |
|---|---|
| conn | 浏览器↔middleware 连接状态 |
| snapshot age | 同上；>3000ms 标红 |
| batch p50/p95/p99 µs | AEP worker 批处理延迟 |
| aep events / queue drops | 事件总量 / 丢数（>0 标红） |
| md overflow / trader depth | AEP 队列瞬时深度 |
| ui event drops / coalesced | UI 事件 FIFO 溢出 / 快照合并（偶发可忽略，持续>10 关注） |
| TPS | 当前 / 上限 |
| orders/fills/cxl/rej | 当日会话报单生命周期计数 |
| fillRatio / cancelRatio | 成交率 / 撤单率（全局口径） |

### 3.4 到期选择器 / 到期摘要 / 对冲腿

- 到期条：每个到期一个按钮（`F1`–`F8` 快捷键），右侧圆点 =
  **fit 就绪灯**：绿=已拟合，黄=forward 就绪，红=门未开。
  悬停显示 fwdReady/fitReady/books 明细。
- 到期摘要：DTE、远期、ATM vol、vol sprd、分到期 Δ/ν、
  **booksWithBid/booksTotal**（fit 门核心诊断，0 标红）、
  fwd/fit/und 三个就绪 chip。
- 对冲腿：该到期 hedge 合约的买/卖/持仓。

---

## 4. Grid 页（核心盯盘）

### 4.1 列含义（每侧：Call 左 / Put 右）

| 列 | 含义 |
|---|---|
| bidSz / bid | 市场最优买量 / 买价（点击开卖出 Ticket） |
| last | 最新成交价 |
| theo | 理论价（未定价显示 `-`） |
| iv | 隐含波动率 |
| ask / askSz | 市场最优卖价 / 卖量（点击开买入 Ticket） |
| delta | 理论 delta（4 位小数） |
| vshift | `theoVol − atmVol`（Ctrl+V 切换显隐） |
| pos | 净持仓（含 offsets）；**单元格底色=qmode 状态** |
| pnl | 该腿当前浮动盈亏 |

### 4.2 qmode 着色（单元格底色）

| 颜色 | qmode | 含义 |
|---|---|---|
| 深绿底白字 | on (1) | 强制报价 |
| 蓝底 | auto (0) | 自动（默认） |
| 浅绿底 | close (2) | 平仓模式 |
| 红底 | off (-1) | 停报 |

### 4.3 交互

| 操作 | 效果 |
|---|---|
| 点击 bid/ask 价 | 打开 Ticket（带合约/方向/参考价） |
| 双击 pos 单元格 | **单码 qmode 切换**（off↔on；`readonly` 时提示） |
| `F1`–`F8` | 切换到期月 |
| `Ctrl+V` | 显示/隐藏 vshift 列 |
| `↑`/`↓` | 移动选中行 |
| `Esc` | 取消选中 |
| 表头复选框 | vshift 列 |
| `+`/`-`（选中行） | 快速开买/卖 Ticket（以 call 腿为对象） |
| 行单击 | 选中行（高亮） |

### 4.4 readonly 行为

`readonly:true`（当前生产配置）时：Ticket 的价格/数量输入禁用并显示
黄色提示条；双击 qmode 仅提示；命令面板的写按钮置灰。Refresh 永远可用。

---

## 5. Trades 页

- **活跃订单表**（快照权威）：id / 合约 / 方向 / 价 / 量 / 已成交 / 剩余。
  - 双击行 或 点行尾 **✕** → `cancel_order` 撤单（readonly 时隐藏）。
  - 表头 **Cancel all** → 整活单全撤（有确认语义：当前直接执行，P3 版本无二次弹窗，谨慎）。
- **成交卡**：最近 500 笔（新→旧），买绿卖红；含 fills 历史回放
  （重连/refresh 后由桥重发）。

---

## 6. Analysis 页

| 面板 | 内容 |
|---|---|
| Vol smile | 选中到期：C/P 市场 iv（实线）+ theo vol（虚线 = atmVol+vshift）+ atmVol 水平虚线；悬停数据见到期摘要 |
| PnL | 组合 cur/realized 曲线（快照流环形缓冲，约 10 分钟窗） |
| Batch latency | 批处理 p50/p95/p99 曲线（µs） |

回放/复盘时同样可用（快照流天然覆盖回放期）。

---

## 7. 命令面板

### 7.1 按钮

| 按钮 | 命令 | 说明 |
|---|---|---|
| Refresh | `refresh` | 强制立即快照 + 重发 fills 历史（幂等，永远允许） |
| STOP / RESUME / PANIC | `stop`/`resume`/`panic` | 状态机控制；PANIC 后仍可撤单（风控减仓通道保持） |
| qmode on/off/close | `qmode` | **全网格**报价模式（注意影响面） |

### 7.2 Hot params（热参数）

- 白名单 6 键：`wgt_vegaflow / wgt_frontfut_skew / wgt_deltaflow /
  wgt_atmsig / wgt_rollema`（范围 [0,10]）与 `max_tps`（[1,10000]）。
- 输入框 placeholder 显示**当前值**（快照 params 回显）；回车或点 set 提交。
- `sticky_base / improve_retreat_ratio` 等**非热参数被拒绝**（init 期读取，
  热改不生效，白名单直接拦截）。
- 写入走既有热更新通道（有 `share_domain` 时写共享内存指针，否则写成员 +
  同批 `applyHotParams()` 应用），下帧快照即可见新值。

### 7.3 消息日志

ack / 错误 / 快照 / fills / 拒绝 等事件倒序滚动显示（保留 200 条）。

---

## 8. 命令协议参考（运维/调试）

浏览器→middleware 为 JSON；middleware→桥（127.0.0.1:8710）透明转发；
桥内翻译成 CommandBus 线格式由 worker 执行，ack 带回 `cmdSeq`。

| JSON | 线格式 | 说明 |
|---|---|---|
| `{"cmd":"refresh"}` | 内置 | 强制快照+fills |
| `{"cmd":"stop"}` / `"resume"` / `"panic"` | `STOP` / `RESUME` / `PANIC` | 状态机 |
| `{"cmd":"qmode","mode":"on\|off\|close"}` | `QMODE,<mode>` | 全网格 |
| `{"cmd":"qmode_code","code":...,"mode":...}` | `QMODES,<code>,<mode>` | 单码 |
| `{"cmd":"order","code":...,"side":"B\|S","price":...,"qty":...}` | `ORDER,<B\|S>,<code>,<px>,<qty>` | 手动下单（全套风控门） |
| `{"cmd":"cancel","code":...}` | `CANCEL,<code>` | 按合约撤 |
| `{"cmd":"cancel_order","id":<localid>}` | `CANCELO,<localid>` | 按单撤（双击撤单） |
| `{"cmd":"cancel_all"}` | `CANCELALL` | 全撤活单 |
| `{"cmd":"set_param","key":...,"value":...}` | `SETPARAM,<key>,<value>` | 6 键白名单 |

下行帧：`hello / snapshot / order / trade / reject / fills / ack`（均带 `seq`
或 `cmdSeq`；前端按快照 seq 水位丢弃过期事件）。

调试：服务器上 `nc 127.0.0.1 8710` 可直接发 `REFRESH` 或线格式命令。

---

## 9. 安全模型

| 层 | 机制 |
|---|---|
| 网络 | UiBridge 仅绑 127.0.0.1（硬编码拒绝非回环，除非 `allow_remote`）；公网仅暴露 middleware |
| 认证 | middleware token（cookie，7 天）；无 token 拒绝非回环绑定 |
| 写门 | 策略侧 `readonly:true`：除 refresh 外全部拒答（桥 IO 层硬门） |
| 参数 | SETPARAM 白名单 + 范围校验；未知/非热键拒答 |
| 审计 | 每条写命令记 `UI cmd: <线格式> -> <结果> (<耗时>us)`（策略日志） |
| 持仓/风控 | 下单走 `executeOrder` 全套风控门（限位/通道/TPS），UI 无旁路 |

---

## 10. 常见问题速查

| 问题 | 答案 |
|---|---|
| 某列全是 `-` | 该字段引擎暂无数据（如未定价 theo/iv）——P1 设计：缺失即 null，不编造 |
| fit 灯红 | 看到期摘要 `books x/y`：x=0 说明无买一行情（fit 门核心）；fwd chip 红说明远期未就绪 |
| age 变灰/红 | >1s 黄、>3s 红：检查策略进程与 middleware 日志 |
| 改 max_tps 不生效 | 确认 ack ok；生效于**下一个 TPS 窗口**；`share_domain` 部署走共享内存 |
| 想临时开放写命令 | 服务器配置 `params.ui.readonly:false` 并重启策略；**验证后立即恢复 true** |
| 服务进程 | 策略 `WtRunner`（dist/WtRunnerOption）；UI `start_ui.sh [start\|stop]`（~/projects/wondertrader/ui/optiontrader-ui） |
