# WtOptionCore 阶段 11：FAST 二阶增量与 Vega 热点预取

- **日期**: 2026-10-30
- **范围**: FAST gamma 修正 / forward 大步回退；Vega ATM 与 expiry 标量预取
- **验证**: 主构建 `src/build_all` 编译通过；WtOptionCore 7/7 CTest 通过

---

## 已实现变更

### 1. FAST 增量定价二阶修正

文件：

- `OptionPricer2.h/.cpp`
- `HftOptionStrategy.cpp`
- `ConfigValidator.h`

旧 FAST 更新只使用一阶 delta：

```text
theo' = theo + delta * dF
delta' = delta + gamma * dF
```

现在默认加入二阶 gamma：

```text
theo' = theo + delta * dF + 0.5 * gamma * dF²
delta' = delta + gamma * dF
```

新增配置：

- `fast_use_gamma_correction`
  - 默认 true
- `fast_full_reprice_forward_step`
  - 默认 0，表示禁用
  - 大于 0 时，若 `abs(dF)` 超过阈值则回退完整 Black `computeValue()`

### 2. FAST 数值回归测试

新增：

- `test_fast_incremental_gamma_correction`

用 BlackCalc 对比：

- 一阶近似
- 二阶 gamma 近似
- 新 forward 下完整 Black

断言：

- 二阶误差小于一阶误差；
- 二阶误差在设定阈值内。

### 3. Vega 热点第一轮安全优化

文件：

- `CompositeOptionPricer.cpp`

原先 `updateRiskShiftsVega()` 中每个 option 都：

- 调 `getAtmStrike()` 线性查找 ATM；
- 从 `ExpiryData` 查 `ExpireGreeksFrac()`；
- 从 map 查 `vega_tw_target`；
- 对每个 option 再循环所有 expiry 做 target 扣减。

现在改为每次 `updateRiskShiftsVega()` 预计算：

- `atmVegaCache[expiry]`
- `expiryFracCache[expiry]`
- `expiryTargetCache[expiry]`

循环内只做 O(1) 查表。

说明：

- 这一改动消除了每 option 的 ATM strike-vector 扫描和重复 map/expiry 查询；
- 持仓相关项仍保留原公式，未做近似；
- 完整 O(options × positions) 算法级重构仍需后续进行。

---

## 验证

构建：

```bash
cmake --build src/build_all \
  --target WtOptionCore \
  test_ctg_ranking test_enhancements test_scanners test_stress \
  test_hedge_coordinator test_command_handlers test_async_event_processor \
  -j4
```

测试：

```bash
ctest --test-dir src/build_all/WtOptionCore --output-on-failure
```

结果：

```text
7/7 tests passed
100% tests passed, 0 tests failed
```

`git diff --check -- src/WtOptionCore` 通过。

---

## 尚未完成

1. Vega O(options × positions) 算法级预聚合。
2. Scanner/Gamma/Power 语义完整实现。
3. 端到端 replay 延迟基准。
