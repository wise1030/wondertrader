# 遗留项清单（2026-11-25，除已知观察点外）

> 状态口径：【待办】已定位需处理；【进行】部分完成待验证；【可选】二期增强/调优；【观察】仅需监控、非缺陷。

---

## A. 运营/人工待办（最高优先）

| # | 项 | 现状/证据 | 建议动作 |
|---|----|-----------|----------|
| A1 | **遗留持仓平仓**（15:00 763 手/54 码 → 最新快照约 723 手/24 码，含 cu.2612 期货 82L/101S、cu.2701 空 210+50） | `analysis/closeout-20261125.md` 已出（含 CSV、平今/平昨方向、P.84000 手工提示） | **人工执行清单**；执行后重核柜台持仓归零 |
| A2 | **SimNow 资金缺口**（error 31，全天 474 次） | 保证金被遗留库存占满，任何 OPEN 方向报单被拒 | 与 A1 联动：平仓释放保证金或入金；否则夜盘降险买单仍被拒（REDUCING CHANNEL 告警） |
| A3 | **vega -17.7k 超限（max_vega=10k，severity 1.79）** | 遗留 ITM put 空头库存 + `enable_auto_close: false`，已挂 5 天 | 根源处置 = A1 平仓 + 处置库存结构；`max_delta=1000` 维持（已定） |
| A4 | **P.84000 撤单上限**（`cancel_total_limits: 470`） | 22:12 曾触发 riskmon 排除；重启已清，但撤单率 90.9% 会再累积 | 观察撤单率；必要时调报价行为（avoid_trade 频率/参数）而非抬上限 |

## B. 已定位的技术债（按优先级）

| # | 项 | 状态 | 建议 |
|---|----|------|------|
| B1–B14 | **本轮已全部执行** | 完成/关闭 | 逐项代码、数值归因、测试和生产证据见 `analysis/b-tasks-closure-20261125.md`。B4 为工程修复闭环，历史根因仍受原始全线程 core 缺失限制；B9/B10/B12/B14 的“保留”均属显式设计决策，不再是未决技术债 |

## C. 二期可选增强/调优（方案已有或原型已备）

| # | 项 | 备注 |
|---|----|------|
| C1 | **GARCH-lite replay A/B** | `GarchLiteEstimator` + `setGarchBlend` 已实现（weight=0 默认关）；用 `test_replay_benchmark` 对比 GVV vs GARCH-lite 的 theo 偏差/翼部 medDev 后再启用 `garch_weight` |
| C2 | **扫描器调优** | StrikeSpreadScanner 已灰度接入（min_edge/garch 参数默认）；先观察首跑成交/拒单分布再调参 |
| C3 | **sticky_force / 深度门槛启用** | queue-pos 真排队量已做；sticky_force（按盘口档位量加权）与 `min_market_depth_qty`（默认 0=off）为可选项 |
| C4 | **feedPanic（行情连续 stale 升级）** | 现 marketFresh 只停刷新；连续 stale 超阈是否升级 panic 源，二期评估 |
| C5 | **panic 信号链扩展**（波动跳变/盘口消失等可配置信号类型） | 现 `signals.risk[]` 仅 Toxicity/PnlLimit；可按需扩展 SignalFactory 类型 |
| C6 | **`_riskSeverity` 前端展示** | 后端字段 `risk.severity` 已上 WS 快照；前端 Risk 面板显示可后补 |
| C7 | **GARCH 估计器与 `AnalysisRecorder` 分钟 bar 对接** | 现用 underlying tick EWMA；可改用分钟收益序列（更稳），复用分析记录器数据 |
| C8 | **`panic_backoff` cap（1800s）参数化** | 现硬编码，可挪 config |
| C9 | **alpha 权重与 `signals.alpha[]` 的运行时归因** | 已 config 化；可补 per-signal enable/threshold 细调与归因 |
| C10 | **OpenMP（`use_parallel`）** | 刻意不启用；中位延迟已优，暂不评估 |

## D. 观察项（非缺陷，仅监控）

| # | 项 |
|---|----|
| D1 | 今晚 21:00 夜盘：SLOW 60s 周期尾部延迟分布（目标 p90 <15ms） |
| D2 | 扫描器首跑组合单行为（SimNow 资金限制下拒单/取消属预期） |
| D3 | REJECT-ONLY 释放后反复闩锁频率（资金不足下的自然振荡） |
| D4 | failStreak 告警（每 60s）是否连续——资金恢复后应自动停止 |
| D5 | `cancel ack lost` / `late fill` / `app discard` 小量计数 |
| D6 | P.84000 撤单率与 riskmon 排除是否再触发（A4 联动） |
| D7 | `hot_params.json` 回放后 targ_delta/targ_vega 运营值是否需要非零目标位（现为 0=推向 0） |
| D8 | futures 双侧挂单（`und.ourBid/ourAsk`）与 OQM 串行换价的成交率 |

## E. 已关闭（本轮完成，勿再列）

- **B1–B14 全部执行完成**：逐项实现、决策、测试和生产证据见 `b-tasks-closure-20261125.md`
- PositionGuard per-side 假背离、reconcile 误初始化（reset+onFill 防御）
- 隔夜对冲风暴（时段门控）、REJECT-ONLY 永久闩锁（释放窗口）、OQM 计数器重置时序
- fit 未就绪算理论价/报单（valuesReady 门控）、多层挂单（max_side_orders 强制）
- panic 静态比值/循环（移除 + 指数退避 + failStreak 转告警）
- 202612 config 到期（enable: false）、`pricer:` 段头误吞（validateStructure）
- alpha 热参面板 wgt_*（已移除）、queue-pos 真排队量、GARCH-lite 原型、StrikeSpreadScanner 接入、SLOW 周期 10→60
