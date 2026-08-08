# WtFutuCore 工作规则

## 1. 先规划后执行（不可边思考边修改）

任何修改必须严格按以下顺序进行：

1. **查原因**：先用只读手段（日志、代码、数据）定位根因，给出证据链
2. **设计方案**：从业务逻辑出发给出方案，列出备选与取舍
3. **制定计划**：分文件/分步骤的修改清单 + 验证方法
4. **确认后修复**：用户确认计划后再动手改代码

禁止在调查/思考过程中直接改代码。禁止"改一下试试"式的试错修复。

## 2. 修改范围限制

- **源码修改仅限 `src/WtFutuCore/` 目录内的文件**
- 不得修改框架/库文件（WtBtCore、WtUftCore、WtCore、Share、Includes、WTSTools 等）
- `dist/WtBtFutu/` 是回测部署目标：二进制与配置按既定流程从 src 构建/拷贝，
  运行副本的调参（如 `_ec_5d.yaml`）允许，但配置的权威来源是 `src/WtFutuCore/config/`
- 发现框架层缺陷时：记录在本文件"已知外部限制"，不得越界修复

## 3. 构建与部署

```bash
cd src/build_all && make -j$(nproc) WtFutuCore
cp src/build_all/build_x64/Debug/bin/WtUftRunner/futu/libWtFutuCore.so dist/WtBtFutu/uft/
cp src/WtFutuCore/config/coordinator.yaml dist/WtBtFutu/
```

## 4. 回测

```bash
cd dist/WtBtFutu
LD_LIBRARY_PATH=./uft:$LD_LIBRARY_PATH timeout 900 ./uft/WtBtRunner -c <config.yaml> -l logcfgbt.yaml < /dev/null
```

- 策略日志: `outputs/Strategy_uft.log`，Runner 日志: `outputs/Runner.log`
- 成交/资金: `outputs_bt/uft/{trades,funds,positions,closes}.csv`
- 回测 coordinator 必须 `useAsyncArbThread: false`（主线程同步）

## 已知外部限制（框架层，禁止越界修复）

- **WtBtCore/HftMocker.cpp 回测不可复现**：`splitVolume()` 用 `srand(time(NULL))`
  随机拆分成交量、`genRand()` 用 `srand(getCurMin())`、`makeLocalOrderID()` 用墙钟播种
  → 同配置两次运行成交拆分序列不同，策略轨迹有随机性（幅度随交易频率放大）。
  评估策略表现需接受该噪声或多次运行取均值。
- `AsyncArbitrageExecutor` orphan leg 超时用 `steady_clock`（罕见路径，同理不可复现）。

## 框架层已打补丁（越界修改记录，2026-08-03，GUI 监控接入需要）

- **`src/WtUftRunner/WtUftRunner.cpp`**：`config()` 尾部补 `initEvtNotifier()` 调用。
  原因：框架缺陷——`initEvtNotifier()` 定义存在但从未被调用（对比 WtRunner.cpp:241 /
  WtRtRunner.cpp:654 均有调用），导致 config.yaml 的 `notifier` 段被静默忽略，
  EventNotifier 不发布任何 MQ 事件，WtMonSvr GUI 无法接收实时订单/成交/日志。
  上游修复后可还原。重建流程：`dist/WtRunnerFutu/rebuild_release.sh`。
- **`dist/WtRunnerFutu/libWtMsgQue.so`**：EventNotifier 按 CWD 优先查找 MQ 模块，
  需将该库置于 runner 工作目录（从 dist/bin 拷贝）。
