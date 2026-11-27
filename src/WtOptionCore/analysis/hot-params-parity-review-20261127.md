# 热更新参数体系对比：当前项目 vs 源项目 quantbox（2026-11-27/28，三轮复核定稿）

> 背景：11-27 多层挂单/改价风暴排查中发现 14:14 有一组 UI 热调
> （`sprd_fwd:202701 0.02→1.0`、`sprd_atmvol:202701 0.15→0.05`，加上此前的
> `targ_vega:202701=+5000`），由此展开热更新参数体系的完整对照。
> 本文经过三轮复核，**两处初版结论被源码推翻**（见 §2），以此版为准。
> 源项目源码：`/mnt/d/gf_pc/WonderTrader/quantbox/`（quantbox/strategy/optioncore、
> quantbox/strategy/stratlib、optiontrader/）。

## 1. 源项目参数注册的四类语义（PropertyManager.h/.cc 实证）

| 注册 API | 可写 | SQLite 持久 | 启动恢复 | 源项目实例 |
|---|---|---|---|---|
| `registerProperty(p, true)` | ✔ | ✔ | ✔（`*p = getProperty(key, default)`） | per-expiry 11 个：sprd_fut / sprd_fwd / sprd_atmvol / sprd_corr、delta_min / delta_max、enable、enable_auto_close、max_pos_stk / max_pos_opt、max_qsize |
| `addProperty(p)`（= registerProperty 默认 persistent=true） | ✔ | ✔ | ✔ | per-expiry：targ.delta、targ.vega_tw、gvv_weight |
| `registerReadOnlyProperty(p)` | ✘ **仅展示** | 可选 | — | per-expiry：numcxl、numrej、numpos、numfil（报/撤/持仓/成交计数） |
| `registerAndResetPropertyOnNewDay(p, today, reset)` | ✔ | ✔ 跨日重置 | ✔ | 机制存在，CompositeOptionPricer 未使用 |

- **可写+持久合计 14 个 per-expiry 参数**。
- 未注册的 `m_prop_*`（策略层 panic_thresh1/2 等）：**不可热调、GUI 不可见**，纯启动配置。
- set 路径实证（PropertyManager.cc `onSetProperty`）：
  `command.properties.set.<key>` → lexical_cast → set_value → **立即 save(SQLite)** →
  变更通知（COP `onPropertyChange`：重算 risk shifts + pricingChangedEvent）。
  **全程无范围校验**（仅挡类型错误 bad_lexical_cast）。
- 持久化介质：`db::SQLitePropertyTable`（PropertyManager 构造传入 filename），
  **跨重启、跨交易日恢复**。

## 2. 复核中被推翻的初版结论（留档）

| 初版结论 | 源码实证 | 修正 |
|---|---|---|
| "源项目未见持久化机制" | PropertyManager 继承 SQLitePropertyTable；registerProperty(p, **true**) 第二参数即持久化标志 | 源项目有 SQLite 持久化 + 启动恢复，且 11+3 个参数全部持久 |
| "当前项目按 hot_params.json 的 tdate 跨日失效" | `replayHotParams`（HftOptionStrategy.cpp:4826-4870）**只解析 params、从不读 tdate**；唯一过滤是过期合约月剪枝（B2） | **当前项目实际也是跨日持久**——22:26（tdate=20261130）重启恢复了 14:14（tdate=20261127）的热调即为实证。tdate 是写入但从不校验的**死字段**（疑似未完成设计） |

## 3. 修正后的差异总表

| 维度 | 源项目 quantbox | 当前项目 | 定性 |
|---|---|---|---|
| per-expiry 可写+持久 | **14 个** | **6 个**（targ_delta / targ_vega / sprd_fwd / sprd_atmvol / sprd_corr / volcurve_weight） | **缺 8 个**：delta_min / delta_max、enable_auto_close、max_pos_stk / max_pos_opt、max_qsize、sprd_fut（enable 有 EXPIRYENABLE 命令替代） |
| 持久化介质 / 语义 | SQLite，跨日持久 | hot_params.json，**实际跨日持久**（tdate 死字段） | 语义一致；当前项目需对 tdate 做"接失效判断或删字段"的二选一决策 |
| 过期处理 | 无（靠 active_expiries 配置收敛） | 过期合约月键 replay 时自动剪枝 | 当前项目更优 |
| 只读展示 | numcxl / numrej / numpos / numfil | UI 快照等价字段 | 一致 |
| 范围校验 | **无** | 有（mk / mkExp 的 [lo,hi] clamp） | 当前项目更优，**但 `mkExp("sprd_fwd", 0, 1)` 上限与 cu 量纲冲突**（见 §4） |
| 变更联动 | changeEvent → 重算 risk shifts + pricingChangedEvent | applyHotParams 批处理 + UI 热参数面板 | 等价 |
| 全局参数热调 | 策略层 m_prop_* 未注册（不可热调）；COP 全局 notifiable 部分注册 | 11 个全局 mk（volcurve_weight、sticky_base、improve_retreat、trade_shock_ticks、risk_shift_*×6、max_tps）；**alpha 权重刻意不可热调**（既定要求） | 当前项目热调面更宽（risk_shift_* 为本项目 v3 新增，源项目无对应物——其 L3 是注释掉的死代码） |

## 4. 关键参数的量纲语义差异（风暴的参数侧根子）

源项目配置样例（`optiontrader/examples/000300/config.lua`，注释为原文）：

```lua
-- detla_min delta_max sprd_fut sprd_fwd sprd_atmvol sprd_corr
{0.1, 0.9, 0.06, 0.10, 0.1232, 0.8, ...}
-- sprd_fut    买卖价差 最小保证盈利的价差   ← 单位：元（000300 合成 fwd 实际价差 ≈0.10 元）
-- sprd_atmvol 平直vol 最小保证盈利的差价    ← 单位：vol 点
```

代入 `core_spread = √(δ²·s_f² + ν²·s_v² + 2δs_f·νs_v·corr)/2 + fees`
（cu：F≈89000、tick=10 元、ATM vega≈107 元/vol 点）：

| 参数 | 源项目语义 | 当前 cu 配置 | 量纲核查 |
|---|---|---|---|
| sprd_fwd | forward **实际买卖价差（元）** | 0.02（14:14 热调至 1.0=clamp 上限） | ❌ cu 期货 tick=10 元，合成 fwd 价差量级 ~10 元——0.02 小约 500 倍，core_spread 的 delta 项（δ×s_f≈0.01 元）形同虚设；热调上限 1 使**正确值永远调不进、无意义值（1）却合法** |
| sprd_fut | 期货买卖价差（元），000300=0.06 | **无配置项，硬编码 100.0**（ExpiryRiskConfig::init） | ⚠ 不可配置、不可热调、未经 cu 校准 |
| sprd_atmvol | vol 价差（vol 点） | 0.15（14:14 热调至 0.05） | ✔ 量纲一致；0.05 使 ATM 半价差 9 元→3.7 元（≈1.8 tick），**过窄**——11-27 改价风暴的初始推动之一 |

## 5. 与 11-27 风暴的关联（详见当日多层挂单分析）

- 14:12–14:14 的一组热调（targ_vega=+5000、sprd_atmvol 0.05、sprd_fwd=1）方向一致：
  **收窄价差促成交、加速买入回补 vega 空头**——运营意图可理解；
- 副作用：报价贴 theo（1.8 tick）→ 趋势日（cu +3.4%）改价率/成交率激增 →
  叠加 CTP 撤单流控丢失 → 撤单重试正反馈 → 风暴；
- **缺失的护栏**：①量纲合理性提示（sprd_fwd 上限 1 锁死正确量纲）；
  ②热调无行情状态联动告警；③操作审计（hot_params.json 无 setter/时间戳，
  追溯靠日志 grep）。

## 6. 建议清单（三轮复核后的最终版，均未实施）

| # | 项 | 说明 |
|---|---|---|
| 1 | sprd_fwd 量纲修复 | cu 按源项目语义应 ~10（元）；`mkExp("sprd_fwd", 0, 1)` 上限按品种放宽（如 0~50） |
| 2 | sprd_fut 配置化 + 热调 | 消除 100.0 硬编码，对齐源项目 per-expiry sprd_fut |
| 3 | 补齐 8 个 per-expiry 热调缺口 | 优先 max_qsize（做市义务手数运营需要）、delta_min/max |
| 4 | tdate 死字段决策 | 二选一：接跨日失效判断（targ 类日内参数更适合，需配套开盘前参数检查清单）或删字段；现状（写了不校验）最误导 |
| 5 | 热调量纲告警 | sprd_fwd < 1 tick、sprd_atmvol 收窄超 50% 等触发 WARN + UI 提示 |
| 6 | 热调操作审计 | hot_params.json 增加 setter/时间戳字段 |
| 7 | （联动项）cancel_max_retries 3→1、wait_for_cancels 回滚 false | 11-27 深夜实证：wait_for_cancels 在确认丢失 ~50% 环境下使撤单流量 ×4 适得其反；降重试才是给流控解压的正确方向 |

## 7. 证据索引

- 源项目：`quantbox/strategy/stratlib/PropertyManager.h`（四类注册 API）、
  `PropertyManager.cc`（onSetProperty 无校验）、
  `optioncore/CompositeOptionPricer.cc:369-401`（11 个 registerProperty + 4 个
  registerReadOnlyProperty）、`:443-450`（targ/gvv_weight addProperty）、
  `optiontrader/examples/000300/config.lua:82-96`（量纲样例与中文注释）；
- 当前项目：`HftOptionStrategy.cpp:4686-4731`（mk/mkExp 注册表与范围）、
  `:4779-4870`（save/replayHotParams，tdate 死字段）、
  `CompositeOptionPricer.cpp:725-745`（core_spread 公式）、
  生产 `generated/hot_params.json`（6 参数实证）、
  `Logs/Runner_2026-11-27.log` 14:12–14:14 UI SETPARAM 记录。
