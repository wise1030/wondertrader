# 跟进点实施记录 + futures 死函数删除复核（2026-11-25）

## A. futures 死函数删除是否误删？——结论：未误删

### A.1 删除对象与证据
`updateOurMarketSide`（option 版）与 `updateOurMarketSideFuture`（futures 版）在删除前均为**零调用点死代码**（全库 grep 仅定义，无调用）。原版 quantbox 的 `updateOurMarketSideFuture` 是活跃 per-side 路径，但现项目早已由另一条路径覆盖。

### A.2 futures 双边报价的真实活跃路径（不依赖被删函数）
```
CompositeOptionPricer::computeOurMarketsFuture(ed)
  → mid = ed.forward；spread = sprd_fut/2 + sticky floor
  → risk_adjustment_future(ed)（方向性偏移，净空头 delta 时 bid 激进/ask 高挂）
  → __computeBidAndAskFuture + round_to_tick
  → size clamp（max_pos_fut，且含 v3 风险收缩）
  → mkt.setBest(BID/ASK)（UTD ourMarket 双侧）
  → CTG futures UTD desired → OQM future
      enable_quote_api=true + quote_style=paired → sendQuote（双腿）
      （buy_sell 时 sendSingle）
```
futures 的 ourMarket 由该路径**直接写入双侧 best bid/ask**；`updateOurMarketSideFuture` 只是一个等价的 per-side 包装，删除它不改变任何运行时行为。若将来需要 per-side 细粒度（如独立 sticky/teenie 生命周期），可在 `computeOurMarketsFuture` 内补齐，而非恢复死代码。

### A.3 验证口径
- 快照字段：`expiries[].und.ourBid/ourAsk/ourBidSz/ourAskSz`（UiSnapshotBuilder::buildExpiry → hasUndOur）即为 futures 双侧报价的直接观测量，可用其复核（夜盘开盘后核对非空）。
- 结论：**futures 双边报价不依赖被删函数，删除正确且已完成。**

---

## B. 三个跟进点实施状态（全部完成，16:55 部署 PID 3164653）

| 跟进点 | 实施 | 验证 |
|--------|------|------|
| ① config patch 校验流程固化 | `ConfigValidator::validateStructure`：启动必需段（pricer/expiries/orderManager）缺失即 LL_ERROR 明确指出段头被吞 | 15:08 修复 pricer 段头后，16:55 启动 **0 UNKNOWN key、0 CONFIG-STRUCTURE** |
| ② vega shift 量纲根治 | 新增 **vol 空间** 参数 `risk_shift_distort_vega_vol / risk_shift_max_vega_vol`（vol 分数，`shift_vol × vega_leg` = 元，量纲纯净），优先于旧元键；config 已替换为 `0.002 / 0.005`；热参同步接线 | 启动正常、无 REJECT；下轮夜盘观察平移幅度（预期 <1 tick 量级） |
| ③ `_riskSeverity` 接 UI | `UiSnapshotView.riskSeverity` + `UiSnapshotBuilder` encode `risk.severity` + strategy 赋值 | WS 快照已含 `severity` 字段（当前 0.0，收盘后无持仓 greeks） |

9/9 测试通过；备份 `libWtOptionCore.so.bak-20261125-followups`。

## C. 已出方案（未动代码）
- 低延迟：`analysis/latency-offloader-plan.md`（SLOW 条件化 + Stress 后台化，复用 SingleFlightTaskRunner）
- 借鉴适配：`analysis/borrow-garch-book-scanner-alpha-plan.md`（GARCH-lite 复用 IVolCurve/AsyncVolFitter、IBook 用现有多档盘口、扫描器复用 OptionScanCoordinator+ComboOrders、alpha 信号 config 化并清理热参 wgt_*）
