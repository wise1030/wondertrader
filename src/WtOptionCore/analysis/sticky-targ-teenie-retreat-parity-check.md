# sticky / targ / teenie / retreat 与原版 quantbox 对照（2026-11-25）

> 对照对象：`wondertrader/src/WtOptionCore/CompositeOptionPricer.cpp` vs 原版 `quantbox/quantbox/strategy/optioncore/CompositeOptionPricer.cc`（2709 行）。逐项核对实现、默认值、生产配置与启用状态。

---

## 1. sticky —— ✅ 完全对齐

| 项 | 原版（cc:1634-1660） | 现版（cpp:740-757） |
|----|----------------------|---------------------|
| 基量 | `base = sticky_base × tick_size` | 同 |
| 方向性比值 | BID：`upper=improve_retreat_ratio, lower=1`；ASK 镜像 | 同（逐行一致） |
| 粘住条件 | `new_px ∈ (our−lower·pts, our+upper·pts)` → 保持原价 | 同 |
| 默认值 | `sticky_base=0.5`、`improve_retreat_ratio=3.0`（Params.xml） | 同（生产 config 3.0） |
| 语义 | BID 改善方向（上移）3 倍宽粘滞带（改善保守）、撤退方向 1 倍（撤退更容易）——"more aggressive in retreating, less aggressive in improving" | 同 |

原版有注释掉的 `sticky_force`（按盘口档位挂单量加权）与 `sticky_max_ticks` 封顶——两侧均未启用，无差异。

**调用点差异**：两侧都在 `__computeBidAndAsk` 与 `computeOurMarkets` 各调用一次（双重 sticky），结构一致。

**结论：对齐。** 唯一的业务注意点：risk shift（L3/软限）生效时 sticky 的粘滞会阻碍偏移落地——v3 方案已包含"`|pgn|>0.5` 时 sticky 降权"，这是对原版的**业务性增强**（原版无 risk shift 联动，因为原版 shift 也被注释）。

## 2. targ 目标位 —— ✅ 语义对齐，现版**更优**

| 项 | 原版（cc:443-446, 2085, 2241） | 现版 |
|----|-------------------------------|------|
| 存储 | `m_prop_exp_{delta,vega_tw}_targets`（notifiable property，`<exp>.targ.delta/.targ.vega_tw`） | `m_prop_exp_{delta,vega_tw}_targets` 同名同结构 |
| 用途 | delta `pgn=(pg−targ)/tol`（cc:2085）、vega `pgn=(pg−targ0)/tol`（cc:2241） | 完全一致（cpp:1341-1353, 1520-1543） |
| 写入通道 | **仅 monitor property**（GUI/monitor 运行时写），默认 0 | **三通道**：config 逐月 `targ_delta/targ_vega` → 热参（ParamRegistry）→ UI cmd（`setExpiryTargets`，cpp:1300/4468/4882） |
| 显示 | `values0.targ_vega_tw` 存入 OptionValues | UI snapshot `expiries[].targDelta/targVega` |

**结论：语义对齐，现版通道更优**（config 初始化 + 热参热改 + UI 命令，原版只有运行时 property）。生产缺口是 targ 值全空（=0）——即"推向 0"退化语义，这是运营配置问题不是实现差距（v3 方案 §3.5 已含目标位语义说明与同源对齐）。

## 3. teenie —— ⚠️ 行为等价（option 侧均未启用），现版防御更好，但无可用实现

| 项 | 原版 | 现版 |
|----|------|------|
| OptionPricer | **注释禁用**（cc:171-207） | **stub 强制 OFF**（cpp:129-134，4.1 audit：stub 会冻结报价，构造时强制 false + LL_ERROR 报警） |
| 活跃实现位置 | 仅 `CompositeMMPricer`（cc:1481-1495）与 `CompositeCommPricer` | 无 |
| 原版语义（MMPricer） | `bid = min(bid, mkt_bid + 1 tick)`——bid 最多比市场 best 好 1 tick（排队优先不穿透）；**市场无 bid 时 return false（不报价）** | stub：`mkt.sz() < our_sz ? 0 : 1 tick`（量比较语义，未完成移植）且引用空 `PriceSize` → 若启用会冻结报价 |
| 现版 stub 死分支 | — | `updateOurMarketSide` cpp:1208-1218 引用恒空的 `mkt_bid`——因构造函数强制 OFF 不会走到（`m_config.enable_teenie_pricing` 已被置 false），安全 |

**结论：行为等价（option 做市两侧都不启用 teenie），现版的强制关闭是对未完成移植的正确防御。**

**可选增强（业务评估后决定）**：cu 期权排队竞争需要 teenie（挂 `mkt_best + 1 tick` 抢队列优先）时，按**原版 MMPricer 语义**重写——`bid ≤ mkt_bid + 1tick` 封顶、市场无 bid 跳过该侧——而不是修现版 stub。建议排入二期，且必须带 replay 验证（teenie 改变成交率结构）。

## 4. improve_retreat_ratio —— ✅ 完全对齐

- 原版唯一使用点是 sticky 的方向性比值（cc:1649-1650）；OtcPricer 中注释。
- 现版唯一使用点相同（cpp:747-748）。
- 默认 3.0 两侧一致，生产配置 3.0。
- 语义：BID 侧改善容忍带 3×sticky_pts（不轻易跟价上移）、撤退带 1×（下移更容易）——**撤退激进、改善保守**，做市防逆向选择的标准姿态。

**结论：对齐，无行动项。**

## 5. 汇总

| 项 | 对照结论 | 行动 |
|----|----------|------|
| sticky | ✅ 对齐（含默认值） | 无；risk shift 联动降权已在 v3 方案 |
| targ | ✅ 语义对齐，现版写入通道**更优** | 运营配置 targ 值（v3 §3.5） |
| teenie | ⚠️ 行为等价（双方 option 侧均禁用）；现版 stub 若启用更危险，已被正确防御 | 二期可选：按原版 MMPricer 语义重写 + replay 验证 |
| improve_retreat | ✅ 对齐 | 无 |

**总体判断：这四个机制不需要"向原版回移"——sticky/retreat 逐行对齐，targ 优于原版，teenie 的差异是"双方都禁用 + 现版防御更规范"。真正缺的仍是一个可用的 teenie 实现（可选）与 v3 方案的 risk shift 主链路。**
