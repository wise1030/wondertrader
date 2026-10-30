# WtOptionCore 阶段 10：AsyncVolFitter 与版本化曲线发布

- **日期**: 2026-10-30
- **范围**: immutable fit snapshot、single-flight 后台拟合、worker batch 边界版本化发布
- **验证**: 主构建 `src/build_all` 编译通过；WtOptionCore 7/7 CTest 通过

---

## 已实现变更

### 1. 新增 AsyncVolFitter

文件：

- `AsyncVolFitter.h`（新增）
- `CMakeLists.txt`

核心语义：

1. worker 在 timer/batch 边界构造 `VolFitExpirySnapshot`：
   - expiry
   - forward
   - maturity
   - ATM vol
   - fit points `{K-F, iv/atmVol}`
   - version
2. snapshot 提交给 `SingleFlightTaskRunner`；
3. 后台线程基于 snapshot 拟合新 curve；
4. worker 在后续 `on_batch_complete` 中调用 `tryPublish()`；
5. `OptionPricer2::publishFittedCurves()` 仅在版本更新时替换 curve。

同名任务 single-flight，重复 submit 返回 false。

### 2. OptionPricer2 版本化发布接口

文件：

- `OptionPricer2.h/.cpp`

新增：

```cpp
struct FittedCurveSet {
    expiry;
    volCurve;
    volCurve2;
    atmVol;
    version;
}

publishFittedCurves(...)
getCurveVersion(exp)
```

规则：

- version 为 0 忽略；
- 旧版本忽略；
- 发布时同步 curve 的 ATM forward / maturity / ATM vol；
- worker 是唯一发布写者。

### 3. Hft 接入

文件：

- `HftOptionStrategy.h/.cpp`

变更：

- `OptionPricer2Config::enable_fitter = false`
  - 停用旧同步 `PeriodicCurveFitter`；
  - 避免旧 fitter 与 AsyncVolFitter 双写同一曲线；
- timer 路径不再同步 `triggerDoFit()`；
- 新增：
  - `submitAsyncVolFit()`
  - `publishAsyncVolFit()`
- 后台拟合失败 GVV 时回退 Power 曲线，保证版本化 async 路径可发布；GVV 数值稳定性后续专项调优。

### 4. 并发隔离

- 后台 fit 只读取提交时拷贝的 `VolFitExpirySnapshot`；
- 不访问 OptionGrid / OptionData；
- curve 对象在后台新建；
- worker 后续通过版本化 publish 替换旧 curve；
- 避免 fit 线程直接写共享 Grid / ExpiryData。

---

## 新增测试

### `test_async_vol_fitter_publish`

覆盖：

- snapshot submit；
- single-flight 拒绝重复 submit；
- 后台完成拟合；
- worker 发布一次后结果消费；
- `OptionPricer2::getCurveVersion()` 更新；
- 发布后的 curve 可求值且有限。

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

- 该版本迁移了 curve fit，不再在 worker 中同步执行 `PeriodicCurveFitter::doFit()`；
- 全链 IV 重算仍在 pricing 主路径中；
- GVV 对某些近平坦 snapshot 可能数值拟合失败，当前回退 Power；
- 后续需补：
  - GVV 数值稳定性专项；
  - FAST 增量定价；
  - Vega O(N×M) 优化；
  - replay benchmark。

---

## 尚未完成

1. FAST 增量定价。
2. Vega O(N×M) 优化。
3. Scanner/Gamma/Power 语义完整实现。
4. 端到端 replay 延迟基准。
