# WtOptionCore 监控 UI（P1 只读版）

为 WtOptionCore 期权做市引擎提供的实时 Web 控制台（设计文档：
`src/WtOptionCore/analysis/ui-migration-design-2026-11-16.md` v2.1）。
**操作功能手册：[`docs/USER-MANUAL.md`](docs/USER-MANUAL.md)**（接入/布局/交互/协议/安全/排障）。

## 架构

```
WtRunner(HftOptionStrategy)                    本目录
  └─ UiBridge 127.0.0.1:8710 (JSON Lines)  ◀──▶  middleware (fastapi/uvicorn :8002)
                                                    └─ WS /ws + 静态托管 frontend/dist
                                                       └─▶ 浏览器
```

- 桥接在策略插件进程内（`params.ui.enabled=true` 才创建，默认关闭——回测加载同一 .so 零影响）。
- 下行：1Hz 全量快照（seq 水位去重）+ 订单/成交/拒单增量事件 + 重连 fills 历史。
- 上行：P1 仅 `refresh`；P2 已支持 STOP/RESUME/PANIC/QMODE/ORDER/CANCEL/CANCELO/SETPARAM，
  JSON 在桥内翻译成 CommandBus 线格式、worker 批顶执行并审计；**`readonly:true` 时
  除 refresh 外全部拒答**（写命令需配置显式 `readonly:false` 并授权）。
- 静默行情：有客户端时每 `heartbeat` 秒 `enqueue_control()` 唤醒出快照（golden/回测必须 `heartbeat:false`）。

## 启动（服务器）

```bash
cd ui/optiontrader-ui
./start_ui.sh            # setsid nohup 起 middleware :8002（运维纪律同 dist/start.sh）
```

策略侧在 `dist/WtRunnerOption/config.yaml` 的 `strategies.hft[].params` 下加：

```yaml
            ui:
                enabled: true
                bind: 127.0.0.1
                port: 8710
                snapshot_interval: 1.0
                heartbeat: 1.0
                readonly: true
```

## 访问

开发/内网（无认证，仅回环）：

```bash
ssh -L 8002:127.0.0.1:8002 ubuntu@129.211.5.54
# 浏览器 http://localhost:8002
```

**互联网直接访问（token 认证）**：在 `middleware.env` 里设
`WTOPTION_LISTEN_HOST=0.0.0.0` + `WTOPTION_UI_TOKEN=<强随机串>`，云安全组放行
TCP 8002。首次访问 `http://129.211.5.54:8002/?token=<串>`（种 cookie），之后
直接开 `http://129.211.5.54:8002` 即可。无 token 时进程拒绝绑非回环地址。
注意：HTTP 明文，token 仅防君子；更高要求请套 HTTPS/反向代理。UiBridge
始终保持仅回环。

## 目录

- `middleware/` — fastapi + asyncio TCP 行客户端（单上游，无 wtpy/nanomsg 依赖）
- `frontend/` — React19 + Vite + Tailwind（迁移自 quantbox optiontrader UI，
  协议层/平铺属性归一化已废弃，改为结构化快照 adapter）

## 功能（三期已全量交付）

- **Grid**：期权网格（市场价/theo/iv/delta/vshift/持仓/qmode 着色）、F1–F8 到期切换、
  点价 Ticket、双击 pos 单码 qmode 切换、Ctrl+V vshift 列。
- **Trades**：活跃订单（双击/✕ 撤单、Cancel All）+ 最近 500 笔成交。
- **Analysis**：Vol smile（C/P 市场 iv + theo vol + atmVol 参考线）、PnL 曲线、
  批延迟 p50/p95/p99 曲线（快照流环形缓冲 600 点）。
- **状态**：TradingState 横幅（含 session 倒计时）、组合希腊/PnL 条、
  到期 fit 就绪灯、健康卡（批延迟/AEP 计数/drop 计数/快照龄期）。
- **命令**：refresh/STOP/RESUME/PANIC/QMODE(全网格)/QMODE_CODE(单码)/
  ORDER/CANCEL/CANCEL_ORDER/CANCEL_ALL/SETPARAM(6 热参白名单)；
  `readonly:true` 时仅 refresh，写命令需显式 `readonly:false` 并授权。

## 测试

- C++：`src/build_all/WtOptionCore/test_uibridge`（单测 + 回环集成）。
- golden：`YAML=configbt_option_replay_ui.yaml GOLDEN=1 bash
  src/WtOptionCore/replay/run_option_replay.sh`（ui 开启 + loopback 客户端，
  结构与 default 基线一致；trades 时间戳为分子级非确定，见 golden_extract.sh）。
