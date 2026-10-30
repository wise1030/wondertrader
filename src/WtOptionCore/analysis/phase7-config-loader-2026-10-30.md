# WtOptionCore 阶段 7：Typed ConfigLoader 与 Effective-Value Dump

- **日期**: 2026-10-30
- **范围**: typed config loader、关键配置范围校验、legacy key 警告、effective-value dump
- **验证**: 主构建 `src/build_all` 编译通过；WtOptionCore 7/7 CTest 通过

---

## 已实现变更

### 1. 新增 OptionConfigLoader

文件：

- `OptionConfigLoader.h/.cpp`（新增）
- `CMakeLists.txt`

提供：

```cpp
OptionConfigLoader::Result load(WTSVariant* cfg)
OptionConfigLoader::dumpEffectiveValues(result)
```

`Result` 包含：

- `ok`
- `values`
- `errors`
- `warnings`
- `effective`

当前 typed values 覆盖报告中最容易漂移、且会直接影响交易正确性的关键项：

- `riskFreeRate`
- `maxTPS`
- `minComputeInterval`
- `marketFreshTimeoutMs`
- `pricer.slow_compute_interval`

### 2. 配置语义修正

#### 显式零利率不再被覆盖

旧逻辑：

```cpp
if (_riskFreeRate == 0.0) _riskFreeRate = 0.03;
```

会把合法配置 `riskFreeRate=0` 改成 3%。

现在：

- 未配置：默认 3%；
- 显式配置 0：保留 0；
- 负数 / 非有限值：启动失败。

#### slow_compute_interval 真正进入 CompositePricer

旧 full config 配置了：

```json
pricer.slow_compute_interval = 0.1
```

但 loader 未传给 `CompositeOptionPricerConfig`，实际使用默认 15 秒。

现在：

- `OptionConfigLoader::Values::slowComputeInterval`
- `setupPricer()` 显式赋给 `copCfg.slow_compute_interval`

### 3. Legacy OQM key 警告

以下旧 key 仍然会被 schema 接受但当前 loader 不消费，现在会产生 warning：

- `orderManager.option.cancel_timeout_ms`
  - 应使用 `cancel_ack_timeout_ms`
- `orderManager.option.ttl_ms`
  - 应使用 `time_in_force_ms`
- `orderManager.option.max_orders_per_code`
  - 应使用 `max_side_orders`
- future 同名 legacy key 同样处理

### 4. 范围校验

已实现：

- required:
  - `exchange`
  - `futuresProduct`
- `riskFreeRate >= 0`
- `maxTPS >= 0`
- `minComputeInterval >= 0`
- `marketFreshTimeoutMs >= 0`
- `pricer.slow_compute_interval >= 0`
- `expiries[].expiry != 0`
- `0 <= delta_min < delta_max <= 1`

核心配置无效时 `HftOptionStrategy::init()` 返回 `false`。

### 5. Effective-value dump

`HftOptionStrategy::init()` 现在在 ConfigValidator 前输出：

```text
effective config values:
  exchange = ...
  futuresProduct = ...
  riskFreeRate = ...
  maxTPS = ...
  minComputeInterval = ...
  marketFreshTimeoutMs = ...
  pricer.slow_compute_interval = ...
  expiries[...].delta_min = ...
  expiries[...].delta_max = ...
```

用途：

- 证明 key 被实际消费；
- 证明默认值/迁移后的实际值；
- 避免“schema 合法但 loader 未使用”的假信心。

### 6. ConfigValidator 修正

- 新增 `sessionSchedule.enabled`，避免 full config 误报。

---

## 新增测试

### `test_option_config_loader`

覆盖：

- 显式 `riskFreeRate=0` 保持为 0；
- `maxTPS=0` 迁移为 effective 50；
- `marketFreshTimeoutMs` 生效；
- `pricer.slow_compute_interval` 生效；
- legacy `cancel_timeout_ms` 产生 warning；
- effective dump 包含关键值；
- 负利率导致 `ok=false` 并产生 error。

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

## 当前边界

本轮先覆盖审查中确认发生漂移且高风险的关键配置。完整 typed schema 仍保留后续范围：

- 所有 OQM/filter/scanner/gamma 字段逐项迁移；
- schema、默认值、loader、effective dump 由同一字段描述生成；
- 配置值与运行时对象行为的全量断言测试。

---

## 尚未完成

1. worker 减负与延迟基准。
2. Scanner/Gamma/Power 语义完整实现。
