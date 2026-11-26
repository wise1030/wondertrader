# 低延迟优化方案：underlying-driven + 重活移出 worker（2026-11-25）

> 依据：全天 2466 批采样——中位 p50=8μs / p95=37μs / p99=65μs（已达标），p90 尾部 20–70ms 尖峰与 `slow_compute_interval=10s` 全量重算、60s 压力测试、60s 拟合发布同频。目标：**不动 FAST 热路径语义，把周期性重活从 worker 热路径挪走/降频**。
>
> 借鉴：quantbox 的 underlying-driven 调度（只有标的变化才重算）+ 本项目已有 `SingleFlightTaskRunner`（AsyncVolFitter 已在用的单飞后台执行器）。**不引入新线程模型、不上 OpenMP。**

---

## 1. 现状热路径与尖峰归因

| 周期任务 | 位置 | 频率 | 估算单次耗时 | 影响 |
|----------|------|------|--------------|------|
| SLOW 全量重算（`computeValues_SLOW`，全 strike BlackCalc+IV+曲线） | worker `shouldCompute` 内 | 10s | 20–70ms | 主因 |
| 压力测试 `OptionStress::evaluateAll/worst` | worker 同块 | 60s | 数 ms–数十 ms | 次因 |
| 拟合快照构建 `submitAsyncVolFit`（O(strikes) 收集点） | worker | 60s | <1ms | 小 |
| 拟合发布 `tryPublish`（写回曲线/ATM re-anchor） | worker | 拟合完成时 | 小 | 小 |

FAST 路径（underlyingChanged + 20ms debounce）保持现状：中位指标已优秀。

## 2. 方案（三档，按风险递增）

### 方案 A（首选，最小改动）：SLOW 周期兜底调长 + 保留拟合发布即时 SLOW

**复核订正（2026-11-25）**：原方案 A 的三条触发条件中，"新拟合曲线发布即全量重算"**原版与现版均已存在**（`CompositeOptionPricer.cpp:158-162` 注册 `onFitCompleted` → `m_bReprice=true` → 下批强制 SLOW），无需新增"fit 版本变化检测"代码。原版的 SLOW 调度本来就是"**周期兜底 + 事件强制**"：

```
m_bReprice = m_bReprice || (tv_diff > slow_compute_interval)   // 周期兜底（现生产 10s）
if (m_bReprice) → computeValues_SLOW（全量）                  // 全量重算
onFitCompleted → m_bReprice = true                            // 拟合发布 → 下批即 SLOW（已存在）
```

**方案修订为**：
1. **`slow_compute_interval` 生产值 10 → 60**（纯配置，一行）。平时 SLOW 从 1/10s 降到 1/60s，尾部尖峰约降为原来的 1/6；保留 60s 兜底防全量数据陈旧。
2. **保留 onFitCompleted 即时 SLOW**（现版已具备）——新拟合曲线发布仍即时全量重算，不受周期调长影响。
3. 可选增强（二期，原版没有）：forward 变动超过阈值（如 0.2% 或 2 tick）立即 SLOW。一期不做。

- 效果：尾部尖峰约降为原来的 1/6，新曲线生效语义不变，FAST 热路径完全不动。
- **零代码改动、零语义风险**（一期只需 config 一行 + 启动日志确认 SLOW 周期为 60s）。

### 方案 B：压力测试移出 worker（SingleFlight 复用）

- 把 `OptionStress::evaluateAll/worst` 的输入（position greeks 快照 + spot）打包投到 **SingleFlightTaskRunner**（AsyncVolFitter 同款后台单飞），worker 只消费结果（`pushUiAlert`/写 risk 字段）。
- 收益：压力测试不再占用批处理时间；worker 每批只做轻量快照（O(expiries)，极轻）。
- 风险：低——压力测试是只读分析，不写报价状态。

### 方案 C（备选，若 A+B 后尾部仍高）：SLOW 后台化（写侧双缓冲）

- 在后台跑 SLOW 全量重算并写**非活跃 values 槽**，由 worker 在批边界 `commitUpdateValues()` 切换活跃槽（`OptionData::beginUpdateValues/commitUpdateValues` 已有双缓冲机制）。
- 风险：FAST/SLOW 写槽一致性、 Greeks/Risk 读取时机——需严格"后台只写非活跃槽、worker 独占提交"的纪律。**不建议首选**（复杂度高、收益相对 A 有限）。

## 3. 实施顺序与验证

1. **方案 A 一期（配置）**：`slow_compute_interval: 10 → 60`，保留 onFitCompleted 即时 SLOW → replay 对比尾部 p90/p99（目标：p90 尾部 <15ms）；
2. **方案 B**（Stress 后台化）→ 同上复测；
3. **方案 A 二期（可选增强）**：forward 大变动立即 SLOW（原版没有，仅在确有需要时加）。

验证：
- 单测：SLOW 周期兜底触发、onFitCompleted 即时 SLOW 各命中一次；
- replay（11-24/25 数据）：统计 batch p50/p95/p99/p99.9 分布对比；
- SimNow 观察 1 个交易日：确认报价行为与现状一致（FAST 不变）。

## 4. 明确不做

- 不上 OpenMP / 多线程定价（中位已达标，避免共享状态竞态）；
- 不改 FAST 的 underlying-driven + 20ms debounce 主逻辑；
- 不把拟合计算挪回 worker（已在后台，保持）。
