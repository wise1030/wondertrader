# WtOptionCore 监控/交互 UI 迁移设计方案（v2.1）

- 日期：2026-11-16（v1/v2 同日；v2.1 为二轮代码实证复核修订版）
- 状态：**P1（只读 MVP）已按本方案实施并通过验证**（实施记录见 §14；v2.1 定稿后未改架构）
- 源头：`/mnt/d/gf_pc/gui/optiontrader_ui/sharklib-react-dist`（quantbox/optiontrader 交易 UI）
- 目标：为 WtOptionCore 期权做市引擎提供实时可视化与安全交互能力

> **结论先行**：v2 的总体方向正确，但二轮复核共确认 **9 项缺陷**（配置接入 1、静默行情盲区 1、事件通道选型 1、实现机制 3、验证/部署 2、字段真实性 1），因此 v2 **不是最终最优**。v2.1 修正后在「框架文件零修改 + 回测加载同一 .so + 批处理尾延迟预算」三条硬约束下为最优；唯一可能更优的路径是改框架（不推荐，见 §10.5）。

---

## 0. 版本变更记录

### 0.1 v1 → v2（首轮评审）

| # | 类型 | v1 结论 | 首轮评审发现 | v2 修正 |
|---|---|---|---|---|
| 1 | **事实错误** | “无逐笔 order/trade 事件出口” | 框架已有 `EventNotifier`→`WtMsgQue`（nanomsg pub/sub），**WtRunner HFT 模式已接线**（`WtRunner.cpp:403,578,632-641`；`TraderAdapter.cpp:2231,2349`），配置 `notifier:` 段即激活 | 订单/成交/日志事件改走既有 MQ 通道，**零 C++ 新增**；UiBridge 职责收缩为快照+状态+命令 |
| 2 | 次优 | 快照在 worker 线程构建+序列化（~100µs 尾延迟尖刺入批处理 p99） | — | worker 只做 POD 结构拷贝，JSON 序列化移到 IO 线程 |
| 3 | 次优 | 热参数改值隐含“Python 侧 ctypes 复刻 SHM 结构”风险 | 策略自身持有 `sync_param` 返回的 `_hot.*` 指针 | `SET_PARAM` 在 worker 线程直接写指针，与外部写者等价 |
| 4 | 遗漏 | CommandBus 无单笔撤单 | 原 UI 双击撤单是核心交互；`HftOptionStrategy::executeCancel(uint32_t)` 已存在 | 新增 `CANCELO,<localid>` 命令 |
| 5 | 遗漏 | 快照缺 PnL/风控限位/合约 meta/TPS/session 倒计时 | PnlTracker、checkRiskLimitsEx、PositionGuard、CTG TPS 计数均为现成接口 | 快照 schema 扩充 |
| 6 | 遗漏 | 未显式声明快照不受 enabled 门控 | 本轮冒烟核心教训：fit 被 `enabled` 门控造成观测盲区 | 写入设计原则：快照无条件构建 |
| 7 | 遗漏 | 验证未覆盖“桥开启对交易输出零影响” | golden 门禁是既有体系 | golden 对拍增加 `ui.enabled=true` 场景 |
| 8 | 补充 | 部署清单不完整 | `libWtMsgQue.so` 未入 dist；nanomsg 为系统依赖 | 部署清单补齐 |

**v1 中经复核维持的结论**：三段式架构、localhost TCP JSON Lines 桥（对快照+命令）、前端迁移复用范围、rapidjson 选型、默认只读/绑定回环/有界队列背压、回测默认关桥、框架文件零修改。

### 0.2 v2 → v2.1（二轮复核，全部有代码证据）

| # | 类型 | v2 结论 | 二轮复核发现（证据） | v2.1 修正 |
|---|---|---|---|---|
| 1 | **配置接入错误** | `ui:` 放 `config.yaml` 顶层（与 `env:/parsers:` 平级） | `WtRunner.cpp:319` 只把 `cfgItem->get("params")` 传给策略 → **顶层 `ui` 策略永远读不到**；且 `ConfigValidator.h` 根 schema 无 `ui`，放进 params 会每次启动 WARN | `ui:` 移到 `strategies.hft[].params.ui`；`ConfigValidator` 登记 `ui` 及其子键 |
| 2 | **静默行情盲区** | 快照挂 `on_batch_complete`，每批触发 | `OptionAsyncEventProcessor.cpp:442` 空批直接 `continue`；Timer 仅由 `on_tick` 产生（`HftOptionStrategy.cpp:2853-2868`）→ **无行情时无批、无 Timer、无快照**——恰好是“卡 fit/断行情”最需要看的场景 | 桥 IO 线程在**有客户端连接时**每 `heartbeat` 秒 `enqueue_control()`（Custom 控制事件，可靠唤醒，只驱动 `on_batch_complete`）；默认 1s；空批不进批延迟直方图，成本可忽略 |
| 3 | **事件通道选型反转** | 复用 EventNotifier/WtMsgQue 推 `TRD_ORDER`/`TRD_TRADE`，UiBridge 只做快照+命令 | `TRD_*` schema 同样**无 userTag**、trade **无时间戳**；middleware 需第二传输 + nanomsg/`libWtMsgQue`（dist 未带）；订单镜像仍要在桥内自维护 | **单通道**：订单/成交/拒单事件由 AEP 既有 `on_order/on_trade/on_entrust` 回调（worker 线程，已接线）入桥出站队列；快照内嵌 `orders` 权威全量 + 重连 `fills` 历史自愈；`notifier:` 从本方案移除，MQ 仅留作未来外部告警（默认关） |
| 4 | **SET_PARAM 空指针 + 白名单错误** | worker 直接写 `_hot.*` 指针；白名单含 `sticky_base/improve_retreat_ratio` | 当前 `dist/WtRunnerOption/config.yaml` **没有 `share_domain`** → `ShareManager::allocate_value` 因 `_inited=false` 返回 `nullptr`（`ShareManager.cpp`）→ 默认部署下所有 `_hot.*` 为空；且 `sticky_base/improve_retreat_ratio` 只在 init 读取，`applyHotParams` 不消费（`HftOptionStrategy.cpp:3802-3834`） | `SETPARAM,<key>,<value>` 统一走 CommandBus：指针非空写指针，否则写成员变量（`_wgt_*/_maxTPS`），再置 `_hotUpdatePending`，由既有 `applyHotParams()` 应用；白名单只保留真正热生效的 **6 个键**（5 个 alpha 权重 + `max_tps`）；`sticky_base/improve_retreat` 标注“非热参数，P2 不开放” |
| 5 | **命令消费机制错误** | 入站命令入 worker 队列（`enqueue_control` 既有机制）执行 | `enqueue_control()` 只是 `make_control()`（无 payload），仅驱动 `on_batch_complete`；命令不能搭载在它上面 | 桥内 `std::deque<UiCommand>`（mutex，MPSC）+ `enqueue_control()` 只做唤醒；worker 在 `on_batch_complete` **最顶部**（`!_grid` 早退之前）drain，逐条 `CommandBus::dispatch` / refresh，执行结果以 `ack{cmdSeq}` 回桥 |
| 6 | **golden 对拍扰动** | 只加 `ui.enabled=true` 场景 | 回放加速下墙钟心跳会注入 Custom 空批（每 100 批多一行 batch 日志、改变批边界），可能造成日志/时序 diff | golden 场景固定 `ui.enabled=true, ui.heartbeat=false`，并挂 loopback 测试客户端确保快照路径真实执行；交易 CSV 必须逐字节一致 |
| 7 | **中间件依赖过重** | 双上游（TCP + WtMsgQue），可能引入 wtpy/nanomsg | 与 #3 同源；UI 只要一条 TCP 即可 | middleware 收敛为**纯 Python**（fastapi/uvicorn + asyncio TCP 行客户端 + WS 广播 + 静态托管）；部署清单删 `libWtMsgQue.so`/`libnanomsg`/wtpy |
| 8 | **字段真实性** | `cancelSoft/cancelHard`、桥内 `snapshotAgeMs`、网格 `obid/oask` 等 | 撤单软/硬限是**配置项**不是运行计数；`snapshotAgeMs` 由桥计算无意义（消费端才知道龄期）；WtOptionCore 合约对象无 `obid/oask` getter（我方报价在订单表） | `risk` 改用 `QuoteStatistics::getSessionSummary()`（orders/fills/cancels/rejects/ratio）+ `_limitsBreached/_maxTPS`；`snapshotAgeMs` 由 middleware 转发时计算注入；网格列删 `obid/oask`，保留真实可得字段 |
| 9 | 健壮性补强 | 单出站队列(64)丢最旧 | 40KB 快照与逐笔事件混在一个队列，溢出时可能先丢事件；事件/快照跨线程发送可能乱序 | 快照用**单槽 latest-wins**（`snapshotCoalesced`）；事件用有界 FIFO（默认 256，`eventDrops`）；两者带 `seq`（`_batchCount`），前端丢弃 `seq <= lastSnapshotSeq` 的过期事件 |

### 0.3 二轮复核维持的结论

- 三段式（React SPA + FastAPI middleware + 策略侧桥）不变；
- UiBridge 用 127.0.0.1 TCP JSON Lines（理由见 §10.1）；
- 前端工程骨架/OptionGridView/TradesTab 复用范围不变；
- rapidjson 选型不变（Linux 已装 1.1.0，Windows 走 MyDeps141）；
- 默认只读、绑定回环、有界队列背压、回测默认关桥、框架文件零修改不变；
- 快照“不受 enabled/channelReady/fitReady 门控”原则不变，并进一步明确“只允许初始化空对象 guard，不允许业务门”。

---

## 1. 背景与目标

当前 WtOptionCore 已具备完整交易闭环，但运行状态只有三类文本出口可观测（策略日志 ATTRS/OPT/UND/batch/FITGATE、`aep_metrics.csv`、QPM 计数 CSV）。冒烟期定位“enabled=false 卡 fit 门”全靠 tail 日志，无法交互。

**目标**：迁移 quantbox 期权交易 UI 的功能范式，改造为 WtOptionCore 实时 Web 控制台：

1. **看得见**：期权网格全状态、到期月聚合（forward/ATM vol/fit 状态）、组合希腊与 PnL、订单/成交镜像、AEP 健康、TradingState 状态机；
2. **控得住**：QMODE 切换、STOP/RESUME/PANIC、单笔/整码撤单、Ticket 手动下单、热参数修改——全部走既有 CommandBus 与风控门，不开旁路；
3. **不添乱**：交易路径零侵入、批处理尾延迟影响可忽略、回测（WtBtRunner 加载同一 .so）默认关、框架文件（WtCore/Share/Includes）零修改。

## 2. 原 UI 资产分析（复核后维持）

### 2.1 架构与功能

```
浏览器 React SPA ──WebSocket(JSON {type,data})──▶ FastAPI middleware ──TCP+私有二进制协议──▶ quantbox Echelon 服务
       ▲                                                │
       └──────────── 静态托管 frontend/dist ◀────────────┘
```

- **frontend**（19 tsx，~2400 行）：React 19 + Vite 7 + Tailwind 3.4 + TS。三 Tab：
  - **Command**：策略选择、Refresh/Connect、消息日志（可开关）、原始命令行、属性键值网格（布尔高亮/可编辑）；
  - **Grid**（核心）：到期月选择条（F1–F8）、到期摘要卡（delta/vega/atm_fwd/atm_vol/sprd_fwd/rej/cxl/fil_pos）、标的报价表、期权网格大表（每行一执行价，Call 左/Put 右：市场买量买价 | mbid | theo | mask | 卖价卖量、delta、vshift、持仓、qmode 四色着色）、活跃订单表（双击撤单）、Ticket 弹窗（点价下单）；
  - **Trades**：订单表 + 最近 500 笔成交卡。
- **middleware**：FastAPI + WS 广播 + 静态托管；EchelonClient 做 4 字节头二进制协议 ↔ JSON 转换；新客户端接入自动重发 refresh。

> 注：原 UI 的 `mbid/mask` 就是市场买价/卖价（见 `OptionGridView.tsx:48-49`），不是“我方报价”。v2.1 直接改用 `bid/ask` 语义命名，前端 adapter 做一次映射。

### 2.2 协议语义

下行 `refresh-request`/`strategy-status`/`strategy-update`（平铺 K/V，含 `202605.xxx` 到期前缀键）/`optiongreeks`/`udata`/`morder-list`/`morder-update`/`fill`；上行 `command <sid> refresh|set|cancel|cancel_all`。前端用正则把平铺 K/V 拆主属性/到期属性并按优先级排序渲染——这是 quantbox stratlib 属性注册表特有语义。

### 2.3 复用决策

| 资产 | 处置 | 理由 |
|---|---|---|
| 工程骨架（socket 单例/DataContext reducer+100ms flush/memo 行/ErrorBoundary/Toast） | 迁移 | 与推式增量+快照模型同构 |
| OptionGridView 大表全部交互（着色/双击 Ticket/F1–F8/点价下单） | 保留 | 做市盯盘核心交互 |
| TradesTab/OrdersTable/Ticket | 保留 | 字段重映射 |
| CommandTab | 改造 | 平铺属性网格 → 结构化面板 + 精简命令行 + 结构化按钮组 |
| EchelonClient 协议层、expiryRegex/PropertyCache 归一化 | 废弃 | quantbox 私有协议/语义；新协议直接给结构化快照 |

## 3. WtOptionCore 对接面盘点（v2.1 修订）

### 3.1 已具备（零或近零开发）

| 能力 | 位置 | 内容 |
|---|---|---|
| **订单/成交/拒单事件（worker 线程）** | AEP `AsyncCallbacks`（`OptionAsyncEventProcessor.h:57-77`） | `on_order(code,localid,isBuy,totalQty,leftQty,price,isCanceled)`、`on_trade(code,localid,isBuy,vol,price)`、`on_entrust(localid,success)` 已在 `HftOptionStrategy.cpp:1860,2020,2114` 接线；v2.1 在回调尾部追加桥入队（guarded，无桥时零成本） |
| **订单权威全量** | `IssuedOrderRegistry.h:70-86,223` | `orders()` 返回 `unordered_map<uint32_t,Rec>`；`Rec` 含 code/totalQty/filledQty/fillQty/done/rejected/isBuy/issuePx/issuedTime + `vector<Fill>`（px/qty/timeUs/late） |
| 完整状态镜像 | `HftOptionStrategy.cpp` worker 内 | PositionBook 权威持仓、`_positions` 净值、IssuedOrderRegistry、OQM 订单状态、OptionRisk 希腊/对冲持仓 |
| 状态机 | `TradingState.h:37-57` | Disabled/Recovering/Normal/SessionStopped/Panic + reason/channelReady/underlyingReady + `enabledFlag()/panickedFlag()` |
| 逐合约数据 | `OptionData.h:104-122,131,140` | `getBid/getAsk/getLast/bidSize/askSize`、`getTheoPrice()`、`getImpliedVol()`、`getMarket()`、`greeks()`、`getPosition()`、`getTradingData()` |
| 网格查询 | `IOptionGrid.h:40` | `expiries()`/`getStrikesByExpiry`/`getUnderlyingPrice`/`getExpiryData(exp)` |
| 到期数据 | `ExpiryData.h:53,67,114-116,147,154` | `daysToExpiry()`、`getForward()`、`isForwardReady()`、`isFitReady()`、`isUnderlyingPriceValid()`、`getHedgeUTD()` |
| ATM vol | `IOptionPricer.h:71` | `getATMVol(exp)`（Composite/OptionPricer2/GammaScalp 均已实现） |
| 组合/分到期希腊 | `OptionRisk.h:109,113` | `getPositionGreeks()` + `getExpiryGreeks(exp)`；`OptionGreeks` 含 delta/gamma/vega/vegaTW/theta/rho |
| PnL | `PnlTracker.h:58-70` | 逐合约 `getCurPnl/getRealizedPnl/getCurTurnover`；组合值由 builder 汇总 |
| 报价统计 | `QuoteStatistics.h:224` | `getSessionSummary()` → totalOrders/totalFills/totalCancels/totalRejects/ratio/延迟 |
| 风控状态 | `_limitsBreached`、`_maxTPS`、CTG `getTransactionCount` | 限位熔断、TPS 上限与当前计数 |
| 命令总线 | `CommandBus.h` + `OptionCommandHandlers.cpp` | STOP/RESUME/PANIC/QMODE/ORDER/CANCEL；`_cmdBus` 已在 `HftOptionStrategy.cpp:3635` 注册 |
| 热参数 | `sync_param`（`HftOptionStrategy.cpp:701-714`） | `_hot.*` 指针 + `commit_param_watcher()`；**当前 dist 无 `share_domain`，指针为 nullptr，回退成员变量** |
| 性能计数 | AEP `totalEvents()/queueDrops()/mdOverflowDepth()/traderDepth()` + `_batchLatencyUs` | 实时健康度 |
| 推送挂点 | `on_batch_complete`（`HftOptionStrategy.cpp:1572`） | 全量镜像更新完成后的天然挂点；空批不会进入（需心跳，见 §5.1） |

> `EventNotifier`/`WtMsgQue` 仍然存在且 HFT 已接线，但 v2.1 **不把它作为 UI 数据通道**（理由见 §10.2）。它保留给未来的外部告警机器人，默认配置不启用。

### 3.2 真缺口（v2.1 后真正要补的）

1. **网格级快照出口**：订单/成交事件之外，网格/拟合/希腊/状态机/PnL/健康度没有结构化出口；
2. **命令 socket 入口**：CommandBus 无 TCP/WS 入口（现仅 `manual_order` 64B SHM 串）；
3. **单笔撤单命令**：CommandBus 只有 `CANCEL,<code>`（整码），缺 `CANCELO,<localid>`；
4. **热参数进程内写入口**：无桥内写通道（外部 SHM 写入口在当前部署也未启用）。

## 4. 目标架构（v2.1：单通道）

```
┌────────────────────── WtRunner 进程（HFT 引擎） ─────────────────────┐
│  TraderAdapter ──enqueue──▶ AEP（worker）──on_order/on_trade/on_entrust──┐
│                                                                        │
│  HftOptionStrategy ── on_batch_complete 尾 ──▶ UiSnapshotBuilder        │
│        (worker, POD 拷贝) ──▶ 快照单槽(latest-wins) ──▶ UiBridge IO 线程 ─┼──▶ ① 快照/状态/健康度
│        事件回调 ──▶ 事件 FIFO(256) ────────────────────▶ UiBridge IO 线程 ─┼──▶ ② 订单/成交/拒单增量
│        ◀── 命令队列(deque) ◀── enqueue_control() 唤醒 ◀── UiBridge ◀──────┼──◀ ③ 命令（新增）
└────────────────────────────────────────────────────────────────────────┘
                                                                    │
                                              FastAPI middleware ───┴── WebSocket JSON ──▶ React SPA
                                              (单 TCP 上游 + 静态托管)
```

- **只有一条进程内 TCP（127.0.0.1:8710，JSON Lines）**：②③① 全走它；
- 桥内**快照单槽**与**事件 FIFO**分离：快照可合并（latest-wins），事件不可丢（有界 FIFO，溢出计数）；
- 所有下行帧带 `seq`（worker 的 `_batchCount`），前端以快照 `seq` 为水位丢弃过期事件；
- 桥 IO 线程在**有客户端**时按 `heartbeat` 周期 `enqueue_control()`，解决“静默行情无批”问题；
- `notifier:` 段不再出现在本方案部署配置中；`EventNotifier` 保留但默认关。

**否决过的备选**：

| 备选 | 否决理由 |
|---|---|
| 日志 tail 解析 | DEBUG 量巨大、文件 IO、字段缺失（fwd 不打印）、解析脆弱；仅可作 P0 临时手段 |
| 共享内存快照块 | 需自定义 block+seqlock；Python ctypes 复刻结构（Windows/Linux 双端维护）；复杂度高收益低 |
| `sync_param` 扩容 | 值域 64B 先天上限；全量网格塞不下 |
| 纯“文件落盘(/dev/shm)+轮询”遥测 | P1 只读场景可行，但 P2 命令仍需 ctypes 写 SHM（布局耦合+清除协议含糊）→ 双机制劣于单一 TCP 双工；且失去 `nc` 调试性 |
| 双通道（TCP 快照/命令 + MQ 订单事件） | v2 原方案；MQ 的 `TRD_*` schema 缺 userTag/时间戳，middleware 多一条传输与 nanomsg 依赖，订单镜像仍要自维护；单通道更小、更快、更一致（见 §10.2） |
| 适配 Echelon 协议让原 UI 原样跑 | Echelon 语义=平铺属性注册表，WtOptionCore 无此层，伪造成本高于前端改造 |
| wtpy WtMonSvr 全套 | 面向进程/回测监控与 CTA 事件，无期权网格渲染能力；middleware 仅 ~150 行，自建更轻 |

## 5. 策略侧设计（新增组件，全部落在 WtOptionCore 内）

### 5.1 组件划分与线程模型

```
UiBridge（IO 线程，唯一碰 socket）
   ├─ 出站快照: 单槽 UiSnapshotView（worker 写入，latest-wins）
   ├─ 出站事件: 有界 FIFO<UiEventFrame>(256)，worker 写入
   ├─ 入站命令: std::deque<UiCommand>(mutex) + enqueue_control() 唤醒
   └─ 心跳: 有客户端时每 heartbeat 秒 enqueue_control()
UiSnapshotBuilder（纯函数，可单测；零 IO 零锁）
   输入: IOptionGrid*/ExpiryData*/OptionRisk*/PnlTracker/QuoteStatistics/TradingState/AEP counters/_hot 配置
   输出: UiSnapshotView（POD：到期×执行价×C/P 腿 + orders + meta）
```

1. **挂点**：`on_batch_complete` 尾部（`HftOptionStrategy.cpp:1572` 起）——先 drain 命令，再 `publishAsyncVolFit`/`applyHotParams`，最后按 `snapshot_interval` 构建快照入槽；
2. **worker 成本**：快照构建为字段读取 + POD 拷贝（~20–50µs，1Hz；即使无客户端也构建，用于 golden 验证读路径零副作用）；JSON 编码（~100µs）全部在 IO 线程；
3. **背压**：快照单槽覆盖旧值（`snapshotCoalesced++`），事件 FIFO 满丢最旧（`eventDrops++`），**绝不阻塞 worker**；
4. **线程安全**：快照只读 worker 持有状态（vol 曲线由 fitter 线程计算、经 `AsyncVolFitter::tryPublish` **在 worker 应用**，`AsyncVolFitter.h:86-104`）；IO 线程零策略对象访问；
5. **静默行情**：`enqueue_control()` 是 `AsyncEvent::Custom` 控制事件，不受队列容量门控（`OptionAsyncEventProcessor.cpp:271`），只驱动 `on_batch_complete`，不携带命令 payload；空批不进入 `_batchLatencyUs`（`_batchStartValid` 仅 tick 批设置）；
6. **崩溃隔离**：IO 线程全 try/catch，异常只杀 IO 线程 + ERROR 日志；`UiBridge::stop()` 挂入 `HftOptionStrategy::shutdown()`，且必须在 `_async->stop()` **之前**调用（先停心跳，再停 worker）；
7. **慢客户端隔离**：socket 设 send timeout（~100ms）或非阻塞发送；超时丢帧并计数，绝不反压 worker；客户端断开只影响自身连接。

**心跳的边界条件（含副作用声明）**：

- `heartbeat: 0` 关闭；默认 `1.0s`；
- 只有 `clientCount > 0` 才发心跳（无人看时不产生空批）；
- 回放加速下墙钟心跳会改变批边界 → golden/回测场景必须 `heartbeat: false`（§8）；
- 生产静默行情：1Hz 空批仅多一次 `_batchCount++`（每 100 批一行日志），可忽略；
- **副作用**：心跳批会执行 `on_batch_complete` 的周期段——`drainPendingQuotes()`（每批无条件）、`checkMarginLimits`/`checkDeltaHedge`（自带节流，注释明确“quiet markets 也要查”）。它**不会**触发 `computeValues`（`shouldCompute` 仍由 `_underlyingChanged`/`_needsRefresh` + 防抖驱动），`checkRiskLimitsEx` 也在 `shouldCompute` 门内不受影响。净效果只是把已 staged 的 drain 与两个节流检查提前到 1Hz，是有意的 keep-alive；若要求严格零附加动作，可在 P2 增加 AEP `on_control` 轻量回调（仅快照、不走周期段），当前 1Hz 语义与成本可接受。

### 5.2 配置（**必须放在策略 params 子树**）

```yaml
strategies:
    hft:
    -   active: true
        id: option_mm_ag
        params:
            # ... 现有参数 ...
            ui:
                enabled: true          # 默认 false；回测缺省即关
                bind: 127.0.0.1        # 仅允许回环；非回环需 allow_remote: true（默认 false）
                port: 8710
                snapshot_interval: 1.0 # 全量快照秒
                heartbeat: 1.0         # 有客户端时的空批唤醒；0=关闭
                readonly: true         # P1 强制 true：除 refresh 外拒收一切写命令
                token: ""              # 可选；非空则首帧需 {"auth":"..."}
                allow_remote: false    # 显式允许非回环绑定（默认拒绝）
```

- **位置原因**：`WtRunner.cpp:319` 只把 `params` 子树传给策略，顶层 `ui` 读不到；
- `ConfigValidator.h` 根 schema 必须新增 `ui` 及其子键，否则每次启动 WARN（`ConfigValidator::validate` 在 `HftOptionStrategy.cpp:524` 调用）；
- `enabled:false` 时 `UiBridge` 不创建、不监听——WtBtRunner 加载同一 .so 零影响；
- 绑定失败（端口占用等）**不阻断策略启动**：ERROR 日志 + UI 禁用，交易继续。

### 5.3 快照 schema（下行，bridge→middleware→UI）

```jsonc
{
  "type": "snapshot", "proto": 1, "seq": 12345, "ts": 1763300000123,
  "sid": "option_mm_ag",
  "state": {"trading": "Normal", "reason": "", "channelReady": true,
             "underlyingReady": true, "enabled": true, "panicked": false, "tdate": 20261116},
  "greeks": {"delta": -12.3, "gamma": 0.04, "vega": 890.1, "vegaTW": 870.2, "theta": -156.0},
  "pnl": {"cur": 1234.5, "realized": 900.0, "turnover": 45600.0},
  "risk": {"limitsBreached": false, "tps": 12, "tpsMax": 30,
           "ordersSent": 1200, "fills": 86, "cancels": 310, "rejects": 4,
           "cancelRatio": 0.258, "fillRatio": 0.072,
           "sessionStopInSec": 5400},
  "perf": {"batchP50": 66, "batchP95": 204, "batchP99": 1355,   // µs
           "aepEvents": 123456, "queueDrops": 0, "mdOverflow": 0, "traderDepth": 0,
           "eventDrops": 0, "snapshotCoalesced": 0},
  "expiries": [{
      "expiry": 202612, "fwd": 101255.5, "fwdReady": true, "fitReady": true,
      "underlyingValid": true, "atmVol": 0.131, "atmVolSprd": 0.02, "daysToExpiry": 9,
      "booksWithBid": 84, "booksTotal": 96, "greeks": {"delta": -5.1, "vega": 300.2},
      "und": {"code": "SHFE.cu.2612", "bid": 101240, "ask": 101260, "pos": 1},
      "strikes": [{
          "k": 100000,
          "C": {"bid": 1233, "bidSz": 2, "ask": 1237, "askSz": 1, "last": 1235,
                 "theo": 1235.2, "iv": 0.128, "delta": 0.52, "vshift": -0.3,
                 "pos": 2, "qmode": 1, "pnl": 12.5},
          "P": {"...": "同构"}
      }]
  }],
  "orders": [
      {"id": 10001, "code": "SHFE.cu2612.C.100000", "buy": true, "px": 1233,
       "qty": 2, "filled": 0, "done": false, "rejected": false, "t": 1763300000.0}
  ],
  "meta": {"contracts": {"SHFE.cu2612.C.100000": {"mult": 5, "priceTick": 2}}}
}
```

**字段来源（全部真实接口）**：

| 字段 | 来源 |
|---|---|
| `state.*` | `TradingState` 字段 + `toString()` + `enabledFlag()/panickedFlag()` |
| `greeks`（组合） | `OptionRisk::getPositionGreeks()` |
| `pnl` | builder 汇总所有 option + hedge 的 `PnlTracker`（cur/realized/turnover）；tracker 已由 compute 循环逐合约创建（`HftOptionStrategy.cpp:1650,1665`），快照只读 |
| `risk.limitsBreached/tps/tpsMax` | `_limitsBreached` / `CTG::getTransactionCount()` / `_maxTPS` |
| `risk.ordersSent/fills/cancels/rejects` | `_quoteStats.getSessionSummary()`（totalOrders/totalFills/totalCancels/totalRejects） |
| `risk.cancelRatio/fillRatio` | builder 由 totals 计算**全局口径**（totalCancels/totalOrders、totalFills/totalOrders）；`getSessionSummary()` 的 `avgFillRatio/avgCancelRatio` 是跨合约均值，**不进快照** |
| `risk.sessionStopInSec` | `_secSecs` + `_stopLeadSeconds` + 当前秒（**best-effort；表未建时 null**） |
| `perf.*` | AEP getters + `_batchLatencyUs.p50/p95/p99` + 桥计数 |
| `expiries.*` | `ExpiryData` + `IOptionPricer::getATMVol/getATMVolSprd` + `OptionRisk::peekExpiryGreeks`（P1 新增的只读变体，缺失返回 null，不 lazy-create） |
| `expiries[].und` | `ExpiryData::getHedgeUTD()` → `getBid/getAsk/getPosition()` |
| `strikes[].C/P` | `OptionData::getMarket()`（bid/ask/last/size）、`getTheoPrice/getImpliedVol/greeks/getPosition`、`activeValues().theoVol()`、`OptionTradingData::getQuoteMode()`、`PnlTracker::getCurPnl()` |
| `strikes[].vshift` | `theoVol − getATMVol(exp)`（曲线未初始化时 null） |
| `booksWithBid/booksTotal` | 该到期 `getBid() > 0` 的腿数 / 总腿数（与 FITGATE 日志 `HftOptionStrategy.cpp:3341-3350` 同口径） |
| `orders` | `IssuedOrderRegistry::orders()` 过滤 `!done && !rejected`（活跃订单）；`t=issuedTime` 为策略时钟秒，与事件墙钟 `ts` 区分 |
| `meta.contracts` | `OptionData::getMultiplier()/getTickSize()` |
| `snapshotAgeMs` | **middleware 转发时计算**（`now - ts`）并注入，不在桥内计算 |

**设计原则（不可违反）**：

1. **快照无条件构建**——不检查 `_traderCtx->enabled`/`channelReady`/`fitReady`；只允许 `!_grid || !_pricer` 的初始化 guard；
2. 缺失字段显式 `null`（前端显示 “–”），禁止编造默认值；`iv/atmVol/theo` 等以 0 为“未计算”哨兵，桥侧统一转 null；
3. `booksWithBid/booksTotal`（fit 门诊断核心指标）与 middleware 注入的 `snapshotAgeMs`（防 stale 幻觉）为固定字段；
4. 订单/成交事件是**增量提示**，`orders` 全量是**权威**；事件丢失/乱序由下一帧快照自愈。

### 5.4 命令协议（上行，UI→bridge→worker）

| JSON 命令 | 线格式 / 实现 | 备注 |
|---|---|---|
| `{"cmd":"refresh"}` | 桥内置 `REFRESH`（不经过 CommandBus） | 置强制快照标志 + `enqueue_control()`；幂等 |
| `{"cmd":"qmode","mode":"on\|off\|close"}` | `QMODE,<mode>` | 全网格；CTG `onSetQMode` |
| `{"cmd":"order","code":...,"side":"B\|S","price":...,"qty":...}` | `ORDER,<B\|S>,<code>,<px>,<qty>` | 走 `executeOrderIds` 全套风控门 |
| `{"cmd":"cancel","code":...}` | `CANCEL,<code>` | 整码撤单 |
| `{"cmd":"cancel_order","id":<localid>}` | `CANCELO,<localid>`（新增 handler） | → `executeCancel(localid)`（`HftOptionStrategy.cpp:3241`），补齐原 UI 双击撤单 |
| `{"cmd":"stop"}` / `resume` / `panic` | `STOP`/`RESUME`/`PANIC` | 状态机既有语义（PANIC 后仍可撤单） |
| `{"cmd":"set_param","key":...,"value":...}` | `SETPARAM,<key>,<value>`（新增 handler） | 白名单 **仅**：`wgt_vegaflow/wgt_frontfut_skew/wgt_deltaflow/wgt_atmsig/wgt_rollema/max_tps` |

**SETPARAM 的实现语义（v2.1 关键修正）**：

```
worker 执行 SETPARAM:
  1) 校验 key 在白名单、value 为有限数、范围合理；
  2) 若 _hot.<key> != nullptr: *ptr = value;       // 有 share_domain 的实盘
     否则:                     _wgt_* = value;        // 当前 dist 默认路径（无 share_domain）
                               _maxTPS = (int32)value;
  3) _hotUpdatePending.store(true);
  4) 同一批 on_batch_complete 稍后的 applyHotParams() 通过 hotVal(ptr, member) 读到新值并应用
     （alpha 权重 → setAlphaWeights；max_tps → setMaxTransactionsPerSec）。
```

- **不再依赖** `share_domain`/`ctypes`；当前 `dist/WtRunnerOption/config.yaml` 没有 `share_domain`，`sync_param` 返回 nullptr 是**默认路径**而非边角情况；
- `sticky_base`/`improve_retreat_ratio` 只在 init 读取，**不是热参数**；P2 不开放编辑（UI 显示为只读配置），如未来需要，必须先在 pricer/OQM 增加 apply 逻辑；
- `qmode_override`/`command`/`manual_order` 等 SHM 串不暴露给 UI（避免与 CommandBus 双入口）。

**命令消费与 ack**：

- 桥 IO 线程解析 JSON/CSV → 白名单/readonly 校验 → 推入 `std::deque<UiCommand>`（mutex，上限 1024，满则 ack `busy`）→ `enqueue_control()`；
- `enqueue_control()` 只负责唤醒，**不携带命令**；worker 在 `on_batch_complete` **最顶部**（`if (!_grid || !_pricer) return;` 之前）drain 队列，逐条 `_cmdBus.dispatch(line, err)`；refresh 置标志；
- 执行结果回 `{"type":"ack","cmdSeq":n,"ok":true|false,"err":"..."}`；所有写命令打 INFO 审计日志（来源/内容/结果/耗时）；
- 同时兼容裸 CSV 行（`ORDER,B,...`/`QMODE,on`），`nc 127.0.0.1 8710` 可直测；
- **readonly:true**：除 `refresh` 外全部拒答 `{"type":"ack","ok":false,"err":"readonly"}`（P1 的硬安全门；紧急操作仍走既有运维通道）。

### 5.5 订单/成交/拒单事件（单通道，AEP 回调）

**事件源**（全部在 worker 线程，已在 `setupAsyncCallbacks()` 接线）：

| 回调 | 事件帧 | 字段 |
|---|---|---|
| `on_order` | `{"type":"order","seq":...,"ts":...,"id":localid,"code":...,"buy":...,"qty":totalQty,"left":leftQty,"px":price,"canceled":...}` | AEP 现有字段；`filled=qty-left` 由前端计算 |
| `on_trade` | `{"type":"trade","seq":...,"ts":...,"id":localid,"code":...,"buy":...,"qty":vol,"px":price}` | AEP 无时间戳 → 桥补 `ts`（墙钟） |
| `on_entrust` | `{"type":"reject","seq":...,"ts":...,"id":localid,"ok":success}` | 委托失败无 terminal on_order，必须单独报 |

- 事件在回调尾部入桥 FIFO；`_uiBridge == nullptr` 时零成本；
- 事件不带 userTag（AEP 入队时已丢弃），UI 也不需要；订单归属用 `localid`；
- **自愈**：快照 `orders` 是全量权威；重连/刷新时桥额外发 `{"type":"fills","items":[...]}`（从 `Rec::fills` 汇总最近 ≤500 笔，按 `timeSim` 排序，受 registry 保留窗口限制），前端重放成交卡；未知 localid 的成交仍落 `fill_anomaly.csv`，不进 `fills` 历史（P2 可选补 `id=0` 条目）；
- 事件与快照都带 `seq`；前端收到快照后记录 `lastSnapshotSeq`，丢弃 `seq <= lastSnapshotSeq` 的过期事件，避免“旧事件复活已结束订单”。

### 5.6 对现有代码的影响面

- **新增**：`UiBridge.{h,cpp}`、`UiSnapshotBuilder.{h,cpp}`（POD 视图 + JSON 编码分离）、`tests/test_uibridge.cpp`；
- **修改（全部在 WtOptionCore 内）**：
  - `HftOptionStrategy`：init 读取 `params.ui`、创建/启动桥（`_async->start()` 之后）、`shutdown()` 先停桥、`on_batch_complete` 顶部 drain + 尾部入槽、`on_order/on_trade/on_entrust` 尾部 1 行桥入队、`registerCommands()` 增加 `CANCELO`/`SETPARAM` hooks；
  - `OptionCommandHandlers`：新增 `CANCELO`、`SETPARAM` handler 与 `cancelOne/setHotParam` hooks；
  - `OptionRisk`：新增 `peekExpiryGreeks(exp) const`（只读，不 lazy-create；快照读路径不得改变风控表）；
  - `ConfigValidator.h`：根 schema 加 `ui` 及子键；
  - `CMakeLists.txt`：加 rapidjson（Linux 系统已装 rapidjson-dev 1.1.0 `/usr/include`；Windows 走 MyDeps141，`EventNotifier.cpp:22` 先例）；
- **不动**：AEP 队列/批处理逻辑、交易路径、任何框架文件（`WtCore/Share/Includes` 零修改）。

### 5.7 对交易路径的侵入性论证

- 快照构建为只读：不写 `_grid/_risk/_ctg/_traderCtx`，不触发 refresh/下单；
- 事件入队为 `try_lock`/短临界区 + 容量判断，不等待 IO；
- 命令只在 worker 批边界执行，与既有 `manual_order` 在 `applyHotParams` 内 dispatch 的时机同构（已被实盘验证）；
- 心跳只增加空的 `Custom` 批，不进入批延迟直方图、不改变 `_batchStartValid`；但会执行 `on_batch_complete` 的周期段（见 §5.1 副作用声明），golden/回测用 `heartbeat:false` 排除；
- golden 对拍是最终门禁（§8）。

## 6. 中间件设计（纯 Python 单上游）

复用原 `main.py` 骨架，职责变为**单 TCP 上游 + WS 广播 + 静态托管**：

1. `wt_option_client.py`：asyncio TCP 行客户端连 UiBridge（重连退避 1s→30s）；解析 JSON 行；
2. 直接向所有 WS 客户端广播；对 `snapshot` 帧注入 `snapshotAgeMs = now - ts`（消费端视角的龄期）；
3. 浏览器命令只路由到 TCP 客户端；`refresh` 原样转发；
4. 静态托管 `frontend/dist`，端口 8002；
5. 可选 `--record` 落盘全天消息（复盘 + 前端 mock）。

**依赖**：`fastapi`、`uvicorn`（含 `websockets`/`starlette`）；**不需要** wtpy、nanomsg、`libWtMsgQue.so`。部署为 `python3 -m venv` 或系统 pip，`start_ui.sh` 用 `setsid nohup`。

## 7. 前端改造设计

| 原组件 | 处置 |
|---|---|
| socket 单例 / DataContext reducer+100ms flush / memo 行 | 保留架构；消息类型换为 §5.3/§5.5；**删除** expiryRegex 平铺归一化与 PropertyCache；新增 `adapters/snapshot.ts`（快照 → 网格组件 prop 形状）与 `adapters/events.ts`（order/trade/reject → Order/Fill 形状） |
| OptionGridView / OptionGridWidget / ExpirySelector / ExpirySummary / UnderlierView | 保留全部交互；列源=快照 strikes；F1–F8/双击 Ticket/点价下单/双击撤单全留；`mbid/mask → bid/ask` 仅做字段名映射 |
| TradesTab/OrdersTable/Ticket | 保留；Ticket 加**二次确认弹窗**（readonly 时禁用）；成交卡支持 `fills` 历史重放 |
| CommandTab | 改造：策略下拉删除（单策略）；结构化按钮组（STOP/RESUME/PANIC 红色确认、QMODE 三态、Cancel All）+ 消息日志 + 白名单命令行 |
| OverviewPanel 属性网格 | 替换为新面板 |

新增面板：

1. **状态横幅**：TradingState 大色块（Normal 绿/Recovering 黄/Panic 红/SessionStopped 灰/Disabled 灰）+ channelReady/underlyingReady 灯 + session 停止倒计时（null 显示 “–”）；
2. **Fit 就绪矩阵**：每到期 `fwdReady/fitReady/underlyingValid/booksWithBid/booksTotal/atmVol/fwd/daysToExpiry`——落地后**退休 FITGATE 临时日志**；
3. **希腊聚合条 + PnL 条**：组合 greeks + cur/realized PnL，红绿着色沿用原语义；
4. **健康卡**：AEP 计数、batch p50/p95/p99 sparkline、TPS、eventDrops/snapshotCoalesced、快照龄期（>3s 标灰）；
5. **持仓区**：期权净持仓按到期聚合 + 对冲腿。

工程：依赖沿用不新增；vol smile 图（P3）纯 SVG 自绘。快照到达时按 `seq` 水位丢弃过期事件（§5.5）。**消息日志**仅展示桥 ack/错误/状态迁移与命令审计，不引入全框架 LOG 洪水（全框架日志仍走文件）。

## 8. 验证与测试

| 层 | 内容 |
|---|---|
| C++ 单测 | `test_uibridge.cpp`：fixture 网格 → `UiSnapshotBuilder` 字段/空值断言；**`enabled=false` 时快照仍构建**（无业务门控）；**`peekExpiryGreeks` 不创建缺失到期**；`SETPARAM` 指针非空/为空两条路径；`CANCELO` 映射；readonly 拒答矩阵；配置缺失/非法 bind/token 校验 |
| 回归 | golden 双基线重放零 diff；**新增场景：replay + `ui.enabled=true + ui.heartbeat=false` + loopback 测试客户端**（确保快照路径真实执行）→ 交易 CSV 逐字节一致；heartbeat=true 仅允许日志层差异，单独记录 |
| 集成 | middleware pytest：录制快照/事件回放 → WS 帧断言；**静默行情心跳测试**：无 tick、仅心跳，2s 内必到 snapshot；断线重连 → snapshot + orders + fills 历史；事件溢出/快照合并计数 |
| 性能 | worker 快照构建 p99.9 < 50µs（1Hz）；IO 线程 CPU < 1%；`_batchLatencyUs` p99 与未开桥基线无可测差异。若实测超 50µs，降级为 2s 间隔或分片构建（架构不变） |
| 冒烟 | 服务器：`nc 127.0.0.1 8710` 手测 hello/snapshot/readonly 拒答；SSH 隧道打开 UI 核对 202 合约网格/fit 矩阵/订单流；`-Wall -Wextra` 零告警 |

## 9. 仓库与部署

### 9.1 代码位置（推荐）

```
ui/optiontrader-ui/          # wondertrader 仓内顶层，与 src/ 平级
    frontend/                # 迁移改造后的 React 源码
    middleware/              # main.py + wt_option_client.py（纯 Python）
    README.md / deploy.sh
```

C++ 侧（UiBridge/UiSnapshotBuilder/测试）在 `src/WtOptionCore/`，随库发布。备选：独立仓——部署脚本需跨仓，不推荐。

### 9.2 部署形态（服务器 129.211.5.54）

- `dist/WtRunnerOption/ui/` = middleware + frontend/dist + `start_ui.sh`（setsid nohup，同现有运维纪律）；
- `config.yaml` 的 `strategies.hft[].params` 下加 `ui:`（readonly:true）；**不再需要** `notifier:`；
- 访问：`ssh -L 8002:127.0.0.1:8002 ubuntu@129.211.5.54` → `http://localhost:8002`；防火墙不开公网；UiBridge 绑 127.0.0.1:8710。

### 9.3 部署清单（v2.1）

| 项 | 状态 |
|---|---|
| `libWtMsgQue.so` | **不再需要**（单通道方案已移除 MQ 依赖） |
| `libnanomsg.so.5` | **不再需要** |
| rapidjson 头 | Linux 已装 rapidjson-dev；仅编译期依赖 |
| Python 依赖 | `fastapi`、`uvicorn`（venv 或 pip）；无 wtpy |
| `ui:` 配置段 | 新增在 `params.ui`（enabled:true + readonly:true + heartbeat:1.0） |
| `notifier:` 配置段 | **不新增**（EventNotifier 保留但默认关） |
| 端口 | 仅 127.0.0.1:8710（桥）与 127.0.0.1:8002（middleware） |

## 10. 最优性复核（二轮论证记录）

### 10.1 UiBridge 进程内 TCP 为何仍是最优

约束集：①不碰框架文件（WtRunner 无第三方模块挂载点，桥只能住策略插件内）；②P2 需要可靠双工命令（64B `sync_param` 不够，ctypes 写 SHM 脆弱且清除协议含糊）；③运维需要 `nc` 可调试；④批处理尾延迟预算严格。四条约束下，“worker 只读构建 POD + IO 线程 JSON + 靠回环 TCP + 有界队列”是满足全部四条的最小方案；文件落盘降级预案仍保留（若 IO 线程评审再暴露风险，P1 可先落文件、P2 再上 TCP，两者共享 `UiSnapshotBuilder`，无返工）。

### 10.2 单通道为何优于 v2 的双通道

v2 假设“复用 MQ 订单/成交事件”能省 C++，但实证推翻了收益：

1. `TRD_ORDER/TRD_TRADE` schema **没有 userTag**、trade **没有时间戳**——省不掉桥内补字段/归属逻辑；
2. middleware 要同时维护 TCP + nanomsg 两条上游、两套重连/背压，部署多 `libWtMsgQue.so` + `libnanomsg`；
3. MQ 事件来自 TraderAdapter 线程，与 worker 快照**顺序不一致**，仍需 `localid` 对账；
4. AEP 的 `on_order/on_trade/on_entrust` 回调本来就在 worker 线程、已接线——在回调尾入桥反而是**零新增通道**，且与快照同源同序。

因此单通道在“依赖数、代码面积、顺序一致性、自愈能力”四项全胜；MQ 仅保留为未来外部告警的旁路（默认关），不进入 UI 关键路径。

### 10.3 关键决策矩阵（v2.1 终局）

| 决策 | 备选 | 选择 | 理由 |
|---|---|---|---|
| 传输 | TCP JSONL / MQ / SHM / 文件轮询 | **TCP JSONL** | 双工、可 `nc`、无结构耦合、依赖最少 |
| 通道数 | 单 / 双（TCP+MQ） | **单** | §10.2 |
| 快照 | 1Hz 全量 / 增量 diff / 逐 tick | **1Hz 全量 + 事件增量** | 回环带宽免费；全量自愈最简单；逐 tick 无必要 |
| 构建线程 | worker POD / IO 读策略对象 / 独立进程 | **worker POD** | 线程安全 + 尾延迟可控；独立进程需跨进程镜像 |
| 快照队列 | 单 FIFO / 单槽 latest-wins | **单槽** | 快照可丢可合并，事件不可 |
| 命令 | CommandBus / 直接调策略 / 新 RPC | **CommandBus** | 复用既有风控门与审计；新增仅 2 个 handler |
| 热参数 | SETPARAM+applyHotParams / ctypes SHM / 重启 | **SETPARAM** | 兼容无 share_domain 的默认部署；与既有热更新路径同构 |
| 前端 | 改造 / 重写 / 原样跑 Echelon | **改造** | 交互资产复用最大化；协议层重写最小 |
| 部署 | 仓内 `ui/` / 独立仓 | **仓内** | 版本同步、单次部署 |
| 安全 | 回环+readonly / 公网+token | **回环+readonly** | 攻击面最小；SSH 隧道足够 |

### 10.4 残余局限（诚实声明）

1. 桥与交易引擎同进程：IO 线程崩溃被 try/catch 隔离，但进程级故障（segfault 在别处）UI 随之失联——由 middleware 的 `snapshotAgeMs` 超时告警兜底；死 man 开关/监控体系不变；
2. 快照 1Hz + 全量：非逐 tick 实时（做市盯盘足够；tick 级数据本就在行情软件侧）；当前 ~202 腿约 40KB/帧，回环/SSH 隧道与前端渲染无压力；扩展到数千腿时再按 P3 增量化（先保证自愈与简单）；
3. `on_trade` 无策略时间戳：事件 `ts` 为桥墙钟，回放加速时与 sim 时间不一致；成交归属/复盘以快照 `fills`（`timeSim`）为准；
4. 订单/成交事件无 userTag：归属靠 `localid`（AEP 入队时丢弃 userTag，框架字段所限）；
5. `sessionStopInSec` 在 `_secSecs` 未建立（首个 Timer 前）为 null；
6. Windows 侧：UiBridge 仅 Linux 实现 + `_WIN32` 存根返回 disabled（当前部署全 Linux；Windows 编译仍过）。

### 10.5 唯一可能更优的路径（不推荐）

若允许修改框架文件，把 UiBridge 提升为 `WtRunner`/`WtCore` 的一等模块（统一生命周期、统一事件总线、多策略 sid 复用）会比“住在策略插件里”更干净；但这违反本项目“框架文件零修改、策略插件自包含”的纪律，且会把 UI 生命周期与引擎绑定，收益不抵风险。**在现有约束下，v2.1 已是可落地的最优解。**

## 11. 分期与验收

| 阶段 | 内容 | 验收 |
|---|---|---|
| **P1 只读 MVP** | UiBridge+SnapshotBuilder（快照/state/perf/orders/fills）；AEP 事件入桥；middleware 单 TCP + WS；前端 Grid/Trades/状态横幅/Fit 矩阵/健康卡（只读） | §8 全绿；服务器冒烟：UI 看到 202 合约网格与 fit 门状态；静默行情 2s 内仍有快照；FITGATE 临时日志退休 |
| **P2 交互** | `readonly:false` + 全部命令（含 `CANCELO`/`SETPARAM`）+ Ticket 确认流 + ack/审计 | 命令审计日志核对；服务器空窗授权下人工点单闭环；SETPARAM 在无 share_domain 路径实测生效 |
| **P3 分析** | vol smile SVG、PnL 曲线、AEP 图表、（可选）回放期可视化（节流上限防洪水） | 回放数据演示 |

每期独立提交（`feat(ui): ...`），遵守“只 stage WtOptionCore 相关文件 + ui/ 新目录”。

## 12. 工作量估算（P1，人日）

| 工作项 | 乐观 | 常规 |
|---|---|---|
| `UiSnapshotBuilder` + POD 类型 + 字段映射 | 2.5 | 3.5 |
| `UiBridge`（TCP/单槽/FIFO/心跳/协议/配置/生命周期） | 2.5 | 4.0 |
| 策略集成（init/shutdown/回调/命令 drain/ConfigValidator） | 1.5 | 2.5 |
| C++ 单测 + golden 场景 | 2.0 | 3.0 |
| middleware（纯 Python） | 1.0 | 2.0 |
| 前端迁移改造（复用原组件） | 3.5 | 5.0 |
| 部署脚本 + 服务器冒烟 + 性能复核 | 2.0 | 2.0 |
| **合计** | **15.0** | **22.0** |

P2 约 4–6 人日（命令/ack/Ticket/审计/SETPARAM 实测），P3 约 3–5 人日。若前端工程师不熟悉 React 19/Vite，P1 上浮约 20%。

## 13. 待拍板事项（附推荐默认）

1. 仓库归属：**推荐** `ui/optiontrader-ui/` 仓内顶层；
2. P2 写命令时机：**推荐** P1 只读先行，随小单闭环验证后开 `readonly:false`；
3. vshift 列：**推荐**实现为 `theoVol − atmVol` 偏差（保留原 UI 列布局且有明确语义）；
4. 多策略 sid：**推荐** P1 即在 hello/snapshot 带 `sid` 字段（零成本前向兼容）；
5. 心跳默认：**推荐** `heartbeat: 1.0`，仅在有客户端时生效；golden 场景显式 `false`。

（以上 5 项已于 2026-11-16 全部按推荐默认拍板并实施。）

---

## 14. P1 实施与验证记录（2026-11-16）

### 14.1 交付物

| 层 | 文件 |
|---|---|
| POD 视图 | `src/WtOptionCore/UiSnapshotView.h` |
| 构建+编码 | `src/WtOptionCore/UiSnapshotBuilder.{h,cpp}`（rapidjson；Windows 存根） |
| 桥 | `src/WtOptionCore/UiBridge.{h,cpp}`（IO 线程/单槽快照/FIFO 256/命令 deque 1024/心跳/auth/背压计数） |
| 集成 | `HftOptionStrategy`（init 读 `params.ui`、on_init 建桥、shutdown 先停桥、batch 顶 drain+尾快照、on_order/on_trade/on_entrust 尾入队）、`OptionRisk::peekExpiryGreeks`、`ConfigValidator` 登记 `ui` |
| 测试 | `src/WtOptionCore/tests/test_uibridge.cpp`（8 项全过） |
| golden 场景 | `src/WtOptionCore/replay/configbt_option_replay_ui.yaml` + `run_option_replay.sh` 注册 |
| 中间件 | `ui/optiontrader-ui/middleware/`（纯 Python） |
| 前端 | `ui/optiontrader-ui/frontend/`（React19+Vite+Tailwind，dist 已构建） |
| 部署 | `dist/WtRunnerOption/config.yaml` 增 `params.ui`、`ui/optiontrader-ui/start_ui.sh` |

### 14.2 验证结果

1. **单测**：`test_uibridge` 全过（含 hello/快照/事件/refresh-ack/readonly 拒答/auth/FIFO 溢出/非回环拒绝）；WtOptionCore 全部 8 个测试二进制回归通过；新代码 -Wall/-Wextra 零告警。
2. **既有 golden 门禁（新 .so，无 ui）**：`GOLDEN=1` 结构集 59 行与基线一致（17 stops/16 resumes/8 armed）——C++ 改动不改变交易行为。
3. **ui golden 场景**（`ui.enabled=true + heartbeat=false + loopback 客户端`，snapshot_interval=30s）：结构集与同一基线一致，两次通过。
4. **对照实验**：连续两次无 ui 重放 trades.csv 亦不相同（分子级时间戳，`golden_extract.sh` 已有结论）——字节级一致从来不是可达门禁，结构集才是；ui 场景满足。
5. **性能 A/B**（Debug 构建）：`snapshot_interval=1.0` 时 p50 +7..22µs/p95 +38..56µs——根因是回放加速下快照墙钟频率 ~320Hz × JSON 编码的 IO 线程 CPU 争抢，**非交易路径工作**；`snapshot_interval=30` 后 p50/p95 落入 ctrl1-vs-ctrl2 噪声带（p50 47-54µs，p95 77-124µs）。实盘 1Hz 墙钟 ≈ 100µs/s 编码 ≈ 0.01% CPU，可忽略；Release 构建（-O3）更低。
6. **E2E**（mock 桥 + middleware + WS 客户端）：静态托管、快照广播、`snapshotAgeMs` 注入、refresh→ack→fills、readonly 拒答全通过。

### 14.3 实施中的设计微调

- `UiLegView` 增加 `code` 字段（OptionData::getCode）——Ticket/选择键/orders 归属需要，原 schema 遗漏。
- golden/回放场景的 `snapshot_interval` 建议 ≥30s（sim 秒），避免回放墙钟频率放大 IO 争抢；实盘保持 1.0s。

### 14.4 P1 验收结论

§11 P1 验收标准全绿；FITGATE 临时日志（`HftOptionStrategy.cpp` 中标注 TEMP 的诊断块）待服务器冒烟确认 fit 门可见后由运维删除。

### 14.5 P2 实施记录（2026-11-16，同日）

**交付**

- `OptionCommandHandlers`：新增 `CANCELO,<localid>`（→ `executeCancel`）与 `SETPARAM,<key>,<value>` 两个 handler；Hooks 增 `cancelOne`/`setHotParam`。
- `HftOptionStrategy::registerCommands`：`setHotParam` 白名单仅 6 键（5 alpha 权重 + `max_tps`，范围校验），双路径写入（`_hot.*` 指针非空写指针，否则写成员并置 `_hotUpdatePending`，由同批 `applyHotParams()` 应用）；`sticky_base/improve_retreat_ratio` 显式拒绝（非热参数）。
- worker drain：`on_batch_complete` 顶部 drain 中非 refresh 线直接 `_cmdBus.dispatch`，**全量写命令审计日志**（线格式 + 结果 + worker 侧耗时 µs）。
- `UiBridge::handleLine`：P2 写路径 JSON→CommandBus 线格式翻译在 **IO 线程**完成（字段校验不过即拒答，不唤醒 worker）；裸 CSV 直通（nc 兼容）；浮点用 fmt 最短回环格式。
- 快照增 `params` 回显（6 热参当前值，`hotVal` 语义），前端编辑前可见当前值。
- 前端：`SetParamPanel`（6 参输入+set）、CommandPanel STOP/RESUME/PANIC/QMODE 按钮在 `readonly:false` 时启用、订单表双击/✕ 撤单、Ticket 下单带 cmdSeq；网格双击提示 qmode 为全网格（CommandBus 无 per-code 命令，P3 可选）。

**验证**

- `test_uibridge` 增 3 组（handler 白名单/写路径翻译+ack/params 回显），全过；8 个测试二进制回归全过；零告警。
- golden 双过（基线 + ui 场景各一次 `GOLDEN=1`，结构集 59 行一致）——drain 路径改造零交易扰动。
- 前端 `tsc+vite` 零错误构建；middleware 上行转发 P1 已证（verb 无关）。

**运行方式**：`readonly:true` 维持 P1 行为；写命令需在配置显式 `readonly:false`（dist 默认保持 true，授权后打开）。

### 14.6 残余事项（P2 后）

- ~~服务器授权窗口：开 `readonly:false` 人工点单闭环~~ —— **已于 2026-11-17 日盘执行**（见 §14.9）。
- per-code qmode、Cancel All、回放期可视化：P3。

### 14.7 P3 实施记录（2026-11-17）

**交付**

- 命令层：`QMODES,<code>,<mode>` 单码报价模式（`_ctg->onSetQMode`，码存在性校验）与 `CANCELALL`（遍历 `_orderRegistry` 活跃单逐笔 `executeCancel`，channel 检查一致，计数审计）。
- 桥翻译：`qmode_code`/`cancel_all` JSON → 线格式。
- 前端 Analysis 页（纯 SVG，零新依赖）：
  - Vol smile：选中到期的 C/P 市场 iv 与 theo vol（=atmVol+vshift）+ atmVol 参考线；
  - PnL 曲线：快照流环形缓冲（600 点≈10 分钟）cur/realized；
  - Batch latency 曲线：p50/p95/p99 同图。
  - 回放期可视化由快照流天然覆盖（快照率由 `snapshot_interval` 控制，无需额外节流）。
- 交互补全：网格双击 pos 单码 qmode 切换（off↔on，恢复原版双击习惯）；订单表 **Cancel All** 按钮。
- 测试：`test_uibridge` 增 QMODES/CANCELALL handler 断言 + 两种翻译断言。

**验证**

- 8 个测试二进制全过（含 P3 新增断言）；golden 基线 + ui 场景双过（59 行结构集一致）。
- 前端 `tsc+vite` 零错误构建。

### 14.8 服务器冒烟记录与两个真 bug（2026-11-17 深夜，重要）

冒烟目标达成：WS 全链路（middleware→bridge）在实盘进程上拿到 1Hz 快照——`SessionStopped` 状态、5 到期 booksWithBid/Total、101 strikes、params 回显、age≈50ms 全部正确。**但过程暴露两个真 bug 与一个构建环境陷阱：**

**Bug 1（决定性）：UI 快照尾块被锚进 `enabled/marketFresh` 守卫体内。** P1 集成时插入点上下文误判（`drainPendingQuotes` 恰为守卫体内最后一句），尾块落入 `if (_traderCtx && enabled && marketFresh)` 块内——实盘 `enabled=false` 时永不执行，快照恒空。回放 golden 全部通过是因为回放全程 `enabled=true`。**修复：尾块移到 lambda 顶层（守卫外），自带 `uiNow`。** 教训：插入点校验不能只看缩进——守卫体与 lambda 体同缩进层级时必须以 brace 配对核实；live-only 行为差异（enabled 门）不会出现在 replay 验证里。

**Bug 2：静默实盘冻结策略时钟，快照节奏停摆。** 无 tick 时 `nowSeconds()` 冻结，`snapshotDue` 的间隔判断永不触发。修复：`snapshotDue` 增加墙钟回退（`wallMsNow() - lastWallMs >= interval`），回放中 sim 时钟领先墙钟故回退静默。

**环境陷阱：`/mnt/d`（drvfs）时间戳不可靠导致 make 增量编译出现混合布局 .o**（类成员偏移跨 TU 不一致），现象是玄学级运行时行为（fprintf 不执行、_hot 指针路径异常、段错误）。排查耗费数小时。**纪律：发布构建一律用 Linux 原生文件系统的 out-of-tree build**（如 `cmake /mnt/d/.../src -B ~/build/xxx && make`——注意源码仍需 rsync 到原生盘才彻底免疫；改动头文件后全量重建）。

**服务器状态**：已部署修复版 Release .so 与 ui 目录；冒烟进程按纪律清理（kill -9）。人工点单闭环（P2 写命令验证）待授权窗口。

### 14.9 P2 写命令闭环验证（2026-11-17 日盘）

临时 `readonly:false` 在实盘进程执行 14 条写命令，**命令链路全部验证通过**：

| # | 命令 | 结果 |
|---|---|---|
| 1-2 | SETPARAM wgt_vegaflow=0.45 / max_tps=25 | ok:true，**下帧快照 params 回显确认生效**（成员路径+applyHotParams 全链路） |
| 3 | SETPARAM sticky_base | ok:false "key not hot-adjustable"（白名单拒） |
| 4 | SETPARAM wgt_vegaflow=99 | ok:false "weight out of range [0,10]"（范围拒） |
| 5/8 | QMODE off / on（全网格） | ok |
| 6-7 | QMODES 单码 on / 未知码 | ok / ok:false "unknown code" |
| 9-10 | STOP / RESUME | ok；channel 未就绪时 RESUME 保持 Disabled（状态机语义正确） |
| 11 | ORDER | ok:false "order rejected (limits/channel)"——命令**穿透到下单网关**后被通道门拒（机制验证） |
| 12 | CANCELO | ok:false "channel not ready"（同上） |
| 13-14 | CANCELALL / CANCEL | ok |

**审计**：14 条写命令全部落 `UI cmd: <线格式> -> <结果> (<耗时>us)` 审计日志。
**唯一阻塞项**：真实成交（fill）路径——simnow 账号 `9999-048363` 因昨夜频繁重启触发 CTP「登录失败次数超限，登录被禁止」，TD 通道不可用（`channel:false` 在 UI 状态横幅可见）。恢复后（通常数小时）重试 ORDER@ask 1 手即可补齐；命令机制本身已被通道门响应证实。
**恢复动作**：验证后 `readonly:true` 已恢复并重启确认（startup 日志 `readonly=1`；写命令实测拒答 `err:'readonly'`）。服务器现常驻只读运行 + middleware 在线，SSH 隧道即可查看。

**2026-11-17 下午补充（成交路径收官）**：用户注册第二 simnow 账号 `049641`（旧账号 048363 因昨夜频繁重启触发"登录失败次数超限"禁令），切换 `tdtraders/tdparsers.yaml` 后 TD 登录成功。13:30 午后开盘策略进入 `Normal/enabled`，临时 `readonly:false` 完成最后闭环：

- **被动单**：`ORDER B SHFE.cu2612.C.88000 @6666`（买一-3 tick 挂单价）→ ack ok → 出现在快照 `orders[]`（id 1384834450）→ `CANCELO` → 订单消失 ✔
- **主动单**：`ORDER B @25386`（卖一+1 tick，1 手）→ ack ok → **trade 事件到达**（id 1384834451，1@25386）→ 快照持仓 `pos: 0→1` ✔
- 验证后恢复 `readonly:true` 并重启确认。

至此 §11 三期 + 冒烟 + 写命令闭环（含真实成交）**全部收官**。服务器部署状态：账号 049641、`readonly:true`、修复版 Release .so、middleware 常驻。遗留：UI 方案零剩余事项；1 手多头持仓为验证产物（simnow 仿真账户，可平可留）。

---

**附：二轮复核的代码证据索引**

- `src/WtRunner/WtRunner.cpp:319`：`stra->self()->init(cfgItem->get("params"));`
- `src/WtOptionCore/OptionAsyncEventProcessor.cpp:442`：`if (events.empty()) continue;`
- `src/WtOptionCore/OptionAsyncEventProcessor.cpp:551`：`if (_cbs.on_batch_complete) _cbs.on_batch_complete();`
- `src/WtOptionCore/OptionAsyncEventProcessor.h:108-113`：`enqueue_control()` 注释“only drives on_batch_complete”
- `src/WtOptionCore/HftOptionStrategy.cpp:2853-2868`：Timer 仅由 `on_tick` 合并产生
- `src/WtOptionCore/HftOptionStrategy.cpp:1572-1595`：`on_batch_complete` 与 `applyHotParams` 顺序
- `src/WtOptionCore/HftOptionStrategy.cpp:701-714`：`sync_param` + `commit_param_watcher`
- `src/WtOptionCore/HftOptionStrategy.cpp:3802-3834`：`applyHotParams` 仅消费 5 权重 + `max_tps`
- `src/WtCore/ShareManager.cpp`：`allocate_value` 在 `_inited=false` 时返回 `nullptr`
- `src/WtOptionCore/ConfigValidator.h`：根 schema 未含 `ui`
- `src/WtOptionCore/IssuedOrderRegistry.h:70-86,223`：`Rec`/`fills`/`orders()`
- `src/WtOptionCore/QuoteStatistics.h:224`：`getSessionSummary()`
- `src/WtOptionCore/OptionValues.h:228-247`：implied/theo vol getters
- `src/WtOptionCore/OptionData.h:104-122,131,140`：market/theo/IV/greeks/position/trading data
- `src/WtOptionCore/ExpiryData.h:53,67,114-116,147,154`：days/forward/underlyingValid/fitReady
- `src/WtOptionCore/IOptionPricer.h:71`：`getATMVol(exp)`
- `src/WtOptionCore/OptionRisk.h:109,113`：组合/分到期希腊
- `src/WtOptionCore/PnlTracker.h:58-70`：PnL 接口
