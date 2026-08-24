# WonderTrader —— C++17 量化交易框架（含 WtFutuCore 期货高频做市引擎）

## 一、整体架构

WonderTrader 采用"核心引擎 + 策略工厂 + 网关适配 + 运行器宿主"四层结构，全部源码位于 `src/`（CMake 根：`src/CMakeLists.txt`）：

```
运行器(WtRunner/WtUftRunner/…) → 核心引擎(WtCore/WtUftCore/WtBtCore) → 策略模块(*StraFact)
                                      ↓
                        数据层(WtDtCore/WtDataStorage) ←→ 网关(Parser*/Trader*)
                                      ↓
                        共享基础(Share/Common/Includes/API)
```

- **引擎**负责调度、组合管理、执行、风控；**策略**只实现信号与仓位目标；
- **网关**统一行情解析(Parser)与交易通道(Trader)两族接口；
- **运行器**是可部署进程壳，按场景选择（实盘/回测/UFT/Porter 服务化）。

## 二、核心引擎模块

### 1. WtCore —— 实盘 CTA/HFT 核心
调度器 `WtEngine`、组合管理 `WtPortfolioManager`、执行管理 `WtExecMgr`、
风控 `WtRiskMonMgr`、事件驱动 `EventNotifier`；支持 CTA 与 HFT 双策略通道，
数据落地经 `WtDataWriter`。

### 2. WtUftCore —— 超低延迟 UFT 核心
精简版引擎：`UftEngine` + `UftRunner` 直连策略回调（无中间队列），
tick-to-order 单线程直达路径；本仓库 WtFutuCore 即构建于该核之上。

### 3. WtBtCore —— 回测核心
`CtaMocker/HftMocker/UftMocker/SelMocker` 四族 mocker、历史数据回放
`HisDataReplayer`、撮合模拟（滑点/部分成交/超价拒单可配）；与实盘共用策略代码，
保证 A/B 可比性。本仓库扩展：期货做市 mocker（srand 时间种子，成交笔数有噪声带）。

### 4. WtDtCore / WtDataStorage —— 行情中继与存储
UDP 组播分发、快照+增量合成、K线合成器；落盘为 wt 数据文件格式，
`WtDataStorage` 提供 cday/min/tick 三级读写 API。

## 三、策略模块

| 模块 | 业务定位 |
|---|---|
| `WtCtaStraFact` | 中低频 CTA 策略模板（趋势/震荡/对冲） |
| `WtHftStraFact` | 高频策略模板（tick 级进出） |
| `WtUftStraFact` | UFT 超低延迟策略接口 |
| `WtSelStraFact` | 选股/多因子 |
| **`WtFutuCore`** ★ | 本仓库重心：期货高频做市引擎（详见第四节） |

## 四、WtFutuCore（重点模块）

面向期货做市的完整交易栈：
- **报价模型**：GLFT 公允价+库存风险定价；alpha 集成（OFI/成交动量/盘口失衡/
  动量/lead-lag），权重经 hotparams.yaml 热更新
- **执行**：UnifiedOrderTracker 订单全生命周期、OrderRouter 分源路由、
  B+ 事件驱动补挂（撤单终态即时 requote）、跨腿套利 SpreadArbMgr +
  AsyncArbitrageExecutor(仲裁线程)
- **风控**：FutuRiskMonitor 停机域原子化（halt/closeout）、毒性熔断、
  自成交校准 SelfTradeCalibrator、PolicyChain
- **并发架构**：Md/Td/arb/Timer 多线程直呼回调 + `_cb_mtx` 大锁兜底；
  WS-A/E/F 去大锁基础设施（停机域原子化、PendingCommand 命令通道、两态编译开关）
  全部就位，物理删锁待 TSAN+逐比特A/B+灰度验收（见 src/WtFutuCore/AGENTS.md）
- **观测**：PerformanceMonitor 延迟环(RingBuffer)+双边统计积压落盘

## 五、网关（Parser*/Trader* 对）

CTP 系（ParserCTP*/TraderCTP*）、CTP Mini、飞鼠等；命名规律：
`Parser*`=行情接入，`Trader*`=交易通道，各自实现框架标准接口后即插即用。

## 六、运行器与工具

| 运行器 | 用途 |
|---|---|
| WtRunner / WtBtRunner / WtUftRunner | 实盘/回测/UFT 宿主进程 |
| WtPorter | 服务化门户（Python/HTTP 外接） |
| WtLatencyHFT / WtLatencyUFT | 延迟测量专用壳 |

## 七、共享代码与基础设施

- `Share`: 跨进程共享内存(SharedMsgQueue/ShmRingBuffer)
- `Common`: 数据压缩/编解码/工具函数
- `Includes`: 全部对外头文件与接口定义（WTSTypes/WTSObject 基类）
- `API`: 各网关官方 SDK 头文件集合
- `deps`: MyDepends141 等第三方预编译库

## 八、测试与验证体系

- `TestUnits`：GoogleTest，当前基线 113/113
  （test_session.test_allday/test_shm.test_sharehelper 为上游平台耦合用例，
  分别硬编码 E:\\ 共享内存路径与时区敏感断言）
- 回测回归：`dist/WtBtFutu/_ec_5d.yaml` 全量基准（EXIT=0/0 HALT/Delta=0 判据）
- 新增测试 .cpp 需 `cmake .` 重配置方被 GLOB 收编

## 九、目录速览

```
src/            全部源码(CMake根: src/)
dist/WtBtFutu   回测就绪包(配置样例+_ec_5d基准)
docs/           ARCHITECTURE.md 及专题设计文档
docker/         Linux 构建镜像
```

## 十、快速上手

```bash
cd src/build_all && cmake .. && make -j$(nproc) WtFutuCore TestUnits   # Linux 构建
./build_x64/Debug/bin/TestUnits/TestUnits                              # 单测
cp build_x64/Debug/bin/WtUftRunner/futu/libWtFutuCore.so ../../..../dist/WtBtFutu/uft/
cd dist/WtBtFutu && LD_LIBRARY_PATH=./uft ./uft/WtBtRunner -c _ec_5d.yaml -l logcfgbt.yaml
```
Windows：MSVC 打开 `src/all.sln`/`uft.sln`，需设 `MyDepends141` 指向依赖库。
