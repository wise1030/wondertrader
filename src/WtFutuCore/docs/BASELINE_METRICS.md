# WtFutuCore 基线度量 (P3.2)

> 改造前后对比锚点。当前为 P1+P2 完成后状态。
> 测量环境: WSL Debug 构建, EC 单日回测 (`_ec_5d_perf.yaml`)。

## 构建与体积
| 指标 | 值 | 说明 |
|------|-----|------|
| WtFutuCore 干净重建 | 77.7s | `-j$(nproc)`, Debug, ~116 源文件 |
| libWtFutuCore.so | 67.8 MB | Debug 构建 (Release 更小) |

## 测试
| 指标 | 值 |
|------|-----|
| TestUnits | 20/22 通过 (2 失败: `test_shm.test_sharehelper` / `test_session.test_allday` - 框架层环境依赖, 无 WtFutuCore 依赖) |

## 回测 tick-to-quote 延迟 (PerformanceMonitor, backtest env)
| 指标 | 值 |
|------|-----|
| mean | 77.42 us |
| p99 | 1856.90 us |
| max | 3219.05 us |
| n | 16384 ticks |
| 高延迟事件 (>1000us 阈值) | 7264 |

> **注**: 回测环境延迟 (HftMocker 单线程 replay), 非实盘 UFT 绝对延迟。用作**相对基线**: 未来改造后重测, mean/p99 显著上升则 flag。实盘延迟须在 UFT 环境另测。

## 测量方法 (复现)
- 构建: `cd src/build_all && cmake .. && time make -j$(nproc) WtFutuCore` (删 `WtFutuCore/CMakeFiles/WtFutuCore.dir` 后干净重建)
- 延迟: `dist/WtBtFutu` 跑 `_ec_5d_perf.yaml`, 临时置 `usePerformanceMonitor: true` + `performance.enabled: true`, grep `[PERF] === Performance Summary ===`
