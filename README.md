# WonderTrader / WtFutuCore —— 期货高频做市引擎

WonderTrader 是 C++17 量化交易框架。本仓库当前工作重心为 `src/WtFutuCore`：
面向期货做市的超低延迟策略引擎（GLFT 报价模型 + alpha 集成信号 + B+ 事件驱动重挂）。

## 架构（WtFutuCore）

```
MdSpi(行情) ─┬─ on_tick ─→ 热路径: 信号集成 → GLFT 报价 → refreshQuotes(CTP 发单)
             ├─ on_transaction/on_order_detail → 毒性/OFI 更新
TdSpi(交易) ─┬─ on_trade/on_entrust/on_order → UnifiedOrderTracker → B+ 补挂
arb 线程 ────┼─ AsyncArbitrageExecutor(跨腿仲裁) / FutuRiskMonitor(停机域, 原子化)
Timer ───────┴─ 周期风控/统计
```

- **策略壳** `UftFutuMmStrategy`：回调入口统一 `FUTU_CB_LOCK_GUARD` 大锁 +
  WS-E 命令通道（channel/session 序列经 PendingCommand 单飞执行）
- **并发基础设施**：TradingState 原子 CAS、RecursiveSpinLock 细粒度域
  （FutuRiskMonitor 停机域 / SelfTradeCalibrator / `_oid_to_pair`）、
  PerformanceMonitor 全原子计数器 + SPSC RingBuffer
- **去大锁开关** `FUTU_CB_LOCK_BIG=big|none`（默认 big；切 none 的全部代码阻断点已清零，
  物理删除 `_cb_mtx` 待 TSAN/逐比特 A/B/灰度流程验收，见 `src/WtFutuCore/AGENTS.md`）

## 功能特性

- GLFT（Guéant–Lehalle–Fernandez-T）公允报价与库存风险定价
- alpha 集成：OFI / 成交动量 / 盘口失衡 / 动量 / lead-lag，权重热更新（hotparams.yaml）
- B+ 事件驱动补挂、自成交校准（SelfTradeCalibrator）、毒性熔断（toxicity breaker）
- 跨腿套利（SpreadArbMgr）、组合级 delta 风控、义务/section 双边统计落盘（内存积压+安静期定点刷盘）
- 回测 mocker（撮合参数可配）与产线共用同一策略代码路径，支持逐比特 A/B

## 构建 / 测试 / 回测

```bash
# 构建（Linux）
cd src/build_all && cmake .. && make -j$(nproc) WtFutuCore TestUnits

# 单元测试（GoogleTest, src/TestUnits）
./build_x64/Debug/bin/TestUnits/TestUnits   # 当前基线 113/113

# 回测（部署 so 后）
cp src/build_all/build_x64/Debug/bin/WtUftRunner/futu/libWtFutuCore.so dist/WtBtFutu/uft/
cd dist/WtBtFutu && LD_LIBRARY_PATH=./uft ./uft/WtBtRunner -c _ec_5d.yaml -l logcfgbt.yaml
```

Windows：MSVC 打开 `src/all.sln` 或 `uft.sln`，需 `MyDepends141` 环境变量指向依赖库。

## 配置文件（src/WtFutuCore/config/）

| 文件 | 内容 |
|---|---|
| `config.yaml` | UftRunner 主配置：basefiles/parsers/traders/env/data/bspolicy/strategies |
| `coordinator.yaml` | 协调器（订单路由/风控协调）参数 |
| `hotparams.yaml` | 运行时热更新参数：基差/数量阶梯/max_delta/alpha 权重等 26 项 |
| `spread_arbitrage.yaml` | 跨腿套利腿定义与阈值 |

回测配置样例见 `dist/WtBtFutu/*.yaml`（`_ec_5d.yaml` 为全量回归基准）。

## 文档

- `docs/ARCHITECTURE.md` 及各专题设计文档（毒性分析/平仓执行器/alpha 框架等）
- `src/WtFutuCore/AGENTS.md`：开发规范、去大锁方案全记录、验证基线与遗留清单
