# WtFutuCore 报价价差 / Skew 链路交接文档

> 目的：记录从信号到最终 bid/ask 的完整链路、每个环节的计算公式、影响变量与配置来源，
> 便于后续单独立项优化 skew / baseSpread / spread_mult / RiskWiden。
> 代码位置：`src/WtFutuCore/SpreadOptimizer.cpp`、`signals` 已移至顶层 `SpreadOptimizer.h`、
> `QuotePolicyChain.h`、`StrategyCoordinator.cpp`、`FutuRiskMonitor.cpp`。

## 1. 总体数据流

```
行情 tick
  └─ checkPreTradePosition (风控裁决 + 带符号仓位利用率 contract_pos_util)
       └─ SpreadOptimizer::computeOptimalQuote
            ├─ computeBaseSpread        -> base_spread (tick 数)
            ├─ spread_mult (毒性/低置信/EMA) -> base_spread *= spread_mult
            ├─ alpha -> fair_value
            ├─ delta skew -> inventory_skew (tick 数)
            └─ bid/ask = fair ± half_spread + skew
       └─ QuotePolicyChain (顺序执行)
            RiskWiden -> ArbCloseSync -> Toxicity -> LimitPrice -> ColdStart -> FillRetreat
       └─ FutuQuoter (sticky / 义务层 / 撤单重挂) -> 交易所
```

## 2. computeBaseSpread（基础价差，单位 tick）

位置：`SpreadOptimizer::computeBaseSpread`。

```text
avg_depth = (bid_depth + ask_depth) / 2

depth_adj =
    avg_depth <= 0 ? no_depth_spread_mult
                   : 1 / (1 + (avg_depth / depth_normalization)
                                * depth_sensitivity * depth_sensitivity_scale)

spread = base_spread * depth_adj

if volatility valid:
    sigma_sq = vol_percentile / vol_percentile_scale
    spread += phi * sigma_sq * vol_scale

spread = clamp(spread, base_spread*min_spread_mult, base_spread*max_spread_mult)
```

### 影响变量与配置来源

| 变量 | 含义 | 默认 | 来源 |
|---|---|---|---|
| `base_spread` | 基础价差（tick） | 2.0 | `config.yaml quoting.baseSpread` |
| `depth_sensitivity` | 深度敏感度 | 0.5 | `coordinator.yaml modules.spreadOptimizer.depthSensitivity` |
| `depth_sensitivity_scale` | 深度敏感度缩放 | 0.2 | `...spreadOptimizer.depthSensitivityScale` |
| `depth_normalization` | 深度归一化基数 | 100.0 | `...spreadOptimizer.depthNormalization` |
| `no_depth_spread_mult` | 无深度数据时倍数 | 1.5 | `...spreadOptimizer.noDepthSpreadMult` |
| `phi` | 波动率惩罚系数 | 0.20 | `...spreadOptimizer.phi` |
| `vol_percentile` | 波动率分位（运行时信号） | — | SignalContext.volatility |
| `vol_percentile_scale` | 波动率分位归一化分母 | 50.0 | `...spreadOptimizer.volPercentileScale` |
| `vol_scale` | 波动率缩放 | 5.0 | `...spreadOptimizer.volScale` |
| `min_spread_mult` | 价差下界倍数 | 1.0 | `...spreadOptimizer.minSpreadMult` |
| `max_spread_mult` | 价差上界倍数 | 3.0 | `...spreadOptimizer.maxSpreadMult` |

注意：`base_spread` 同时被 `SpreadOptimizer` 用作定价核心，也被 `FutuQuoter` 用作
fallback；`hotparams.yaml base_spread` 是运行时热更覆盖，重启后以 config.yaml 为准。

## 3. spread_mult（价差乘子）

位置：`SpreadOptimizer::computeOptimalQuote` 第 2 段。

```text
// 2a 毒性
if toxic_detected && toxicity_score > 0.05:
    tox_mult = 1 + toxicity_score * toxicity_spread_factor
    spread_mult *= tox_mult

// 2b 低置信度
if alpha valid && confidence < low_confidence_threshold:
    low_conf_mult = 1 + (low_confidence_threshold - confidence)
                          / low_confidence_threshold
                          * low_confidence_spread_factor
    spread_mult *= low_conf_mult

// 2c EMA 平滑
rising(风险时):  alpha = 0.30
decay(无风险):   alpha = 0.50，并额外 + 0.05 * (1 - smoothed) 向 1.0 回拉

// 变化率限制
每 tick 上行最多 +10%，下行最多 -15%

result.base_spread *= spread_mult
```

### 影响变量

| 变量 | 含义 | 默认 | 来源 |
|---|---|---|---|
| `toxicity_score` | 综合毒性分 | 运行时 | ToxicFlowDetector |
| `toxicity_spread_factor` | 毒性扩价系数 | 1.0 | `...spreadOptimizer.toxicitySpreadFactor` |
| `confidence` | alpha 置信度 | 运行时 | SignalContext.alpha.confidence |
| `low_confidence_threshold` | 低置信阈值 | 0.3 | `...spreadOptimizer.lowConfidenceThreshold` |
| `low_confidence_spread_factor` | 低置信扩价系数 | 2.0 | `...spreadOptimizer.lowConfidenceSpreadFactor` |
| EMA 常数 | 0.30 / 0.50 / 0.05 | 硬编码 | 代码内 constexpr |
| 变化率限制 | +0.10 / -0.15 | 硬编码 | 代码内 constexpr |

注意：`low_confidence_spread_factor` 头文件字段注释写的是 `default 0.8`，但构造函数与
`fromVariant` 实际默认都是 **2.0**（M8 注释说明已统一）。注释是过期的，需修。

## 4. skew（delta / 库存偏斜，单位 tick）

位置：`SpreadOptimizer::computeOptimalQuote` 第 4 段 + 三个 skew 计算函数。

### 4.1 带符号仓位利用率（skew 的输入）

位置：`StrategyCoordinator::processQuoting`。

```text
projected_long  = (position > 0 ? position : 0) + pending_buy
projected_short = (position < 0 ? |position| : 0) + pending_sell
long_util  = projected_long  / max_position
short_util = projected_short / max_position

contract_pos_util = (long_util >= short_util) ? long_util : -short_util
                     // 带符号，正=净多，负=净空，取较大侧
```

### 4.2 单合约 skew（当前主口径）

```text
computeContractPosSkew(signed_pos_util, half_spread_ticks, cross_max_ticks):
    util = |signed_pos_util|
    direction = signed_pos_util > 0 ? -1 : +1   // 净多 -> 负 skew(卖侧贴), 净空 -> 正 skew(买侧贴)
    norm = util^delta_skew_power * inventory_skew_gain
    cap = (util >= 1.0 && cross_max_ticks > 0)
              ? 1.0 + cross_max_ticks / half_spread_ticks
              : 1.0
    norm = min(norm, cap)
    return direction * norm * half_spread_ticks
```

### 4.3 组合 Delta skew

```text
computePortfolioDeltaSkew(totalDelta):
    util = |totalDelta| / portfolio_max_delta
    if util <= delta_skew_threshold: return 0
    excess = util - delta_skew_threshold
    direction = totalDelta > 0 ? -1 : +1
    return direction * delta_skew_factor * excess^delta_skew_power
```

### 4.4 legacy 单合约 Delta skew（未注入 pos_util 时的回退）

```text
computeContractDeltaSkew(contractDelta, contractMaxDelta):
    utilization = contractDelta / contractMaxDelta
    direction = utilization > 0 ? -1 : +1
    return direction * |utilization|^delta_skew_power * inventory_skew_scale
```

### 4.5 合并与钳制

```text
delta_skew = portfolio_skew_weight * portfolio_skew
           + contract_skew_weight  * contract_skew

cross_authorized = |contract_pos_util| >= 1.0
clamp_limit = cross_authorized ? half_spread + skew_cross_max_ticks : half_spread
total_skew   = clamp(delta_skew, -clamp_limit, +clamp_limit)
inventory_skew = total_skew
```

### skew 影响变量汇总

| 变量 | 含义 | 默认 | 来源 |
|---|---|---|---|
| `position` / `pending_buy/sell` / `max_position` | 策略簿记净仓、同向在途、单合约上限 | — | FutuPortfolio / UnifiedOrderTracker / config.yaml contracts |
| `delta_skew_power` | skew 非线性幂次 | 1.5 | `...spreadOptimizer.deltaSkewPower` |
| `inventory_skew_gain` | 归一化库存 skew 增益 | 1.0 | `...spreadOptimizer.inventorySkewGain` |
| `inventory_skew_scale` | legacy delta 路径放大 | 2.0 | `...spreadOptimizer.inventorySkewScale` |
| `skew_cross_max_ticks` | util≥1 时穿越 mid 最大 tick | 3.0 | `...spreadOptimizer.skewCrossMaxTicks` |
| `portfolio_max_delta` | 组合 delta 软限 | config portfolio.maxDelta | config.yaml portfolio.maxDelta（hotparams max_delta 运行时覆盖） |
| `delta_skew_threshold` | 组合 skew 触发阈值 | 0.3 | `...spreadOptimizer.deltaSkewThreshold` |
| `delta_skew_factor` | 组合 skew 强度 | 1.5 | `...spreadOptimizer.deltaSkewFactor` |
| `portfolio_skew_weight` | 组合 skew 权重 | 0.5 | `...spreadOptimizer.portfolioSkewWeight` |
| `contract_skew_weight` | 单合约 skew 权重 | 1.0 | `...spreadOptimizer.contractSkewWeight` |
| `contract_max_delta` | 单合约 delta 软限 | config contracts[].maxDelta | config.yaml contracts[].maxDelta |

## 5. fair value 与最终 bid/ask

```text
half_spread_price = (base_spread / 2) * tick_size

alpha_adjustment = alphaSensitivity * alpha * confidence_weight * tick_size
confidence_weight = confidence_weight_min
                  + (confidence_weight_max - confidence_weight_min) * confidence
alpha_adjustment = clamp(alpha_adjustment, -half_spread_price, +half_spread_price)
fair_value = mid + alpha_adjustment

skew_price = total_skew * tick_size * spread_mult
skew_limit = (half_spread_price + (cross_authorized ? skew_cross_max_ticks*tick_size : 0))
             * spread_mult
skew_price = clamp(skew_price, -skew_limit, +skew_limit)

bid = floor((fair_value - half_spread_price + skew_price) / tick_size) * tick_size
ask = ceil ((fair_value + half_spread_price + skew_price) / tick_size) * tick_size
```

若 `bid >= ask`，触发 crossed-quote 保护，回退为对称报价并置 `pause_quoting`。

### 影响变量

| 变量 | 含义 | 默认 | 来源 |
|---|---|---|---|
| `alphaSensitivity` | alpha 灵敏度 | 2.0 | `coordinator.yaml pipeline.alphaSensitivity`（hotparams alpha_sensitivity 可热更） |
| `alpha` / `confidence` | 信号值 / 置信度 | 运行时 | SignalContext.alpha |
| `confidence_weight_min/max` | 置信度权重区间 | 0.2 / 1.0 | `...spreadOptimizer.confidenceWeightMin/Max` |
| `tick_size` | 最小变动价位 | 合约 | 基础数据 / createSpreadOptimizer 参数 |
| `total_skew` | 上一步合并后的 skew | — | 见第 4 节 |

## 6. RiskWiden（风控对价差的二次拉宽）

位置：`QuotePolicyChain.h::RiskWidenPolicy`。

```text
// soft（软风控，无状态，每 tick 覆盖）
tickSoft(cur_util, l1, l2):
    target = cur_util >= l2 ? 1.5
           : cur_util >= l1 ? 1.2
           : 1.0

// hard（违规触发，max 闩锁）
onHardWiden(cur_util, l2):
    target = cur_util >= l2 ? 1.5 : 1.2
    _mult = max(_mult, target)   // CAS 保证不回退

// apply（报价链末端，绕中心对称拉宽，不改变 skew 方向）
st.spread_mult *= _mult
center     = (l0_bid + l0_ask) / 2
half_width = (l0_ask - l0_bid) / 2 * _mult
l0_bid = floor(center - half_width)
l0_ask = ceil (center + half_width)
```

### 触发条件与变量

- soft：`FutuRiskMonitor::checkSoftLimits`，当组合 delta 利用率
  `absDelta / portfolio_max_delta >= position_warning_l1` 触发 WIDEN_SPREAD。
  - `l1 = position_warning_l1`（默认 0.8）→ ×1.2
  - `l2 = position_warning_l2`（默认 0.9）→ ×1.5
- hard：风险 violation（如 EXPOSURE/日亏）触发 WIDEN_SPREAD，按 `widen_threshold`
  （breachCount，默认 1）进入 hard 闩锁。

| 变量 | 默认 | 来源 |
|---|---|---|
| `position_warning_l1` | 0.8 | `config.yaml risk.frequency.positionWarningL1` |
| `position_warning_l2` | 0.9 | `config.yaml risk.frequency.positionWarningL2` |
| `widen_threshold` | 1 | `config.yaml risk.frequency.widenThreshold` |
| `portfolio_max_delta` | config | `config.yaml portfolio.maxDelta` |

## 7. 完整报价链顺序（一次 tick）

1. `checkPreTradePosition`：算 `contract_pos_util`、`long/short_util`、`halt_quoting`、
   `side_pause`、`block_add`、`force_*_obligation`。
2. `computeOptimalQuote`：base_spread → spread_mult → fair_value → skew → bid/ask。
3. `QuotePolicyChain.run`：
   RiskWiden → ArbCloseSync → Toxicity → LimitPrice → ColdStart → FillRetreat。
4. `FutuQuoter::refreshQuotes`：风控闸门（halt/side_pause）→ 义务/自由层 → sticky → 撤单重挂。

## 8. 后续优化切入点（handoff 目标）

1. **skew 灵敏度**：`delta_skew_power / inventory_skew_gain / inventory_skew_scale /
   delta_skew_threshold / delta_skew_factor / portfolio_skew_weight / contract_skew_weight /
   skew_cross_max_ticks`。中低持仓时 skew 很小（util^1.5），减仓侧贴 mid 的速度偏慢。
2. **baseSpread 与深度/波动率**：`base_spread / depth_sensitivity / depth_normalization /
   no_depth_spread_mult / phi / vol_scale / vol_percentile_scale`。夜盘薄盘时深度调整接近 1，
   最终宽度主要被 spread_mult 和 RiskWiden 推高。
3. **低置信度治理**：`low_confidence_threshold / low_confidence_spread_factor`。当前 conf
   常贴 0.3 阈值，导致 spread_mult 常驻 1.5–2.0。
4. **RiskWiden 幅度**：`position_warning_l1/l2`（×1.2/×1.5）与 `widen_threshold`。
5. **spread_mult EMA/限速常数**：0.30 / 0.50 / 0.05 / +0.10 / -0.15，目前硬编码。
6. **最终宽度硬上限缺失**：当前是 base × mult × RiskWiden 复合放大，没有总宽 clamp，
   可考虑新增 `maxQuoteSpreadTicks`。

## 9. 已知文档/注释不一致

- `SpreadOptimizer.h` 中 `low_confidence_spread_factor` 字段注释写 `default 0.8`，
  但构造与 `fromVariant` 默认都是 2.0。
- `GLFTParams` 构造默认 `base_spread=2.0 / tick_size=0.2`，但实际由
  `createSpreadOptimizer` 用 config.yaml 与合约 tick 覆盖，默认值仅兜底。
