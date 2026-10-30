# WtOptionCore 阶段 9：Fit 慢线程化前置基础设施

- **日期**: 2026-10-30
- **范围**: single-flight 慢任务执行器与并发隔离测试
- **验证**: 主构建 `src/build_all` 编译通过；WtOptionCore 7/7 CTest 通过

---

## 本轮完成内容

### 1. 新增 SingleFlightTaskRunner

文件：

- `SingleFlightTaskRunner.h`（新增）
- `CMakeLists.txt`

能力：

- 后台串行执行慢任务；
- 同名任务 single-flight，重复 submit 返回 false；
- 捕获任务异常，不终止后台线程；
- `stop()` 排空当前队列后退出；
- 后续 vol fit / stress / counter dump 的异步化都应以该执行器为载体。

### 2. 未直接异步化 PeriodicCurveFitter 的原因

当前 `PeriodicCurveFitter`：

- 直接遍历 `OptionGrid`；
- 调用 `OptionPricer2::getVolCurve()`；
- 拟合过程中原地修改 curve；
- 通过 `ExpiryData::setFitReady()` 写回数据层。

如果简单把现有 `triggerDoFit()` 放到后台线程，会产生以下共享可变状态竞争：

- worker 正在 pricing / refresh；
- fit 线程正在修改同一批 curve；
- scanner / risk / attribute 同时读取 Greeks 或 curve；
- `ExpiryData` 被并发写。

因此本轮没有伪造“已安全异步化”，而是先落地 single-flight 执行器与并发隔离测试。真正的 fit 慢线程化必须在下一步补齐：

1. worker 在 batch 边界采集不可变 fit snapshot；
2. fit 线程只处理 snapshot；
3. 产出新 curve；
4. worker 在下一 batch 边界原子发布 curve / fitReady。

---

## 新增测试

### `test_single_flight_task_runner`

覆盖：

- 同名任务重复 submit 被拒绝；
- 后台任务不会并发重入；
- `maxRunning == 1`。

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

1. 基于 immutable snapshot 的 AsyncVolFitter。
2. curve 版本化发布接口。
3. FAST 增量定价。
4. Vega O(N×M) 优化。
5. Scanner/Gamma/Power 语义完整实现。
6. 端到端 replay 延迟基准。
