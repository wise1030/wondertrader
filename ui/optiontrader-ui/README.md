# WtOptionCore 监控 UI（P1 只读版）

为 WtOptionCore 期权做市引擎提供的实时 Web 控制台（设计文档：
`src/WtOptionCore/analysis/ui-migration-design-2026-11-16.md` v2.1）。

## 架构

```
WtRunner(HftOptionStrategy)                    本目录
  └─ UiBridge 127.0.0.1:8710 (JSON Lines)  ◀──▶  middleware (fastapi/uvicorn :8002)
                                                    └─ WS /ws + 静态托管 frontend/dist
                                                       └─▶ 浏览器
```

- 桥接在策略插件进程内（`params.ui.enabled=true` 才创建，默认关闭——回测加载同一 .so 零影响）。
- 下行：1Hz 全量快照（seq 水位去重）+ 订单/成交/拒单增量事件 + 重连 fills 历史。
- 上行（P1）：仅 `refresh`；其余命令被 readonly 硬门拒绝（P2 开放）。
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

```bash
ssh -L 8002:127.0.0.1:8002 ubuntu@129.211.5.54
# 浏览器 http://localhost:8002
```

防火墙不开公网；UiBridge 只绑回环。调试可用 `nc 127.0.0.1 8710` 直发
`REFRESH` 或 JSON 行。

## 目录

- `middleware/` — fastapi + asyncio TCP 行客户端（单上游，无 wtpy/nanomsg 依赖）
- `frontend/` — React19 + Vite + Tailwind（迁移自 quantbox optiontrader UI，
  协议层/平铺属性归一化已废弃，改为结构化快照 adapter）

## 测试

- C++：`src/build_all/WtOptionCore/test_uibridge`（单测 + 回环集成）。
- golden：`YAML=configbt_option_replay_ui.yaml GOLDEN=1 bash
  src/WtOptionCore/replay/run_option_replay.sh`（ui 开启 + loopback 客户端，
  结构与 default 基线一致；trades 时间戳为分子级非确定，见 golden_extract.sh）。
