# WonderTrader 项目全面审阅报告

> **审阅日期**：2026-10-18
> **审阅对象**：`/mnt/d/gf_pc/WonderTrader/wondertrader`（master，HEAD `b1eab4fa`）
> **审阅范围**：`src/` 下全部自研与框架代码（约 27 万行，不含 `src/API` 9.2 万行券商 SDK 头文件）、构建脚本、测试、`dist/` 部署配置、`docs/` 与仓库治理
> **审阅方法**：静态阅读 + 调用链追踪 + 量化统计（git / wc / grep）+ 五路并行审阅 + 关键结论逐行抽样复核
> **约束**：审阅阶段未修改源码；后续“策略插件边界专项”的实施与验证记录见 `src/WtFutuCore/AGENTS.md`
> **复核标记**：`✅` 已逐行复核（证据链完整）；`◐` 机制已确认、影响程度需结合现场数据量化；`○` 审阅发现、尚待复核
> **版本**：v1.1（2026-10-18）——根据项目方澄清，修正 `WtFutuCore`/`WtOptionCore` 的定位为“分别基于 UFT/HFT 引擎的策略插件”，新增 §1.2.1 双栈对照，并同步调整 A1/A2、3.3/3.4 的表述；问题清单与结论不变。
> **v1.2（2026-10-18）**：策略插件边界专项已按“仅改两个策略”原则实施（`FutuEnginePort` 收敛引擎实现依赖、`WtOptionCore` 移除 `WtCore` 链接）；策略侧 P1-21 闭环，P1-19/P1-20 的构建/运行器部分仍待另案。

**目录**：0 执行摘要 · 1 项目概览 · 2 架构评估 · 3 功能模块 · 4 业务逻辑 · 5 代码质量 · 6 工程治理 · 7 问题清单 · 8 路线图 · 9 附录

---

## 0. 执行摘要

### 0.1 总评

WonderTrader 是“**框架 + 引擎 + 策略插件**”的三层结构：通用框架（`WtCore`/`WtUftCore`/`WtBtCore` + 20 余个 Parser/Trader 网关）提供两套高频引擎——**UFT 引擎**（`WtUftCore`，tick 直达、单线程低延迟）与 **HFT 引擎**（`WtCore` 内的 `WtHftEngine`，组合/执行/风控齐备）。本仓库重点投入的是分别挂在这两套引擎上的两个策略插件：期货高频做市 `WtFutuCore`（`UftStrategy`，3.8 万行）与期权做市 `WtOptionCore`（`HftStrategy`，2.5 万行）。

**业务能力与工程记录明显领先于基础质量**：

- 业务侧有 GLFT 报价、alpha 信号、毒性检测、B+ 订单槽状态机、delta/position 语义边界、套利残腿治理、Black-76 定价/IV 求解等完整实现，且 `AGENTS.md`/`docs` 对设计取舍有罕见的高密度记录。
- 基础侧仍保留大量“上游框架年代”的欠账：订单回报无幂等、持仓容器跨线程裸奔、线程对象析构即 `std::terminate`、mmap 读写缺边界检查、回测用时间种子、构建依赖全局变量、无 CI、仓库 85% 是二进制。

一句话结论：**业务层已经处于“生产级复杂度”，基础层还停留在“能跑就行”的阶段；两者之间的落差，就是当前最大的系统性风险。**

| 维度 | 评分（1–5） | 判断 |
|---|---:|---|
| 业务功能完整度 | 4.5 | 做市/期权/套利/风控/监控链路齐全，细节考虑深 |
| 分层与模块边界 | 2.5 | 四层结构清晰，但自研模块反向依赖框架内部实现 |
| 并发正确性 | 2.0 | 三套线程模型并存；关键容器仍靠一把大锁或完全无锁 |
| 资金/订单安全 | 2.0 | 存在多处可放大持仓、可丢平仓单、可残腿的确定性缺陷 |
| 回测可信度 | 2.0 | 撮合过理想、时间种子不可复现、缺少涨跌停/量约束 |
| 可测试性 | 2.0 | 215 个用例但只覆盖 WtFutuCore，核心框架无测试入口 |
| 构建可复现性 | 1.5 | CMake 3.0 + C++17、全局 INCS/LNKS、GLOB、依赖路径写死 |
| 安全与合规 | 1.0 | 实盘账号/IP 进日志、SSH 密码进 git 历史、部署配置明文口令 |
| 可观测性 | 4.0 | 分类日志、KPI、延迟环、watchdog 做得较好 |
| 文档与知识沉淀 | 3.5 | 设计文档丰富，但 README/单测基线失真、流程文档膨胀 |

**综合：2.6 / 5。** 不是“代码写得差”，而是“业务迭代速度远超基础工程质量”，需要用一次系统性的“止血 + 补地基”把两者拉齐。

### 0.2 最需要先处理的十二个问题

| # | 级别 | 问题 | 位置 | 直接后果 | 复核 |
|---|---|---|---|---|---|
| 1 | P0 | 实盘凭据随代码/日志入库 | `src/WtQuoteProbe/log_order_run1.log`、`scripts/deployment/deploy-futu.sh:67` | 账户与生产主机可被直接接管 | ✅ |
| 2 | P0 | 局部 `std::thread` 析构即 terminate | `TraderDD.cpp:416`、`TraderHuaX.cpp:501`、`TraderXTP.cpp:671`、`TraderXTPXAlgo.cpp:885` | 连不上柜台时进程直接 abort | ✅ |
| 3 | P0 | Parser 退出未停止/join 线程，对象释放后线程继续访问 | `ParserUDP.cpp:114`、`ParserShm.cpp:83`、`ParserXeleSkt.cpp:117`、`ParserXTP.cpp:136` | 退出/切换行情源时 UAF + terminate | ✅ |
| 4 | P0 | 数据层 mmap 越界 | `WtDataReader.cpp:341/353`、`WtRdmDtReader.cpp:1920`、`WtDataWriter.cpp:1453` | 开盘前读 tick 崩溃、后复权写越界、磁盘满崩溃 | ✅ |
| 5 | P0 | 成交回报无幂等 | `WtCore/TraderAdapter.cpp:2234` 起 | 重连/重复推送导致持仓与资金重复累加 | ✅ |
| 6 | P0 | 撤单回报重复归还可平量 | `WtCore/TraderAdapter.cpp:2126-2147` | 可平量虚高 → 超量平仓/废单 | ✅ |
| 7 | P0 | 平仓单被拒仍置“在途” | `WtFutuCore/CloseoutExecutor.cpp:386-389` | closeout 空等，仓位可能过夜 | ✅ |
| 8 | P0 | 组合单“发出即完成”，残腿失联 | `WtOptionCore/ComboOrders.cpp:123-150` | leg1 成交、leg2 拒单 → 裸敞口 | ✅ |
| 9 | P0 | Delta 对冲手数单位错配（按乘数放大） | `DeltaHedgeExecutor.h:61` + `HftOptionStrategy.cpp:359-368` | 反复超量对冲、过冲 | ✅ |
| 10 | P0 | 持仓容器跨线程无锁读写 | `WtEngine.cpp:953` vs `:199-203` | `unordered_map` 并发插入/遍历 UB | ◐ |
| 11 | P0 | CTP 行情登录错误码被全吞 | `ParserCTP.cpp:429` | 登录失败仍订阅，交易日/数据错误 | ✅ |
| 12 | P1 | 固定 32 字节 tag + `strcpy` | `CtaMocker.cpp:1659`、`CtaMocker.h:283` | 策略传入长 userTag 即堆/栈溢出 | ✅ |

### 0.3 五条主要结论

1. **架构上“框架是底座”已经不成立。** `WtFutuCore` 直接 `#include "../WtUftCore/UftStraContext.h"` 并 `dynamic_cast` 取 `EventNotifier`（`UftFutuMmStrategy.cpp:21`、`UftFutuMmStrategy.h:30`），策略层事实上成了组合根；`WtRunner` 又无条件链接 `WtOptionCore`（`WtRunner/CMakeLists.txt:23`），把期权策略插件、OpenMP、`-march=native` 塞进每一个 CTA/HFT 实盘进程。
2. **业务逻辑的最大风险不在“策略不赚钱”，而在“账不平”。** 订单/成交/持仓三本账的幂等与生命周期是当前最薄弱的环节；`WtUftCore/CloseFreezeLedger.h` 已经给出了正确的“冻结—恰好一次归还”范式，但 `WtCore` 老路径没有采用。
3. **回测的可信度不足以支撑“回测—实盘一致”的结论。** `HftMocker.cpp:57` 用 `srand(time(NULL))` 生成成交量，`_ec_5d` 复跑本身就有 ±1.4k 净值噪声（`AGENTS.md` 自述）；mocker 也没有涨跌停、价格带、可成交量约束。当前回测更适合做“方向性验证”，不适合做参数精细寻优。
4. **工程治理已经影响到交付。** CMake 声明 3.0 却要求 C++17；`WtFutuCore`/`WtOptionCore` 不在 `all.sln` 里，另有 9 个网关不在 Linux CMake 里；`TraderDD.cpp:902` 的全角括号使其在 MSVC 下无法编译；测试 `main()` 里 `getchar()` 会让 CI 永久挂起；README 的“113/113”与实际 215 个 `TEST` 不符。
5. **安全事件已经发生，不是潜在风险。** 实盘账号、经纪商、前置地址、AppID 尾号在被 git 跟踪的日志里；部署脚本的 SSH 密码进入了提交历史（`git log -S` 可命中）。这需要按“已泄露”处置：轮换 + 清史 + 扫描门禁，而不是改个文件了事。

---

## 1. 项目概览

### 1.1 规模与构成

| 功能域 | 行数（约） | 文件数 | 说明 |
|---|---:|---:|---|
| 引擎 `WtCore` | 19,500 | 62 | 实盘 CTA/HFT/SEL 核心、组合、执行、风控 |
| 引擎 `WtUftCore` | 7,900 | 25 | 超低延迟 UFT 内核 |
| 引擎 `WtBtCore` | 14,000 | 20 | 回测框架、四类 mocker、历史回放 |
| 策略 `WtFutuCore`（UFT） | 38,300 | 126 | 期货高频做市 + 套利 + 风控 + 监控；宿主 `WtUftCore`，插件 `FutuStraFact.FutuMM` |
| 策略 `WtOptionCore`（HFT） | 24,700 | 136 | 期权做市 + 定价 + 组合单 + 对冲；宿主 `WtCore`，插件 `OptionStraFact.OptionMM` |
| 数据层（Storage/AD/DtCore/Servo/Helper/Porter） | 19,700 | 68 | 自有 wt 文件格式、mmap 读写、UDP 分发 |
| 网关 `Parser*` + `Trader*` | 27,500 | 58 | 25 个行情/交易网关模块 |
| 基础 `Share/WTSTools/WTSUtils/Includes/Common/FasterLibs` | 50,000 | 221 | 其中 WTSUtils 约 2.2 万行为 zstdlib/lmdb/StackWalker 等第三方 |
| 策略工厂 `*StraFact` / `WtExeFact` / `WtRiskMonFact` | 6,200 | 36 | 示例策略与执行/风控插件 |
| 运行器与工具 | 10,900 | 32 | WtRunner/WtUftRunner/WtBtRunner/WtPorter 等 |
| 测试 `TestUnits` | 5,900 | 42 | GoogleTest，215 个 `TEST` |
| **合计（不含 `src/API`）** | **≈ 270,000** | **≈ 1,000** | |

> 另有 `src/API` 约 9.2 万行券商 SDK 头文件（454 个文件、625 MB），以及 `src/WtQuoteProbe` 约 4.5 万行（其中 4.1 万行是两份重复的 CTP SDK 头文件）——后者是探针工程遗留，不属于框架。

### 1.2 技术栈与运行形态

- C++17；Linux 用 CMake + GCC，Windows 用 MSVC + 64 个 `.vcxproj` / 72 个工程的 `all.sln`。
- 第三方：boost（filesystem/thread）、rapidjson、spdlog（部分）、nanomsg、lmdb、zstd、OpenMP（`WtOptionCore`）。
- 运行形态：`WtRunner`（CTA/HFT/SEL 实盘，承载 `WtOptionCore` 的 HFT 期权策略）、`WtUftRunner`（UFT 实盘，承载 `WtFutuCore` 的 UFT 做市策略）、`WtBtRunner`（回测）、`WtPorter`（C 接口服务化）、`WtDtServo`/`QuoteFactory`（行情落地与服务）。
- 插件：网关/策略/执行器均为动态库，`CtaStrategyMgr`/`HftStrategyMgr`/`ParserAdapter`/`TraderAdapter` 通过 `DLLHelper::load_library` + `dlsym` 按目录扫描加载。

#### 1.2.1 两套高频策略栈（UFT vs HFT）

| 维度 | `WtFutuCore`（期货做市） | `WtOptionCore`（期权做市） |
|---|---|---|
| 宿主引擎 | `WtUftCore`（UFT 引擎） | `WtCore` 的 `WtHftEngine`（HFT 引擎） |
| 策略基类 | `UftStrategy`（`UftFutuMmStrategy.h:245`） | `HftStrategy`（`HftOptionStrategy.h:61`） |
| 工厂接口 | `IUftStrategyFact`（`UftFutuMmStrategy.cpp:1079`） | `IHftStrategyFact`（`HftOptionStraFact.cpp:12`） |
| 工厂 / 策略名 | `FutuStraFact` / `FutuMM` | `OptionStraFact` / `OptionMM` |
| 运行器 | `WtUftRunner` | `WtRunner`（内部 `WtHftEngine`） |
| 插件目录 | `./uft/libWtFutuCore.so` | `./hft/libWtOptionCore.so` |
| 配置节点 | `strategies.uft[]` | `strategies.hft[]` |
| 引擎特征 | tick 直达、单线程回调、低延迟 | 引擎托管组合/执行/风控，回调经 context |
| 策略侧线程 | Md/Td/arb/Timer 自建线程 + `_cb_mtx` | `OptionAsyncEventProcessor` + worker |
| 部署实例 | `dist/WtRunnerFutu/` | `dist/WtRunnerOption/` |

加载链路（已核对）：`WtUftRunner::initUftStrategies` → `./uft/` → `createStrategyFact` → `FutuStraFact.FutuMM`（`WtUftRunner.cpp:204-213`）；`WtRunner::initHftStrategies` → `./hft/` → `createStrategyFact` → `OptionStraFact.OptionMM`（`WtRunner.cpp:330-339`）。

### 1.3 构建与部署现状

- 根 `src/CMakeLists.txt` 用**全局变量** `INCS`/`LNKS`/`LIBS` 传递头文件与库路径，各模块 `INCLUDE_DIRECTORIES(${INCS})` + `file(GLOB SRCS *.cpp)`。
- 依赖路径写死：Windows `%MyDepends141%`，Linux `/home/mydeps`（`src/CMakeLists.txt:17-18,40,46-47`）。
- `cmake_minimum_required(VERSION 3.0.0)`，但设置 `CMAKE_CXX_STANDARD 17`（需要 CMake ≥3.8；CMake 4.x 已移除 <3.5 兼容）。
- `WtFutuCore` 与 `WtOptionCore` 均未进入 `src/all.sln`，Windows 无法用官方解决方案构建这两个策略模块。
- 部署依赖 `scripts/deployment/deploy-futu.sh`（sshpass 明文密码）与远程 cron（`monitor/wt_restart.sh`，工作日 08:40/20:40 重启）。

---

## 2. 架构评估

### 2.1 分层现状

设计意图是清晰的四层：

```text
┌──────────────────────────────────────────────────────────────┐
│ 运行器宿主  WtRunner / WtUftRunner / WtBtRunner / WtPorter    │
├──────────────────────────────────────────────────────────────┤
│ 引擎        WtCore(CTA/HFT/SEL)  WtUftCore  WtBtCore         │
│             ↑ 策略插件：WtFutuCore(UFT) / WtOptionCore(HFT)          │
├──────────────────────────────────────────────────────────────┤
│ 领域服务    组合/执行/风控/策略上下文/数据管理/事件通知          │
├──────────────────────────────────────────────────────────────┤
│ 基础设施    Share / WTSTools / WTSUtils / Includes / API      │
│ 接入        Parser*(行情) / Trader*(交易) / 数据存储           │
└──────────────────────────────────────────────────────────────┘
```

但实际依赖关系已经偏离设计：

```text
WtRunner ──link──> WtOptionCore ──PUBLIC link──> WtCore ──> Share/WTSTools
   │                    │
   └──link──────────────┴──> WtCore（WtRunner 自身并未使用 WtOptionCore 符号）

WtFutuCore ──include──> ../WtUftCore/UftStraContext.h
           ──dynamic_cast──> WtUftCore::EventNotifier
           ──link──> WtUftCore / WTSTools / Share
```

### 2.2 架构问题

**A1｜`WtRunner` 硬链接 `WtOptionCore`，与插件化设计自相矛盾** ✅

`WtOptionCore` 本身是 HFT 策略插件：`OptionStraFact.OptionMM` 由 `HftStrategyMgr` 从 `./hft/` 目录 `dlopen` 加载（`WtRunner.cpp:339`），部署位置是 `dist/WtRunnerOption/hft/libWtOptionCore.so`。但 `src/WtRunner/CMakeLists.txt:23` 又把 `WtOptionCore` 放进 `LIBS`，而 `WtRunner` 源码中没有任何 `WtOptionCore` 符号引用（grep 仅命中 CMakeLists）。后果：

- 每个 CTA/HFT 实盘进程都静态带入一份期权策略代码及其依赖（`WtCore` 被 `PUBLIC` 传递链接、OpenMP/gomp、boost_thread）；
- `WtOptionCore` 的 Release 选项包含 `-O3 -march=native -flto`（`WtOptionCore/CMakeLists.txt:181-189`），产物与构建机 CPU 指令集绑定，换机器可能 `SIGILL`；
- `WtCore` 是 `STATIC` 库，同时被 `WtRunner` 和 `libWtOptionCore.so` 链接，可能产生两份静态状态（日志器、注册表等），破坏单例语义；
- 插件本应“拷一个 `.so` 即可热替换”，硬链接后这个能力名存实亡。

**建议**：删除 `WtRunner` 对 `WtOptionCore` 的链接，恢复 `./hft/` 动态加载；`WtOptionCore` 只链接 `WTSTools`/`Share`（或纯 `Includes` 接口），不要 `PUBLIC WtCore`。

**A2｜策略插件穿透引擎边界，`WtFutuCore` 成为“组合根”** ✅

`WtFutuCore` 直接包含 `WtUftCore` 的内部头文件：

- `FutuDataDefs.h:11` → `../WtUftCore/UftDataDefs.h`
- `UftFutuMmStrategy.cpp:21` → `../WtUftCore/UftStraContext.h`
- `UftFutuMmStrategy.h:30`、`FutuRiskMonitor.cpp:14` → `../WtUftCore/EventNotifier.h`
- `MonitorBridge.cpp:11` → `../WtUftCore/WtHelper.h`

并且在策略构造函数里 `dynamic_cast<UftStraContext*>` 取 `EventNotifier`（`UftFutuMmStrategy.cpp:21` 注释“R1: dynamic_cast 获取 EventNotifier”）。这意味着：

- 策略与框架的具体实现类强耦合，框架升级/替换会直接破坏策略；
- “告警外发”这类基础设施能力本应由引擎注入接口，而不是策略反向抓取；
- `WtFutuCore` 无法在 `WtUftCore` 之外复用，也无法独立单测。

作为对照，`WtOptionCore` 的头文件只依赖 `Includes/*`（`HftOptionStrategy.h:10-34`），插件边界反而是干净的；它的 CMake 却仍 `PUBLIC WtCore`（`WtOptionCore/CMakeLists.txt:204`），属于“头文件干净、链接过度”。两个模块恰好展示了两种典型偏差：一个把引擎实现拉进策略，一个把引擎库塞进插件。

**建议**：在 `Includes` 增加 `IEventSink`/`IAlertSink` 接口，由 `WtUftEngine` 注入；`WtFutuCore` 只依赖 `Includes` 的抽象接口。短期至少把 `UftStraContext.h` 的依赖收敛到一个 `FutuEnginePort` 适配层。

**A3｜构建依赖图是“隐式”的** ✅

- `WtCore` 是 `STATIC`、`WtUftCore` 是 `SHARED`，两者都没有 `target_link_libraries`（`WtCore/CMakeLists.txt:25`、`WtUftCore/CMakeLists.txt:22`）；
- 所有头文件/库路径通过父目录的 `INCS`/`LNKS` 全局变量注入，目标之间没有 `target_include_directories` / `target_link_libraries` 的可传递依赖；
- 40 个 `CMakeLists.txt` 用 `file(GLOB ...)` 收集源文件，新增文件必须手工 `cmake .` 重配（README 也承认这一点）。

后果：任意模块的编译/链接成功依赖“目录处理顺序 + 全局变量 + 手工 LIBS 列表”，CI 无法只构建一个目标，增量构建与并行构建都不可靠。

**建议**：以 `WtOptionCore` 为模板（它是全仓唯一使用 `target_*` + `find_package` + `enable_testing` 的模块），逐步把 `Share`/`WTSTools`/`WtCore`/`WtUftCore` 改成 `INTERFACE`/`PUBLIC` 依赖，配 `CMakePresets.json`。

**A4｜引擎与策略层共四套线程模型，边界不清** ✅

| 栈 | 线程模型 | 同步手段 | 风险 |
|---|---|---|---|
| `WtCore`（CTA/HFT/SEL 引擎，宿主 `WtOptionCore`） | 行情线程 + 交易回调线程 + 任务线程 + 定时线程 | 局部 `SpinLock`、`_mtx_orders`，持仓/资金基本无锁 | 容器并发读写 UB |
| `WtUftCore`（UFT 引擎，宿主 `WtFutuCore`） | 行情/交易回调直接进策略，单线程直达 | 无锁（靠“单线程”假设） | 假设一旦被回调线程打破即失效 |
| `WtFutuCore`（UFT 策略层） | 在引擎回调之外自建 Md/Td/arb/Timer 四线程 | `_cb_mtx` 大锁 + 91 个 `std::atomic` + 155 处 `RecursiveSpinGuard` | 大锁内穿越发单；去大锁路线存在框架级阻断 |
| `WtOptionCore`（HFT 策略层） | HFT 回调线程 + `OptionAsyncEventProcessor` worker | 事件双队列（MD SPSC / 交易 mutex） | 命令通道与回调线程交叉；`CommandBus` 解析可抛异常 |

四种模型对“谁是 owner、什么能跨线程”的约定不同，导致同一类 bug 在不同栈里反复出现（持仓跨线程、回调重入、线程退出）。尤其要注意：**UFT/HFT 引擎本身是单线程/事件驱动的，复杂度主要来自策略层在引擎之上再建线程**。

**建议**：先统一“线程归属”文档与断言（debug 构建下用线程 ID 断言 owner），再谈去锁；`WtFutuCore` 的去大锁应以 `UftStraContext::_order_ids` 等框架容器的线程安全为前提（见 §3.3 与 P1-32）。

**A5｜回测/实盘分叉点散落** ◐

分叉依赖配置 `isBacktest`、`useAsyncArbThread`、`FUTU_CB_LOCK_BIG` 等开关散落在策略/协调器/装配器中。`AGENTS.md` 记录过一次真实事故：回测配置漏写 `isBacktest: true`，导致 `on_trade` 在回调栈内同步发单，mocker 的 `_orders` 迭代器失效并死循环（CPU 105%、日志冻结）。

这不是单点 bug，而是“运行模式”没有收敛为**显式的运行时上下文**。建议引入 `RunMode`（Backtest / Replay / Live）并作为依赖注入到引擎与策略，禁止各模块自行读布尔开关；回测/实盘差异集中在一张“能力表”里，由单测锁定。

**A6｜网关层是复制粘贴式扩展** ✅

25 个 Parser/Trader 模块共享相似结构，但没有公共基类/模板收敛：登录状态机、查询令牌、重连、KV 缓存、userTag 解析在 CTP/Mini/Opt/Femas/XTP/OES/HuaX/YD 中各写一遍，已经出现明显漂移：

- `ParserCTP` 族 `IsErrorRspInfo()` 恒 `false`（`ParserCTP.cpp:429`），登录失败被当成功；
- `TraderOES` 断线只回调事件、不复位 `_state`（`TraderOES.cpp:591`）；
- `TraderHTS` 查询入队无锁（`TraderHTS.cpp:966`）；
- `TraderCTP` 的 `OrderRef` 用 `fetch_add(0)`，userTag 为空时重复引用（`TraderCTP.cpp:353`）。

**建议**：抽 `TraderBase`/`ParserBase`（连接状态机、查询限流、重连策略、错误码映射、userTag 编解码），各网关只实现 SDK 适配。短期先做“错误码统一映射表”，把 CTP 的 31/50/51/流控等分类放到框架层（这也正是 `WtFutuCore/AGENTS.md` 里记录的“已知外部限制”）。

**A7｜数据格式自研，兼容性靠“多版本分支”** ◐

`WtDataStorage` 自定义二进制块格式，`WtDataReader` 里对 block 版本做了 4 套分支（`:385/944/1060/1223/1292`），但多处不检查 `proc_block_data` 返回值（`:1496` 等）。格式没有 magic/version 的显式校验与迁移工具，坏块会被当成有效 bar/tick。

**建议**：文件头增加 `magic + version + endianness + checksum`；读取路径统一走“校验失败 → 返回 NULL + 告警”，并提供 `wt_data_check` 工具做离线巡检。

**A8｜可测试性被构建结构锁死** ✅

`TestUnits/CMakeLists.txt` 只链接 `WtFutuCore/WTSTools/WTSUtils/WtShareHelper`，因此 `WtCore`/`WtBtCore`/`WtDataStorage`/网关**无法被单测覆盖**。215 个 `TEST` 中绝大多数是 `WtFutuCore` 的数学/状态机/配置契约测试；`TraderAdapter`、`CtaMocker`、`WtDataReader` 这些最需要回归的模块零覆盖。

**建议**：把核心库改为可链接的测试目标，至少给 `CtaMocker`、`TraderAdapter`（用 mock `ITraderApi`）、`WtDataReader/Writer` 建 fixture；这比继续给策略层加单测更能降低资金风险。

### 2.3 并发模型细评

| 关注点 | 现状 | 评价 |
|---|---|---|
| 线程创建/销毁 | 多处 `new StdThread` 后不 join/detach；`StdThread` 就是 `std::thread`（`Share/StdUtils.hpp:27`） | **危险**：析构 joinable thread 直接 `std::terminate` |
| 锁粒度 | `WtFutuCore` 的 `_cb_mtx` 覆盖 13 个回调；`FutuQuoter` 自旋锁内调用 `stra_quote/stra_cancel` | **危险**：锁内穿越框架调用，存在锁反转/尾延迟 |
| 原子变量 | `WtFutuCore` 91 个 `std::atomic`，但仍有 `_ctx` 裸指针、`_delta_rate_breach_time` 等非原子共享 | **不一致**：原子化没有统一规则 |
| 容器 | `_pos_map`、`_positions`、`UftStraContext::_order_ids` 等 map 跨线程读写 | **UB**：未定义行为，TSAN 必报 |
| 停机 | `WtEngine::_terminated` 只初始化、从不置真；CTA/SEL 析构不 `stop()` ticker | **泄漏/abort**：任务线程永不退出 |
| 生命周期 | 4 个 Parser 的 `release()` 未 join 线程（3 个空实现，XTP 只 Release API），`ParserAdapter` 随后 delete | **UAF**：IO 线程继续访问已释放对象 |

### 2.4 回测与实盘一致性

| 维度 | 回测 | 实盘 | 一致性判断 |
|---|---|---|---|
| 成交 | mocker 按对手价/信号价立即成交 | 排队、部分成交、拒单 | **偏差大** |
| 涨跌停/价格带 | 无校验 | 交易所拒单 | **缺失** |
| 成交量约束 | `HftMocker` 用 `rand()` 生成可成交量，`srand(time(NULL))` | 真实盘口 | **不可复现** |
| 手续费/保证金 | 有，但 `calcFee` 用 `int32` 分单位 | 柜台计算 | **口径有偏差** |
| 滑点 | 可配 | 真实冲击 | 可接受 |
| 时序 | `replayMsFromTick` 统一时钟；`useAsyncArbThread=false` | 多线程异步 | 设计上已收敛，但依赖开关 |
| 平仓 | closeout 同步 | closeout 异步 + 拒单 | 关键路径未覆盖 |

`HftMocker.cpp:57` 的 `srand((uint32_t)time(NULL))` 使同一份配置两次回测的成交笔数不同；`AGENTS.md` 也自述 `_ec_5d` 复跑有 ±1.4k 净值噪声。**在消除该噪声之前，任何“参数微调带来 +x% 收益”的结论都不可信。**

### 2.5 架构评分

| 子项 | 分数 | 理由 |
|---|---:|---|
| 分层清晰度 | 3.0 | 目录与职责大体清晰，但存在反向依赖 |
| 模块内聚 | 2.5 | 多个 2k–4.5k 行上帝类 |
| 依赖管理 | 1.5 | 全局变量 + 隐式链接 + 无版本锁定 |
| 并发设计 | 2.0 | 三套模型，缺少统一线程归属 |
| 可扩展性 | 3.0 | 插件机制可用，但 ABI/接口耦合重 |
| 可测试性 | 2.0 | 核心库不可测 |
| 可运维性 | 3.5 | watchdog/日志/KPI 较好，但部署靠脚本 |
| **加权** | **2.5** | |

---

## 3. 功能模块评估

### 3.1 成熟度矩阵

| 模块 | 定位 | 成熟度 | 主要短板 |
|---|---|---|---|
| `WtCore` | CTA/HFT/SEL 实盘核心 | 中（功能全、欠账多） | 订单幂等、线程安全、退出生命周期 |
| `WtUftCore` | UFT 超低延迟内核 | 中 | 接口有死代码；容器跨线程假设未文档化 |
| `WtBtCore` | 回测框架 | 中低 | 撮合过理想、非确定性、MatchesEngine bug |
| `WtFutuCore` | 期货高频做市（UFT 策略，宿主 `WtUftCore`） | 高（业务）/低（并发） | 大锁、状态标志作用域、失败静默 |
| `WtOptionCore` | 期权做市（HFT 策略，宿主 `WtCore`） | 中（数学好）/低（编排） | 组合单残腿、对冲手数、无有效测试 |
| 数据层 | 行情存储与分发 | 中 | mmap 越界、坏块不校验、无版本头 |
| 网关层 | 25 个行情/交易通道 | 中低（CTP 高、其余参差） | 生命周期、错误码、复制粘贴漂移 |
| 测试体系 | GoogleTest | 低 | 只覆盖策略层、无 CI、会挂起 |

### 3.2 核心引擎（WtCore / WtUftCore / WtBtCore）

**做得好的地方**

- `WtEngine ← WtCtaEngine/WtHftEngine/WtSelEngine` 的继承结构 + `IStraCtx` 接口，策略与引擎隔离得比较干净；
- `ActionPolicyMgr` 把开/平今/平昨/净仓规则做成品种级配置，适配不同交易所；
- `WtExecuterFactory` 让执行算法可插拔；
- `CloseFreezeLedger`（`WtUftCore`）把“下单即冻结、拒单/撤单恰好一次归还”写成显式不变量，是全仓最正确的订单台账设计。

**主要问题**

1. **上帝类**：`HisDataReplayer.cpp` 4,571 行、`TraderAdapter.cpp` 2,439 行、`CtaStraBaseCtx.cpp` 2,069 行。单类同时承担路由、持仓台账、风控计数、自成交、落盘、日志，修改任意一处都可能影响其他职责。
2. **订单侧有幂等、成交侧没有**：`onPushOrder` 用 `_orderids` 防重复（`TraderAdapter.cpp:2044-2048`），`onPushTrade` 完全不去重（`:2234` 起）。CTP 在重连/查询后会重推成交，这是资金错账的直接来源。
3. **可平量是“加减法”而不是“台账”**：首次订单推送扣减、撤单推送加回（`:2126-2147`）。同一个撤单回报推两次就加回两次；`onRspOrders` 又把已有订单号预登记（`:1726`），使“首次推送”分支被跳过，重连后一次撤单就能凭空放大可平量。
4. **任务线程没有停止语义**：`_terminated` 只在 `WtEngine.cpp:43` 初始化和 `:1094` 读取，全仓没有处置真；`~WtCtaEngine`（`WtCtaEngine.cpp:41-47`）直接 `delete _tm_ticker`，而 `~WtCtaRtTicker` 为空（`WtCtaTicker.h:36`），ticker 内部 `_thrd` 仍是 joinable thread。
5. **UFT 接口死代码**：`WtUftEngine::get_tick_slice` 直接 `return NULL;` 再 `return _data_mgr->...`（`WtUftEngine.cpp:100-104`），`get_kline_slice` 同样。策略按接口语义调用会拿到 NULL。
6. **`ShareManager` 双份实现**：`WtCore` 与 `WtUftCore` 各有一份 259 行 `.cpp` + 137 行 `.h`，内容有差异但职责相同，且头文件硬绑定具体引擎类型（`ShareManager.h:107`）。
7. **`EventNotifier` 内存泄漏**：订单通知 `ordInfo->retain()` 后 lambda 内没有 `release()`（`EventNotifier.cpp:218`），成交分支有配对的 release（`:201/207`）。长跑进程每笔报单泄漏一个订单对象。

**回测引擎（WtBtCore）**

- `HftMocker.cpp:57/87` 用 `srand((uint32_t)time(NULL))`，`:108` 用 `srand(TimeUtils::getCurMin())`，成交笔数/可成交量随墙钟变化——回测不可复现。
- `MatchEngine.cpp:206-210` 构建买盘簿时把 `askqty(i)` 写进 `volume`，`:217-224` 的 `lower_bound` 可能返回 `end()` 后直接解引用。
- `HftMocker.cpp:346-348`、`UftMocker.cpp:230` 存在“空作用域锁”：`{ std::unique_lock ... }` 立即解锁，临界区实际无保护。
- 全模块没有涨跌停、价格带、可成交量约束；`CtaMocker::do_set_position` 在信号价直接记账，限价单价格落在 bar 之外也成交。
- `CtaMocker.cpp:1659/1758` 把策略传入的 `userTag` `strcpy` 进 `_opentag[32]`（字段定义 `CtaMocker.h:283`），长度不受控。

### 3.3 UFT 做市策略 WtFutuCore（宿主：WtUftCore）

`WtFutuCore` 实现 `UftStrategy`（`UftFutuMmStrategy.h:245`），以 `FutuStraFact.FutuMM` 插件形式由 `WtUftRunner` 从 `./uft/` 加载（`WtUftRunner.cpp:204-213`），配置位于 `strategies.uft[]`。因此本节讨论的“线程/大锁”是**策略在 UFT 单线程直达内核之上自建的多线程层**，不是 UFT 引擎本身的调度；UFT 引擎负责把行情/交易回调直接派发给策略。

**结构**（意图清晰，实现偏重）：

```text
FutuComponentFactory / FutuModuleAssembler   ← 装配与依赖校验
        ↓
StrategyCoordinator (1970 行)                ← 编排：行情→信号→报价→风控→落盘→KPI
   ├─ FutuQuoter (910)                       ← 报价层/义务单/撤补
   ├─ SpreadOptimizer + signals              ← 公允价/价差/skew/alpha
   ├─ FutuRiskMonitor (1392) + QuotePolicyChain ← 风控闸门/策略链
   ├─ OrderRouter + UnifiedOrderTracker      ← 发单/订单槽状态机
   ├─ SpreadArbitrageManager + AsyncArbitrageExecutor ← 跨腿套利
   └─ PerformanceMonitor/Analyzer            ← 延迟/KPI
```

**优点**

- B+ 订单槽状态机方向正确：撤单确认后才清槽、超时重试、K 次后升级 zombie，且“永不遗忘活单”的不变量写进了注释与单测；
- `delta`（策略口径）与 `position`（风控口径）的语义边界有成文裁决，并有契约测试守护；
- 配置四源审计（present/read/unknown/read_missing）、热参 seqlock + 边界表、`HotLogGate`、KPI 巡检行，工程化程度高于多数同类项目；
- 回测/实盘分叉点被尽量收敛（`isBacktest`、`useAsyncArbThread`），并记录了踩坑。

**问题**

1. **大锁内穿越发单**（`◐`）：`FutuQuoter` 持 `_lock` 调用 `stra_quote/stra_cancel`（`FutuQuoter.cpp:227→268`、`:634-668`），与 `OrderApiGuard.h:14-20` 的“临界区 = 单次框架调用”约定不符；若框架同步回执进回调，存在锁反转/自旋死锁风险，至少会造成 Td 回调尾延迟。
2. **去大锁路线存在框架级阻断**（`○`）：`WtUftCore/UftStraContext` 的 `_order_ids` 被 Md（发单）与 Td（回报）双线程裸访（`UftStraContext.cpp:920`、`:610-631`），`_cb_mtx` 是唯一的串行化者。`FUTU_CB_LOCK_BIG=0` 之前必须先解决该容器。
3. **订单槽无重复 id 防御**（`✅`）：`UnifiedOrderTracker::track` 未查重（`UnifiedOrderTracker.cpp:33-51`），同一 localid 二次 track 会覆盖索引、留下孤儿槽，pending 永不回冲。当前 `useBilateralQuote:false` 未触发，一旦启用双边报价即成 P0。
4. **`cancelRetryIntervalMs=0` 会静默关闭整个 B+ 保护**（`✅`）：重试、zombie 升级、`_zombie_alive_buf` 刷新全在 `if (timeout > 0)` 内（`UnifiedOrderTracker.cpp:353`），loader 没有下限校验。
5. **全局 bool 对逐合约窗口**（`○`）：`_section_break_active` 是单个 bool（`StrategyCoordinator.h:500`），但 `in_break` 按合约计算；跨品种 session 不同期时会互相覆盖，导致休息窗内反复全撤/重挂。
6. **delta-rate 闸门被采样稀释**（`○`）：32 槽环形缓冲（`FutuRiskMonitor.h:644`）配 10 秒窗口（`:858`），逐 tick 采样，速率会被低估 2–10 倍。
7. **失败静默**：closeout 拒单只 warn（`CloseoutExecutor.cpp:562-590`）、`FixedString24` 截断（`arb/AsyncArbitrageExecutor.h:64-74`）、`maxOrders` 读了不执行（`FutuModuleAssembler.cpp:312`）。

### 3.4 HFT 期权做市策略 WtOptionCore（宿主：WtCore/WtHftEngine）

`WtOptionCore` 实现 `HftStrategy`（`HftOptionStrategy.h:61`），以 `OptionStraFact.OptionMM` 插件形式由 `WtRunner` 从 `./hft/` 加载（`WtRunner.cpp:330-339`），配置位于 `strategies.hft[]`；组合、执行、风控、账户由 HFT 引擎托管，策略侧专注定价、报价、组合单与对冲。它也是本仓库唯一按“干净插件接口”写头文件的策略模块（只包含 `Includes/*`）。

**优点**

- 数学内核质量高：`BlackCalc`/`BlackImpliedCalculator` 与 QuantLib 的解析式逐式一致，put delta 的交叉项处理正确；IV 用 put-call parity 反推远期 `F = K + (C-P)/df`，并有 forward/maturity/discount 预校验；
- 明确的 NaN 语义取舍：拒绝 `-ffinite-math-only`，保留 NaN 哨兵（`CMakeLists.txt:176-182`）；
- 异步双队列（行情 SPSC + 交易 mutex）设计正确，`on_trade/on_position` 一律入队；
- 代码中保留 Bxx/Rxx 缺陷编号与成因，可追溯性好。

**问题**（按资金影响排序）

1. **组合单“发出即完成”**（`✅`）：`SpreadComboOrder::checkDone` 在 leg2 发出后直接返回 `m_leg2Sent`（`ComboOrders.cpp:146-150`，注释自认 “For simplicity”），随后 `m_done=true; m_active=false`，下一轮被移出 `_activeCombos`。leg1 成交而 leg2 被拒时，既没有补腿也没有撤 leg1 → **裸敞口**。
2. **补腿覆盖旧 id**（`✅`）：`hedge()` 每次覆盖 `m_leg2LocalId`（`ComboOrders.cpp:81-85`），`onLegFill` 只认最新 id（`:119`），`cancelAll` 只撤最新（`:168-177`）；早期对冲单成为孤儿，可能双倍对冲。
3. **对冲手数单位错**（`✅`）：`lotsRaw = |excess| / max(minHedgeQty, 1e-9)`（`DeltaHedgeExecutor.h:61`），而 `minHedgeQty` 在 `HftOptionStrategy.cpp:359-368` 从未从配置装载，恒为 1.0；`excess` 来自 `getPortfolioDelta`，后者已乘合约乘数（`OptionRiskData.cpp:64-66`）。IO 乘数 300 时，1 手 ATM 多头约 150 delta → 实际会算出 150 手，再被 `maxOrderSize=5` 截断，每 3 秒重复一批。
4. **无在途单抑制**（`✅`）：`decide()` 不检查未成交对冲单；以 mid 挂限价难成交，冷却期一到再发一批 → 过冲。
5. **`thetaPerDay` 头/实现语义不一致**（`✅`）：头文件声明 `thetaPerDay(double spot, double maturity)`（`BlackCalc.h:49`），实现是 `thetaPerDay(double rate, double vol)`（`BlackCalc.cpp:190`），类型相同编译器不报错；唯一调用方按 `(0, m_theoVol)` 传参（`OptionPricer2.cpp:514`），任何按头文件语义调用的新代码都会得到错误结果。
6. **NaN 被静默洗成 1%**（`✅` 逻辑）：`std::max(0.01, theoVol)`（`OptionPricer2.cpp:540`），当 `theoVol` 为 NaN 时比较为假、返回 0.01，全链以 1% 波动率定价且不告警。
7. **`ConfigValidator` 空转**（`✅`）：`validate()` 里的子键遍历循环第一行就是 `break`（`ConfigValidator.h:78-82`），实际只做硬编码 key 探测；拼写错误的配置键永远不会告警，与注释宣称的“防漂移”相反。
8. **零波动 gamma/vega 除零、IV 无解整档丢弃、`catch(...)` 吞异常、thread_local 裸指针缓存**（`○`）：见问题清单 P1-31、P1-16、P1-17。
9. **测试形同虚设**：`tests/` 只有 2 个自研断言程序，BlackCalc/Brent/ComboOrders/ExpirationSimulator 零覆盖；`test_enhancements.cpp:24-37` 用 `std::exit(1)` 而不是 gtest。
10. **构建**：`-O3 -march=native -flto` 进发行库（`CMakeLists.txt:181-189`）；OpenMP 缺失时直接链 `gomp`（`:212-217`），MSVC/Clang 上会失败。

> 说明：并行审阅曾提出“`ExpirationSimulator` 现金流与盯市重复计入”，经逐行复核**不成立**——`tradingPnl` 是累计现金流、`settlementPnl/closePnl` 是当前仓位估值，两者相加是权益快照，`dayPnl` 取差分，口径正确。已从问题清单剔除。

### 3.5 数据层

**能力**

- 自有 wt 二进制格式，支持 tick / min / day / 复权因子 / 委托队列 / 委托明细；
- `WtDataWriter` 对 tick 做交易日、成交量单调性校验，处理 CZCE 重复时间戳（+200ms），RT block 翻倍扩容；
- `WtDataReader` 兼容 4 种 block 版本与压缩；
- `WtDtServo` 提供随机访问，`WtDtCore` 负责 UDP 组播分发与快照合成。

**问题**

1. **mmap 边界检查系统性缺失**（`✅`）：`WtDataReader.cpp:341` 在 `_size==0` 时 `_ticks + (_size-1)` 下溢；`:353` 在 `pTick==begin` 时 `pTick--` 前越界；`WtRdmDtReader.cpp:1920` 的 `memcpy(..., (newSize-oldSize+1))` 在 `oldSize==0` 时多拷 1 条，直接写穿堆（同文件 `:2327` 的写法是正确的）。
2. **扩容失败不判空**（`✅`）：`WtDataWriter.cpp:1453` 直接使用 `resizeRTBlock` 的返回值，而该函数在写文件/映射失败时返回 `NULL`；磁盘满/映射失败即空指针崩溃。
3. **退出只 join 一个线程**（`✅`）：`release()` 只 join `_proc_thrd`（`:173`），`_proc_chk`（`:164` 创建）未 join，成员析构时 joinable thread → `std::terminate`。
4. **坏块不校验**（`○`）：`WtDataReader.cpp:1496` 等 5 处不检查 `proc_block_data` 返回值，压缩块损坏时会把压缩字节当 bar/tick 返回。
5. **没有文件头版本/校验**（`◐`）：格式靠“分支猜测”，没有 magic/version/checksum，也没有离线校验工具。

### 3.6 网关层

| 模块 | 接口完整度 | 重连 | 对账 | 线程安全 | 备注 |
|---|---|---|---|---|---|
| ParserCTP/Mini/Opt/Femas | 全 | SDK 自动，重登后重订阅 | 无（行情） | 单 SDK 回调线程 | **登录错误码全吞** |
| ParserXTP | 全 | 自建 2s，失败 terminate | 无 | 单回调线程 | DEBUG 日志硬编码 |
| ParserOES | 全 | 无，断线不复位 | 无 | SDK 异步 | UDP 模式重连不再订阅 |
| ParserHuaX/YD | 全 | 自建（HuaX 失败 terminate） | 无 | 单回调线程 | YD 用 TradingDay 当日期 |
| ParserUDP/XeleSkt/Shm | 全 | 无/线程不停 | 无 | 单线程 | `release()` 为空 |
| TraderCTP/Mini/Opt/Femas | 最高（quote 仅 CTP/Opt） | SDK 自动 | 4 查询 + 1s 限流，CTP/Opt 结算确认 | 单查询线程 | Mini/Femas 无结算接口（上游限制） |
| TraderXTP/XAlgo/OES/HuaX/YD | 无 quote/结算 | 自建，部分 terminate；OES 无 | 4 查询无限流 | asio/SDK 线程 | 状态机 `TS_*` |
| TraderATP/HTS/DD/Mocker | 无 quote/结算 | ATP SDK 重连；HTS/DD 无 | 4 查询（HTS 无锁） | 各有 worker | **DD 无法编译；本组多未纳入 Linux CMake** |

**关键缺陷**

- 4 个 Trader 用局部 `StdThreadPtr` 承载重连（`TraderDD.cpp:416`、`TraderHuaX.cpp:501`、`TraderXTP.cpp:671`、`TraderXTPXAlgo.cpp:885`），函数返回时 `shared_ptr<std::thread>` 析构一个 joinable thread → `std::terminate`。
- 4 个 Parser 的 `release()` 未停止并 join 线程（`ParserUDP.cpp:114`、`ParserShm.cpp:83`、`ParserXeleSkt.cpp:117` 为空实现；`ParserXTP.cpp:136` 只 `Release` API、不 join `_thrd_worker`），`ParserAdapter` 随后 `_remover(_parser_api)`（`WtDtCore/ParserAdapter.cpp:264`），IO 线程继续访问 `this`。
- `ParserCTP::IsErrorRspInfo` 恒 `false`（`:429`），`OnRspUserLogin` 仍据此判成功（`:191`），登录失败会按自然日设交易日并继续订阅。
- `TraderCTP.cpp:353` 的 `m_orderRef.fetch_add(0)` 在 userTag 为空时复用 OrderRef，可能被柜台拒单或撤错单。
- `TraderDD.cpp:902` 有一个全角 `）;`，该模块当前**无法编译**。

### 3.7 运行器、策略工厂与工具

- `WtRunner` 额外链接 `WtOptionCore`（见 A1），无实际符号引用；
- `WtUftRunner` 只链 `WtUftCore/WTSTools/WTSUtils`，策略以 `.so` 插件加载；
- `WtPorter`/`WtBtPorter`/`WtDtPorter` 用 C 接口导出，`WtOptionCore` 也提供了 `test_ctg_ranking`/`test_enhancements` 两个独立测试可执行文件，但没有注册到 CTest；
- `WtQuoteProbe` 是探针遗留目录：34 个被跟踪文件、22.6 MB，含两份重复的 CTP SDK 头（各 4.1 万行）、`.so`、PDF 和 17 MB 日志，不属于框架产物。

### 3.8 测试体系

| 指标 | 实际 |
|---|---|
| `TEST/TEST_F` 宏 | 215 个，分布在 42 个 `.cpp` |
| 覆盖对象 | 约 80% 是 `WtFutuCore`（报价数学、状态机、配置契约） |
| 核心框架覆盖 | `WtCore`/`WtBtCore`/`WtDataStorage`/网关：接近 0 |
| 运行方式 | 手工执行 `TestUnits` 二进制；`main.cpp:8` 有 `getchar()` |
| CTest | 仅 `WtOptionCore` 注册了 `enable_testing()`，且未与主测试统一 |
| CI | 无 `.github/`、无 Jenkinsfile |
| 已知环境依赖 | `test_shm.cpp:6` 硬编码 `E:\deploy_uft\uft_test\.share`；`test_object_pool.cpp` 的 TEST 全被注释 |
| 基线一致性 | README 写 113/113；`AGENTS.md` 记录 212/214；实际 215 个宏 |

测试不是“没有”，而是“有量无网”：它保护了策略层，却没有保护最容易出资金事故的订单/持仓/撮合/存储路径。这是当前投入产出比最低的一块。

---

## 4. 业务逻辑问题

本节按“交易链路”而非“目录”组织，便于判断风险传导。

### 4.1 订单与成交

#### 4.1.1 成交回报无幂等（P0）✅

`TraderAdapter::onPushTrade`（`WtCore/TraderAdapter.cpp:2234` 起）在把成交量累加进 `_positions` 之前，没有任何“该 tradeID 是否已处理”的判断；订单侧有 `_orderids` 去重（`:2044-2048`），成交侧没有。

- **触发条件**：CTP 重连后的成交查询、柜台重复推送、同一成交被多个通道回报。
- **后果**：`l_newvol/s_newvol` 重复累加，持仓与资金错账；策略看到虚假仓位后平仓/反手，可能超仓。
- **修复范式**：按 `(交易日, TradeID)` 建 bounded LRU/ring 去重，命中即只记 debug 日志；去重必须在任何持仓/资金变更之前。

#### 4.1.2 撤单重复归还可平量（P0）✅

`onPushOrder` 用 `_orderids` 判断“是否首次推送”：首次推送扣减可平，后续推送走 `else if (WOS_Canceled && 非开仓)` 分支把 `volume - volTraded` 加回 `l_preavail/l_newavail`（`:2126-2147`）。

- **触发条件**：同一撤单终态被重复推送；或 `onRspOrders` 预登记了订单号（`:1726`），使真实“首次推送”被当成“后续推送”。
- **后果**：可平量虚高 → 卖出平仓被柜台判超仓（废单），或买入平仓被当成开仓，风险敞口反向扩大。
- **修复范式**：不要用“第几次推送”做加减法，改成**订单级冻结台账**：下单/首次见到时冻结 `volume`，成交按 `volTraded` 释放，撤单/拒单按 `volume - volTraded` 释放一次，键为 `orderID`，终态后保留“已释放”标记。`WtUftCore/CloseFreezeLedger.h` 已经是这个模型，可直接复用。

#### 4.1.3 订单号/引用复用（P1）✅

`TraderCTP.cpp:353`（Mini `:343`、Opt `:496/626/757` 同构）在 `userTag` 为空时用 `m_orderRef.fetch_add(0)`：序号不递增，连续报单会复用同一 `OrderRef`。

- **后果**：柜台拒单（重复报单引用），或回报关联到错误订单，撤单撤错。
- **修复**：`fetch_add(1)`；同时保证 `userTag` 为空时的本地 id 生成是原子的（`TraderAdapter.cpp:44` 的 `_auto_order_id` 有 check-then-act 竞争）。

#### 4.1.4 订单表/查询风暴（P2）○

`_orderids`、`_trade_refs` 只增不删（按交易日清理），`getOrders()` 每次 `new OrderMap` 全量复制，`onPushTrade` 每笔成交触发一次 `queryAccount()`。高频策略下会打爆 CTP 查询流控（1 秒 1 次）。

- **建议**：日内滚动清理；快照复用；成交后的账户查询做 200–500ms 合并节流。

### 4.2 持仓与资金

#### 4.2.1 持仓容器跨线程无锁（P0）◐

`WtEngine::do_set_position` 在行情线程里 `_pos_map[stdCode]` 插入并只对单个 `PosInfo` 加 `SpinLock`（`WtEngine.cpp:951-957`）；`update_fund` 在任务线程里无锁遍历整个 `_pos_map`（`:199-203`）。

- **后果**：`std::unordered_map` 并发插入 + 遍历是 UB，可能崩溃、迭代器失效或读到半更新对象；`_dynprofit` 的读写也没有同步。
- **修复**：持仓只允许一个 owner 线程写；跨线程读用不可变快照（`shared_ptr<const PositionSnapshot>` 原子交换），或按合约分片加锁且绝不删除条目。

#### 4.2.2 理论仓在信号时刻入账（P1）○

`CtaStraBaseCtx::do_set_position`（`CtaStraBaseCtx.cpp:1438-1470`）在策略发出目标仓时就写 `_volume` 并计手续费/PnL，不等成交回报。

- **后果**：策略的 `stra_get_position`/`stra_get_fund_data` 与柜台脱节；部分成交、拒单、未成交都不可见，回测与实盘在同一策略代码下产生系统性偏差。
- **建议**：区分“目标仓”与“实际仓”；PnL/手续费只在 `on_trade` 入账；`stra_get_position` 默认返回实际仓，另提供 `stra_get_target_position`。

#### 4.2.3 资金/手续费口径（P2）○

`WTSContractInfo.hpp:133` 的 `calcFee` 返回 `(int32_t)(ret*100+0.5)/100.0`：大额费用 `ret*100` 可能溢出 `int32`，且小于 0.005 的费用被抹零；`decimal::eq` 用绝对 `1e-6`，对高价合约不适用。

- **建议**：金额用 `int64` 分或 `double` + 明确舍入规则；浮点比较改相对误差；补边界单测。

### 4.3 风控与平仓

#### 4.3.1 closeout 拒单仍置在途（P0）✅

`CloseoutExecutor` 调用 `submitHedgeOrder(...)` 后无条件 `_inflight_qty = batch_qty`（`CloseoutExecutor.cpp:386-389`）。`submitHedgeOrder` 返回 `void`（`.h:198`），内部对 `rejected/rate_limited/self_trade_blocked` 只打 warn（`:562-590`）。

- **后果**：一张单都没发出去，状态机却进入“等成交”，只能等 drain 超时或 2× 硬截止；盘中风控/收盘减仓时，仓位可能过夜（`AGENTS.md` 记录过 08-31 同类事故）。
- **修复**：`submitHedgeOrder` 返回 `OrderSubmitResult`/`bool`；只有 `localids` 非空才置 `_inflight_qty`；失败立即回 `RETRYING` 并计数，连续失败升级告警。

#### 4.3.2 自成交防护会拦掉平仓单（P1）✅

`OrderRouter::submitBuy/Sell` 对所有来源统一调用 `checkSelfTrade`（`OrderRouter.cpp:60-68/96-101`），`CloseoutExecutor` 走同一入口。当平仓卖价落在 bid1、而 MM 买单尚未撤销（撤单 ack 未到、zombie 闩锁期）时，平仓单被丢弃，只打一行 warn。

- **修复**：`CLOSEOUT`/`RISK_REDUCE` 来源跳过 STP，或强制“先撤 MM 单、确认终态后再发平仓单”，并把“被 STP 拦截的平仓单数”计入 KPI。

#### 4.3.3 风控闸门钝化（P1）○

- `maxDeltaChangePerSec` 的 32 槽环配 10 秒窗口，逐 tick 采样，速率被低估 2–10 倍；
- `hedge_ratio` 首帧只要 `beta>0` 就落库，没有样本数/变化率闸门（`FutuPortfolio.cpp:302-316`）；
- `_section_break_active` 全局 bool 与逐合约窗口混用；
- `cancelRetryIntervalMs=0` 静默关闭 B+ 保护。

这些不是“阈值调参”问题，而是**闸门本身的测量/作用域错误**，会让风控在压力场景下失效。

### 4.4 报价与信号

- **报价链**：`SpreadOptimizer → QuotePolicyChain → FutuQuoter` 的结构清晰，公式与配置来源在 `handoff.md` 中有完整记录，这是本项目最好的知识沉淀之一。
- **问题**：quote 计算在 tick 路径上多次 `std::string` 拼接/哈希（`StrategyCoordinator.cpp:1226/1387/1907` 的 `[tc.code]`），`OrderRouter` 每次 STP 都构造 `std::string(code)`（`:433-445`），日志洪泛时进一步放大延迟。
- **建议**：热路径统一 `string_view`/预分配 key；日志用 `HotLogGate` 限流；对 `FutuQuoter` 锁内发单做“决策在锁内、发单在锁外”的拆分。

### 4.5 期权定价、对冲与组合单

已在 3.4 详述，这里给出业务链路视角的影响：

```text
行情 → IV 求解 → 理论价/希腊字母 → 报价 → 组合单(leg1/leg2) → 对冲 → 到期结算
         ↑  NaN→1%        ↑ theta 语义错      ↑ 残腿/孤儿单      ↑ 手数×300
```

- **定价层**：数学正确，但数值守卫不足（NaN、零波动、IV 无解），会把“算不出来”变成“按 1% 报价”。
- **组合层**：把“已发送”当“已完成”，是典型的**状态机建模错误**；正确模型应跟踪 `leg1_filled/leg2_filled/leg1_terminal/leg2_terminal`，只有两侧都终态且敞口为 0 才 `done`，任一腿被拒要撤另一腿并回报 `FAILED`。
- **对冲层**：`delta_per_lot` 应作为显式配置（默认取合约乘数 × |fut delta|），并检查在途对冲单；`cooldown` 应基于“上次成交/上次终态”，而不是“上次发单”。

### 4.6 回测撮合

| 问题 | 证据 | 影响 |
|---|---|---|
| 非确定性 | `HftMocker.cpp:57/87/108` 的 `srand(time/min)` | 同配置复跑结果不同，参数寻优失效 |
| 买盘量用错字段 | `MatchEngine.cpp:206-210` `volume = askqty(i)` | 排队/成交量系统性失真 |
| `lower_bound` 返回 end 后解引用 | `MatchEngine.cpp:217-224` | 边界行情崩溃 |
| 无涨跌停/价格带 | `CtaMocker.cpp:1601-1668` 无校验 | 板上单、越界价照样成交 |
| 无成交量约束 | `CtaMocker::do_set_position` | 大单在流动性不足时也全额成交 |
| 空作用域锁 | `HftMocker.cpp:346-348`、`UftMocker.cpp:230` | 临界区实际无保护 |
| 理论仓即入账 | `CtaStraBaseCtx.cpp:1438-1470` | 回测/实盘同源偏差 |

**建议的验收标准**：同一配置连续跑 3 次，trades/funds/positions 的 MD5 一致；引入“涨跌停锁定 + 可成交量上限 + 限价单价格区间”三个约束；建立 `baseline/` 黄金输出与断言脚本（EXIT/HALT/Delta）。

### 4.7 行情与数据

- `ParserYD.cpp:131` 用 `TradingDay` 当 `actDate`，夜盘会被前移一天；
- `WtDtServo/WtDataManager.cpp:254` 在循环里用 `rawData->at(0)` 算 barTime，导致复合周期 K 线增量永不更新；`:313` 空 bars 时 `size()-1` 下溢；
- `ParserUDP.cpp:185-189` 写入时去掉了交易所前缀，长度却按原始 `code.size()` 累加，中间留 `\0`，服务端按 C 串解析时每个包只有首个合约生效；
- `WtDtCore/ShmCaster.cpp:52` 的 `_queue->_writable++` 非原子且无溢出丢弃策略。

### 4.8 生命周期与退出

这是本次审阅中**最集中、最可复现**的一类崩溃：

| 场景 | 代码 | 结果 |
|---|---|---|
| 柜台连不上 | 局部 `StdThreadPtr`（4 个 Trader） | 作用域结束 → `std::terminate` |
| 行情源切换/退出 | Parser `release()` 不 join 线程（4 个 Parser） | IO 线程访问已释放对象 → UAF/崩溃 |
| CTA/SEL 引擎析构 | `~WtCtaEngine` 不 `stop()`；`~WtCtaRtTicker` 为空 | ticker 线程未 join → terminate |
| 任务线程 | `_terminated` 从不置真 | 线程永不退出，进程退出时泄漏/abort |
| 数据写入器 | `release()` 只 join `_proc_thrd` | `_proc_chk` 析构 → terminate |
| TraderCTP 关停 | 先置 `m_pUserAPI=NULL` 再 Release，查询线程仍解引用 | 关停期崩溃 |
| TraderATP 退出 | `release` 死等 `_close_flag` | 退出挂死 |

**统一修复原则**：所有线程必须是“可停止 + 可 join”的成员；`release()/析构` 先置停止标志、再唤醒、再 join、最后释放资源；禁止在回调线程里 `delete this`；对第三方 SDK 的 `Release()` 要保证“停止回调 → 排空队列 → 释放对象”的顺序。

---

## 5. 代码质量与性能

### 5.1 体量与复杂度

**最大的源文件**（不含 gtest / API / 探针）：

| 文件 | 行数 | 评价 |
|---|---:|---|
| `WtBtCore/HisDataReplayer.cpp` | 4,571 | 历史数据加载 + 缓存 + 复权 + 回放 + 状态落盘，职责过多 |
| `WtCore/TraderAdapter.cpp` | 2,439 | 订单路由 + 持仓台账 + 查询 + 统计 + 自成交 + 落盘 |
| `WtUftCore/TraderAdapter.cpp` | 2,544 | 同上，与 WtCore 版本高度相似 |
| `WtOptionCore/HftOptionStrategy.cpp` | 2,417 | 配置 + 6 个命令处理器 + 对冲 + 扫描器 + 组合单生命周期 |
| `WtOptionCore/CompositeOptionPricer.cpp` | 1,760 | 定价/波动率/相关性/风险指标混在一起 |
| `WtFutuCore/StrategyCoordinator.cpp` | 1,970 | 编排 + 报价 + 风控 + 落盘 + KPI |
| `WtFutuCore/FutuRiskMonitor.cpp` | 1,392 | 多类风控闸门 + 状态 + 统计 |
| `WtDataStorage/WtRdmDtReader.cpp` | 2,651 | 多格式解析 + 复权 + 缓存 |

**判断标准**：单文件 >1,500 行或单类 >30 个成员函数，基本可以认定为“多职责聚合”。这些类不是“写得乱”，而是“改动半径不可控”——修一个风控分支可能影响回测复现、日志格式、落盘字段。

**建议**：不做大规模重写，按“变更频率 × 事故频率”拆分：

1. `TraderAdapter` → `OrderRouter` + `PositionLedger` + `TradeStats` + `QueryScheduler`；
2. `StrategyCoordinator` → `QuoteOrchestrator` + `RiskFacade` + `PersistenceSink` + `KpiReporter`；
3. `HftOptionStrategy` → `OptionStrategyShell` + `OptionCommandHandlers` + `HedgeCoordinator`；
4. `HisDataReplayer` → `DataLoader` + `BarCache` + `ReplayClock`。

### 5.2 内存安全

| 问题 | 证据 | 风险 |
|---|---|---|
| 固定 32 字节 tag + `strcpy` | `CtaMocker.cpp:1659/1758` + `CtaMocker.h:283`；`CtaStraBaseCtx.cpp:435-436` | 策略传入长 userTag 即堆/栈溢出 |
| `strcat` 无界 | `WtLocalExecuter.cpp:254` | 同上 |
| 日志缓冲无上界 | `WTSLogger.h:198` 的 `fmtutil::format_to` 写 2048 字节 `thread_local` 缓冲 | 长消息/格式化膨胀即溢出 |
| Parser 日志 512 字节 | `ParserCTP.cpp:32` | 同上 |
| mmap 越界 | 见 3.5 | 崩溃/数据损坏 |
| 裸指针 `new/delete` | `WtCore` 66 处 `new`、3 处 `delete`；`WtDataStorage` 59/20 | 泄漏/重复释放风险 |
| `shared_ptr` 过度使用 | `WtOptionCore` 210 处 | 原子引用计数开销 + 生命周期难追踪 |

**修复优先级**：先把所有“外部输入 → 固定缓冲”的路径改成 `std::string` 或 `snprintf` 截断；日志缓冲加 `format_to_n`；`userTag` 在 API 入口做长度校验（框架层统一，不要指望策略自觉）。

### 5.3 并发原语使用

| 指标 | 数值（约） | 观察 |
|---|---:|---|
| `WtFutuCore` `std::atomic` | 91 | 数量多但没有统一的内存序规范 |
| `WtFutuCore` `RecursiveSpinGuard` | 155 | 自旋锁在发单/日志路径上持有时间不可控 |
| `WtFutuCore` `SpinLockGuard` | 59 | 同上 |
| `WtFutuCore` `mutex/recursive_mutex` | 24 | 与大锁/自旋锁混用，锁序未文档化 |
| `WtCore` `std::mutex` | 0 | 主要靠 `SpinLock` 和“约定” |
| `WtUftCore` `std::mutex` | 11 | 与 UFT 单线程假设冲突时无断言 |

典型反模式：

- **空作用域锁**：`HftMocker.cpp:346-348`、`UftMocker.cpp:230`；
- **手动 lock/unlock**：`HftMocker.cpp:157/171`，异常即死锁；
- **锁内发单**：`FutuQuoter.cpp:227→268`；
- **非原子共享指针**：`FutuQuoter.h:355` 的 `_ctx` 由 Md 写、Td 读；
- **全局 static 状态**：`HaltGate.h:103`、`HaltReason.h:108`、`WtCtaTicker.cpp:84-87`；
- **线程对象不 join**：见 4.8。

**建议**：在 Debug 构建引入 “owner thread id” 断言；把 `SpinLock` 的持有时长纳入 KPI（`recordQuoteLockHold` 已有基础）；对 `atomic` 明确 `memory_order` 并在 code review 中检查。

### 5.4 性能热点

以下不是“微优化”，而是会在压力场景放大的结构性问题：

1. **tick 路径堆分配/字符串**：`StrategyCoordinator` 每 tick 多次 `[tc.code]`、`OrderRouter` 每次 STP 构造 `std::string`、`UnifiedOrderTracker::track` 每单复制 code。
2. **日志洪泛**：`WtFutuCore` 有 386 处 `WTSLogger::`，其中 `StrategyCoordinator` 45 处；部分 warn 在每 tick 触发（如 “skip stale/pending cancel”）。
3. **查询风暴**：`TraderAdapter::onPushTrade` 每笔成交 `queryAccount()`（`:2351`），CTP 查询 1 秒限流，会阻塞查询线程。
4. **自旋锁内做 I/O**：`FutuQuoter` 锁内 `stra_cancel`；`CloseoutExecutor` 在锁内做风控判断 + 发单。
5. **`unordered_map` 在热路径**：`_pair_states`、`_contract_states`、`_positions` 等以 `std::string` 为键，每 tick 查找/插入。
6. **回测内存**：`HisDataReplayer` 全量缓存 + `WtDataReader` 的 block 复制，`_ec_5d` 单次回测内存占用可达数 GB（`dist` 已 17 GB）。

**建议**：先做**量化**（`PerformanceMonitor` 已有延迟环），再优化；优先处理“每 tick 一次”的分配与日志，其次是锁粒度。不要在缺少基准的情况下引入 `-march=native`/LTO 这类“不可复现的优化”。

### 5.5 重复、死代码与类型问题

| 类型 | 例子 | 建议 |
|---|---|---|
| 双份实现 | `WtCore/ShareManager.*` 与 `WtUftCore/ShareManager.*`（各 259+137 行） | 抽公共库 + 抽象 sink |
| 网关复制粘贴 | 8 个 Trader 的登录/查询/重连逻辑 | 抽 `TraderBase` |
| 死代码 | `WtUftEngine::get_tick_slice/get_kline_slice` 直接返回 NULL（`:100-104`） | 实现或显式报错 |
| 未使用 API | `OrderRouter::submitExitLong/Short` 零调用且缺 `price<=0` 守卫（`:129-146`） | 删除或补齐 |
| 未使用文件 | `ConstantVolCurve.h` 全仓无引用 | 删除 |
| 注释掉的测试 | `test_object_pool.cpp` 的 TEST 全注释 | 删除或恢复 |
| 类型语义 | `thetaPerDay(double spot, double maturity)` vs 实现 `(rate, vol)` | 用强类型/命名区分 |
| 魔法数 | `0.130` 超时、`5/3.0/1e-4`、`0.30/0.50/0.05` EMA | 提常量 + 配置 |
| 重复枚举 | `QuoteMode` 在 `optioncoretypes.h:22` 与 `CompositeOptionPricer.h:115-119` 各一套 | 合并 |

### 5.6 错误处理

**模式一：吞掉返回值。** `WtDataReader` 5 处忽略 `proc_block_data`；`WtDataWriter` 忽略 `resizeRTBlock` 的 NULL；`DataManager::init` 在符号缺失时仍调用 `_writer->init`（`DataManager.cpp:69-82`）。

**模式二：用 warn 代替失败。** closeout 拒单、STP 拦截平仓、`maxOrders` 超限、配置键未读取，都只打一行日志，调用方无从判断“这单到底发没发”。

**模式三：`catch(...)` 兜底。** `OptionPricer2.cpp:673-679` 把 IV 无解整档丢弃；CTP 回调里 `throw`（`CTPLoader/TraderSpi.cpp:200`）会穿透 C SDK 导致 terminate。

**模式四：NaN/Inf 静默。** `std::max(0.01, NaN)` 返回 0.01；风控比较遇到 NaN 恒 false，等于风控失效。

**建议**：建立“错误必须可观测”的规则——所有 `bool`/指针返回值必须检查；所有静默降级必须计数并在 KPI 行暴露；所有 `catch(...)` 必须记录 `what()` 与上下文；所有 `double` 结果在进入风控/报价前做 `std::isfinite` 检查。

---

## 6. 工程治理

### 6.1 安全与合规（最高优先级）

**已确认的泄露面**（本报告不复述具体凭据值）：

| # | 位置 | 内容 | 状态 |
|---|---|---|---|
| 1 | `src/WtQuoteProbe/log_order_*.log`、`log_qry_czce_full.log` 等 17 个日志 | 实盘账号、经纪商代码、前置地址、AppID/授权码尾号 | **已被 git 跟踪**，18.1 MB |
| 2 | `scripts/deployment/deploy-futu.sh:65-67` | 生产主机 `ubuntu@…`、明文 SSH 密码、`sshpass` 用法 | **已进入 git 历史**（`git log -S` 命中） |
| 3 | `dist/WtRunnerFutu/{mdparsers,tdtraders,config}.yaml`、`dist/WtBtFutu/config_simnow.yaml`、`config_ec_live.yaml` | CTP 账号/密码/经纪商 | 未被跟踪，但随部署包扩散 |
| 4 | `dist/WtRunnerFutu/monitor/runMonSvr.py` | 默认监听 `0.0.0.0`、无 token | 未跟踪，部署后暴露 |
| 5 | `dist/WtRunnerFutu/monitor/init_monitor_db.py:97` | 无盐 MD5 存密码 | 未跟踪 |

**处置建议（按顺序）**：

1. **立即轮换**：CTP 账户密码、SSH 密码/密钥、监控系统口令；不要假设“删了文件就安全”。
2. **清史**：`git filter-repo` 删除日志与脚本中的凭据，强制推送并通知所有克隆方重新克隆；打 tag 记录清洗点。
3. **门禁**：pre-commit + CI 加 `gitleaks`/`trufflehog`；禁止 `*.log`、`dist/**`、`*.pem`、`*.key` 入库。
4. **配置外置**：所有凭据改环境变量/密钥管理（Vault/KMS/系统 keyring），配置文件只留 `${VAR}` 占位。
5. **权限收敛**：监控服务默认 `127.0.0.1` + token；口令改 bcrypt/argon2；部署脚本改 SSH key + `BatchMode`。

### 6.2 构建系统

| 问题 | 证据 | 影响 |
|---|---|---|
| CMake 版本声明失真 | `src/CMakeLists.txt:1` 写 3.0，`:54` 用 C++17（需 ≥3.8）；CMake 4 拒绝 <3.5 | 新环境无法配置 |
| 依赖路径写死 | `:17-18,40,46-47`（`%MyDepends141%`、`/home/mydeps`） | 换机/CI 失败 |
| GLOB 收源 | 40 个 `CMakeLists.txt` | 新增文件漏编，需手工重配 |
| 无 target 依赖 | `WtCore`/`WtUftCore` 无 `target_link_libraries`；`INCS/LNKS/LIBS` 全局变量 | 并行/增量构建不可靠 |
| 默认无警告 | 仅 `WtFutuCore`/`TestUnits` 有可选 `-Wall -Wextra`（默认 OFF） | 大量警告长期不可见 |
| 无 sanitizer 常态 | `build_asan` 靠手工 `CMAKE_CXX_FLAGS` | UB 长期潜伏 |
| 优化选项不一致 | `WtOptionCore` `-march=native -flto`；`WtFutuCore` 明确移除 | 产物不可移植/ABI 不一致 |
| Windows 双轨漂移 | 64 `.vcxproj` vs 66 `CMakeLists`；`all.sln` 72 工程但 0 个 `WtFutuCore/WtOptionCore` | Windows 无法构建主力模块 |

**建议路线**：

- 统一 `cmake_minimum_required(VERSION 3.20)`，加 `CMakePresets.json`（linux-gcc-debug/release、linux-asan、windows-msvc）；
- 每个库用 `target_sources/target_include_directories/target_link_libraries(PUBLIC/PRIVATE)`，源文件显式列表；
- 依赖用 `find_package(Boost 1.72 REQUIRED ...)` + 版本断言，或 Conan/vcpkg 锁文件；
- 默认 `-Wall -Wextra`（先不 `-Werror`），CI 里对新增警告做门禁；
- Windows 只保留 CMake，删除 `.vcxproj/.sln` 双轨，或由 CMake 生成。

### 6.3 依赖管理

- boost/spdlog/rapidjson/nanomsg/lmdb/zstd 以源码或系统库形式混用，没有统一版本清单；
- `src/API` 下 454 个券商 SDK 文件、625 MB，没有 SBOM/许可证清单；`LICENSE` 只有 MIT，未覆盖第三方 SDK 的使用条款；
- `deps/` 下包含预编译 `.a/.lib/.so` 与 CTP 模拟环境，来源/版本不可追溯。

**建议**：建立 `third_party/manifest.yaml`（名称/版本/来源/许可证/哈希）；发行包附 SBOM；把 SDK 从 `src/` 移到 `third_party/sdk/<vendor>/<version>/`，用 CMake `IMPORTED` target 引入。

### 6.4 测试与 CI

- **无 CI**：仓库没有 `.github/`、Jenkinsfile、GitLab CI；所有回归靠人工执行。
- **测试入口会挂起**：`TestUnits/main.cpp:8` 在 `RUN_ALL_TESTS()` 后 `getchar()`，CI 无 stdin 时永久阻塞。
- **环境耦合**：`test_shm.cpp:6` 硬编码 `E:\deploy_uft\uft_test\.share`，Linux 必挂；`test_object_pool.cpp` 的 TEST 全部注释。
- **基线失真**：README 写 113/113，`AGENTS.md` 记录 212/214，实际 215 个 `TEST` 宏。
- **回测基准不在库**：`_ec_5d.yaml`、`baseline_A/*.csv` 被 `.gitignore` 的 `dist/*` 排除，EXIT/HALT/Delta 判据没有脚本落地。
- **无覆盖率**：没有 gcov/llvm-cov 报告，也没有覆盖率门槛。

**建议的 CI（分阶段）**：

```yaml
pre-commit:  gitleaks, clang-format, cmake-lint, 文件大小门禁
build:       ubuntu-22.04(gcc-11/clang-15) × windows-2022(msvc) × cmake 3.22/4.x
             -Wall -Wextra；上传产物与 compile_commands.json
test:        ctest --output-on-failure；ASan/UBSan job；TSAN nightly（WtFutuCore+TestUnits 同开关）
backtest:    固定行情集跑 _ec_5d；脚本断言 EXIT/HALT/Delta 与 baseline；归档 trades/funds/positions
package:     Docker(digest 固定) + SBOM + license/secret 扫描 + 体积门禁
```

### 6.5 仓库卫生

| 指标 | 数值 |
|---|---:|
| 被跟踪文件 | 2,191 |
| 被跟踪总大小 | 792 MB（`du` 约 755 MiB） |
| 二进制文件 | 350 个 / 674 MB（**85.1%**） |
| 被跟踪日志 | 17 个 / 18.1 MB |
| 被跟踪但命中 `.gitignore` 的文件 | 144 个 |
| 工作区 `dist/` | 17 GB（未跟踪） |
| 工作区 `src/` | 7.7 GB（其中 `build_*` 约 4.9 GB） |
| `.git` | 339 MB |

**具体问题**：

- `.gitignore` 第一行 `dist/*` 与 75 个已跟踪的 `dist/` 文件冲突（历史强制添加），导致“到底该不该提交 dist”没有单一事实；
- `src/WtQuoteProbe` 34 个文件 / 22.6 MB：两份重复 CTP SDK 头、`.so`、PDF、17 MB 日志；
- `scripts/Rar.exe` 等工具二进制入库；
- `src/WtFutuCore/build/`、`src/build_*` 等构建目录散落在源码树；
- 无 Git LFS、无制品库、无体积门禁。

**建议**：`dist/` 与 SDK 移到制品库；源码树只保留代码/配置模板/脚本；用 LFS 或 submodule 管理必须版本化的二进制；CI 加“单文件 >5 MB 拒绝”。

### 6.6 文档与知识管理

**优点**：`docs/` 21 篇、约 5,400 行，覆盖架构、做市/套利、平仓、错误状态机、alpha 权重、回测分析；`handoff.md` 把“信号 → 报价”的公式与配置来源写得非常完整；`WtFutuCore/AGENTS.md` 记录了每次事故的根因、方案、验证与回滚，这在同类项目中很少见。

**问题**：

1. **流程文档膨胀**：`AGENTS.md` 6,178 行，已经从“工作规则”变成“变更日志 + 决策记录 + 事故复盘”。新人无法在合理时间内读完，关键约定被淹没。
2. **基线失真**：README 的 113/113、构建命令、目录清单与实际不一致（实际 215 个 TEST；`WtFutuCore/WtOptionCore` 不在 `all.sln`）。
3. **文档与代码漂移**：`docs/DEPLOY.md` 引用不存在的 `build_win.bat`/`build_linux.sh`，并要求 CMake 3.17+，与根 CMake 3.0 矛盾。
4. **缺少“当前事实”页**：没有一页说明“现在生产跑的是哪个版本/哪套配置/如何回滚”，知识散落在 AGENTS 的段落里。

**建议**：

- 把 `AGENTS.md` 拆成 `docs/adr/`（每篇一个决策，带状态/日期/替代方案）+ `docs/runbook/`（部署/回滚/排障）+ 一份 ≤200 行的 `CONTRIBUTING.md`；
- README 增加“自动生成”的模块/测试/构建矩阵，CI 校验；
- 每个版本打语义化 tag，部署包注入 `git describe --dirty` 与构建时间。

### 6.7 可观测性与运维

**做得好的**：

- `WTSLogger` 分类/动态日志，策略与 Runner 日志分离；
- `PerformanceMonitor` 延迟环 + 双边统计 + 积压落盘；
- `channel_watchdog.sh` 覆盖交易日时段、10 分钟去重、日志沉默检测，针对“进程活着但业务全黑”；
- `HotLogGate` 对热路径日志限流；
- KPI 巡检行（挂单、撤单、成交、延迟）。

**不足**：

- 没有统一的 `healthz`/metrics 端点（监控靠日志解析 + 独立 Python 服务）；
- 告警分级不明确：warn 太多，真正的“平仓单被拒/仓位过夜”没有独立告警通道；
- 版本可追溯性弱：二进制无版本号，`.so` 靠 md5 口头记录；
- 部署仍以 `sshpass` + `scp` 为主，没有灰度/回滚编排（AGENTS 里靠人工备份 `.bak_YYYYMMDD_xxx`）；
- 单实例约束靠 cron + 人工 `ps` 检查，缺少文件锁/端口锁的强约束。

---

## 7. 问题清单汇总

> 说明：`P0` = 资金损失 / 崩溃 / 安全 / 构建阻断；`P1` = 高风险或明显架构缺陷；`P2` = 可维护性、性能、治理。
> 复核列：`✅` 已逐行复核；`◐` 机制确认、影响待量化；`○` 审阅发现、待复核。

### 7.1 P0（14 项）

| ID | 问题 | 证据 | 影响 | 建议 | 复核 |
|---|---|---|---|---|---|
| P0-01 | 实盘凭据入库 | `src/WtQuoteProbe/log_order_*.log`、`scripts/deployment/deploy-futu.sh:65-67` | 账户/主机可被接管 | 轮换 → 清史 → 门禁 → 外置 | ✅ |
| P0-02 | 局部 `std::thread` 析构 terminate | `TraderDD.cpp:416`、`TraderHuaX.cpp:501`、`TraderXTP.cpp:671`、`TraderXTPXAlgo.cpp:885` | 连不上柜台即 abort | 存成员并 join 或 detach + 生命周期守卫 | ✅ |
| P0-03 | Parser 退出未 join 线程 → UAF | `ParserUDP.cpp:114`、`ParserShm.cpp:83`、`ParserXeleSkt.cpp:117`、`ParserXTP.cpp:136` | 退出/切换行情源崩溃 | stop io_service → join → 再删除 | ✅ |
| P0-04 | 数据写入器线程/扩容 | `WtDataWriter.cpp:1453`（NULL 未判）、`:164/173`（只 join 一个线程） | 磁盘满崩溃、析构 terminate | 判空 + join 全部线程 | ✅ |
| P0-05 | 后复权 K 线堆溢出 | `WtRdmDtReader.cpp:1920` `memcpy(..., newSize-oldSize+1)` | 写穿堆 | 改为 `newSize-idx`，补边界测试 | ✅ |
| P0-06 | tick 切片越界 | `WtDataReader.cpp:341`（`_size-1`）、`:353`（`pTick--`） | 开盘前/无 tick 时崩溃 | `_size==0` 与 `pTick>begin` 判断 | ✅ |
| P0-07 | CTP 行情错误码全吞 | `ParserCTP.cpp:429` + `:191` | 登录失败仍订阅，交易日错误 | 按 ErrorID 判定并上报 | ✅ |
| P0-08 | 成交回报无幂等 | `TraderAdapter.cpp:2234` 起 | 持仓/资金重复累加 | 按 `(日期, TradeID)` 去重 | ✅ |
| P0-09 | 撤单重复归还可平量 | `TraderAdapter.cpp:2126-2147`、`:1726` | 可平量虚高，超量平仓 | 订单级冻结台账（复用 CloseFreezeLedger） | ✅ |
| P0-10 | closeout 拒单仍置在途 | `CloseoutExecutor.cpp:386-389`、`:562-590` | 平仓空等，仓位过夜 | 返回提交结果，仅成功置 inflight | ✅ |
| P0-11 | 组合单残腿失联 | `ComboOrders.cpp:123-150`、`:81-85` | leg1 成交、leg2 拒单 → 裸敞口 | 两腿状态机 + 残腿撤销/重试 | ✅ |
| P0-12 | Delta 对冲手数单位错配 | `DeltaHedgeExecutor.h:61`、`HftOptionStrategy.cpp:359-368` | 超量对冲、反复叠单 | `delta_per_lot` 显式配置 + 在途检查 | ✅ |
| P0-13 | 持仓容器跨线程无锁 | `WtEngine.cpp:953` vs `:199-203` | `unordered_map` 并发 UB | 单 owner + 快照 | ◐ |
| P0-14 | 32 字节 tag + `strcpy` | `CtaMocker.cpp:1659/1758`、`CtaMocker.h:283` | 堆/栈溢出 | `snprintf`/`std::string`，入口限长 | ✅ |

### 7.2 P1（32 项）

| ID | 问题 | 证据 | 影响 | 建议 | 复核 |
|---|---|---|---|---|---|
| P1-01 | `TraderDD` 全角括号（仅 MSVC 构建） | `TraderDD.cpp:902` `"…"）;`；Linux CMake 未纳入该模块 | MSVC 构建失败 | 修标点并纳入 CI | ✅ |
| P1-02 | CTA/SEL 析构不停 ticker；`_terminated` 永不置真 | `WtCtaEngine.cpp:41-47`、`WtCtaTicker.h:36`、`WtEngine.cpp:43/1094` | 退出 abort / 线程泄漏 | 析构 stop+join；`atomic<bool>` | ✅ |
| P1-03 | 订单通知保留未释放 | `EventNotifier.cpp:218` | 每笔报单泄漏 | lambda 末尾 `release()` | ✅ |
| P1-04 | UFT 切片接口返回 NULL | `WtUftEngine.cpp:100-110` | 策略解引用崩溃 | 实现或显式报错 | ✅ |
| P1-05 | `get_session_info` 未判空 | `WtEngine.cpp:127-129` | 未知合约崩溃 | 判空降级 | ○ |
| P1-06 | 回测买盘量用卖量；`lower_bound` end 解引用 | `MatchEngine.cpp:206-224` | 撮合失真/崩溃 | 改 `bidqty` + 判 end | ○ |
| P1-07 | 回测无涨跌停/价格带/量约束 | `CtaMocker.cpp:1601-1668` | 回测虚高 | 三大约束 | ○ |
| P1-08 | 回测用墙钟种子 | `HftMocker.cpp:57/87/108` | 不可复现 | 配置 seed，默认固定 | ✅ |
| P1-09 | CTA 理论仓即入账 | `CtaStraBaseCtx.cpp:1438-1470` | 回测/实盘同源偏差 | 目标仓与实际仓分离 | ○ |
| P1-10 | 空作用域锁/手工 lock | `HftMocker.cpp:346-348`、`UftMocker.cpp:230`、`HftMocker.cpp:157/171` | 临界区无保护/死锁 | 删空锁，RAII | ○ |
| P1-11 | `ShareManager` 跨线程 + 2µs 忙轮询 | `ShareManager.h:103`、`.cpp:76/112` | 崩溃 + CPU 满载 | 加锁/快照 + 事件驱动 | ○ |
| P1-12 | `_orderids` 无界 + 每成交查账户 | `TraderAdapter.cpp:1726/2351/464-481` | 内存增长 + 查询流控 | 滚动清理 + 合并节流 | ○ |
| P1-13 | 计费/浮点/RingBuffer 析构 | `WTSContractInfo.hpp:133`、`decimal.h:15-25`、`RingBuffer.hpp:108-119` | 费用溢出/比较失效/双析构 | int64 计费、相对 eps、补析构 | ○ |
| P1-14 | `thetaPerDay` 语义错位 | `BlackCalc.h:49` vs `BlackCalc.cpp:190` | 希腊字母错误 | 统一签名/强类型 | ✅ |
| P1-15 | NaN 波动率被洗成 1% | `OptionPricer2.cpp:540` | 以错误波动率持续报价 | `isfinite` 检查 + 告警 | ✅ |
| P1-16 | IV 无解整档丢弃 + `catch(...)` | `OptionPricer2.cpp:673-679` | 可报价档位静默消失 | 分档降级 + 日志计数 | ○ |
| P1-17 | `thread_local` 裸指针缓存/返回引用覆盖 | `OptionPricer2.cpp:365-372`、`OptionRiskData.cpp:81-90` | 串档/覆盖 | 用 ID/版本键，返回值语义 | ○ |
| P1-18 | `ConfigValidator` 空转 | `ConfigValidator.h:78-82` | 拼写错误不告警 | 真正递归/显式 schema | ✅ |
| P1-19 | `-march=native` + LTO 进发行库 | `WtOptionCore/CMakeLists.txt:181-189` | 换机 SIGILL、ABI 不一致 | `x86-64-v3` 或目标矩阵 | ✅ |
| P1-20 | `WtRunner` 死链 `WtOptionCore` | `WtRunner/CMakeLists.txt:23` | 进程膨胀/部署污染 | 删除依赖或独立 Runner | ✅ |
| P1-21 | 策略穿透框架内部 | `UftFutuMmStrategy.cpp:21`、`.h:30`、`FutuRiskMonitor.cpp:14` | 升级即破坏 | 注入接口/适配层 | ✅ |
| P1-22 | 构建依赖隐式（GLOB/全局变量/SHARED 无依赖） | `WtCore/CMakeLists.txt:25`、`WtUftCore/CMakeLists.txt:22`、40 处 GLOB | 构建不可靠 | target 化 + 显式源列表 | ✅ |
| P1-23 | tracker 重复 id 无防御 | `UnifiedOrderTracker.cpp:33-51` | 槽位泄漏/pending 膨胀 | track 首行查重告警 | ✅ |
| P1-24 | `cancelRetry=0` 静默关闭 B+ | `UnifiedOrderTracker.cpp:353` | 僵尸单保护消失 | loader 校验下限 + 显式开关 | ✅ |
| P1-25 | `_section_break_active` 全局 bool | `StrategyCoordinator.h:500`、`.cpp:770-806` | 跨品种互相全撤 | per-contract 状态 | ○ |
| P1-26 | delta-rate 闸门被稀释 | `FutuRiskMonitor.h:644`、`:858` | 速率低估 2–10 倍 | 时间抽样 + 原子化 | ○ |
| P1-27 | `clearZombies` untrack 仍在场的单 | `UnifiedOrderTracker.cpp:584-600` | pending/敞口低估 | 残余量单独记账 | ○ |
| P1-28 | STP 拦平仓单 | `OrderRouter.cpp:60-68/96-101` | 平仓失败 | CLOSEOUT/RISK_REDUCE 豁免 | ✅ |
| P1-29 | `hedge_ratio` 首帧无闸门 | `FutuPortfolio.cpp:302-316` | 全链 delta 放大 | 样本数 + 变化率限制 | ○ |
| P1-30 | CTP `OrderRef` 不递增 | `TraderCTP.cpp:353` 等 | 重复引用/撤错单 | `fetch_add(1)` | ✅ |
| P1-31 | 零波动 gamma/vega 除零 | `BlackCalc.cpp:156-157`、`BlackCalc.h:20-24` | NaN/Inf 进入报价 | 波动率下限 + `isfinite` | ○ |
| P1-32 | 去大锁前 `_order_ids` 无保护 | `WtUftCore/UftStraContext.cpp:920`、`:610-631`；`_cb_mtx` 是唯一串行化者 | `FUTU_CB_LOCK_BIG=0` 即容器损坏/UAF | 先加锁/单线程化，再列为去锁前置判据 | ○ |

### 7.2.1 工程治理类 P1（10 项）

| ID | 问题 | 证据 | 影响 | 建议 | 复核 |
|---|---|---|---|---|---|
| P1-G01 | 构建声明/依赖不可复现 | `src/CMakeLists.txt:1,17-18,40,46-47`；40 处 GLOB；`WtCore/CMakeLists.txt:25` 无 link | 新机/CI 配置失败 | CMake 3.20+、Presets、target 化、依赖锁定 | ✅ |
| P1-G02 | 默认无警告、无 sanitizer 常态 | `WtFutuCore/CMakeLists.txt:40`（默认 OFF）、`TestUnits/CMakeLists.txt:51` | UB/警告不可见 | 默认 `-Wall -Wextra`；CI ASan/UBSan/TSAN | ✅ |
| P1-G03 | 构建矩阵双向漂移 | Windows：`all.sln` 缺 `WtFutuCore/WtOptionCore`；Linux：根 CMake 缺 9 个网关（ParserHuaX/OES/YD、TraderATP/DD/HTS/HuaX/OES/XTPXAlgo）；64 vcxproj vs 66 CMakeLists | 平台能力不一致 | CMake 单一源 + 平台矩阵 CI | ✅ |
| P1-G04 | 测试无法 CI 化 | `TestUnits/main.cpp:8` `getchar()`；`test_shm.cpp:6` `E:\`；无 `.github/`；README 113 vs 215 | 回归靠人工 | 删 getchar、参数化、ctest、CI | ✅ |
| P1-G05 | 仓库二进制化 + ignore 失效 | 2,191 文件 / 792 MB / 85% 二进制；144 个已跟踪文件命中 ignore；`dist` 17 GB | clone/审计困难 | LFS/制品库、重写历史、体积门禁 | ✅ |
| P1-G06 | 监控服务默认暴露/弱口令 | `runMonSvr.py` 默认 `0.0.0.0` 无 token；`init_monitor_db.py:97` 无盐 MD5 | 未授权访问 | 127.0.0.1 + token + bcrypt | ○ |
| P1-G07 | Docker 不可复现 | `docker/Dockerfile:7,17` FROM latest + clone master；`Dockerfile_ubt` 18.04 EOL | 重建结果不同 | 固定 digest/tag/包版本 | ○ |
| P1-G08 | 文档/基线漂移 | README 113/215；`docs/DEPLOY.md` 引用不存在脚本；`AGENTS.md` 6,178 行 | 按文档无法复现 | 自动生成 + ADR/Runbook 拆分 | ✅ |
| P1-G09 | 依赖/SDK 无清单 | `src/API` 454 文件 / 625 MB 无 NOTICE；boost/spdlog 无版本锁定 | 合规/升级风险 | manifest + SBOM + `find_package` | ○ |
| P1-G10 | 版本不可追溯 | 无 `VERSION` 文件；部署靠 md5 口头记录；无语义 tag | 线上无法定位 | `git describe` 注入 + tag | ○ |

### 7.3 P2（18 项，合并同类）

| ID | 问题 | 证据 | 建议 |
|---|---|---|---|
| P2-01 | 上帝类 | `HisDataReplayer` 4,571、`TraderAdapter` 2,439、`HftOptionStrategy` 2,417、`StrategyCoordinator` 1,970 | 按变更频率拆分 |
| P2-02 | 热路径字符串/哈希/日志 | `StrategyCoordinator.cpp:1226/1387/1907`、`OrderRouter.cpp:433-445` | `string_view` + HotLogGate |
| P2-03 | 重复实现 | `WtCore/ShareManager.*` vs `WtUftCore/ShareManager.*`；8 个 Trader 登录逻辑 | 抽公共基类 |
| P2-04 | 死代码/未用 API | `WtUftEngine.cpp:100-110`、`OrderRouter.cpp:129-146`、`ConstantVolCurve.h`、`test_object_pool.cpp` | 删除或补齐 |
| P2-05 | 重复枚举/魔法数 | `QuoteMode` 两套；`0.130`、`5/3.0/1e-4`、EMA 0.30/0.50/0.05 | 合并 + 常量表 |
| P2-06 | `FixedString24` 静默截断 | `AsyncArbitrageExecutor.h:64-74/105` | 截断 assert + loader 校验 |
| P2-07 | `maxOrders` 读了不执行 | `FutuModuleAssembler.cpp:312`、`UnifiedOrderTracker.h:337-341` | 执行上限或删键 |
| P2-08 | `avg_order_lifetime` 分母错 | `UnifiedOrderTracker.cpp:127-133` | 单独 completed 计数 |
| P2-09 | 数据工具边界 | `WtDtHelper.cpp:147`（`ay[2]`）、`WtDataReaderAD.cpp:222`（vector memcpy） | 判 size + 逐元素 |
| P2-10 | 硬编码路径 | `TraderOES.cpp:560`、`monitor` 数据目录 | 入配置 |
| P2-11 | 退出死等 | `TraderATP.cpp:970`、`TraderYD.cpp:560` | 超时 + join |
| P2-12 | 头文件未 install | `WtOptionCore` 13 个头未进 `OPTION_CORE_HEADERS` | 补全 install |
| P2-13 | `AttributePublisher` 只增不删 | `AttributePublisher.cpp:19`、`:93-116` | TTL/清理 |
| P2-14 | 测试空壳 | `test_enhancements.cpp:24-37`、`test_object_pool.cpp` | 改 gtest + 计数 |
| P2-15 | 仓库二进制化 | 792 MB / 85% 二进制；`dist` 17 GB | LFS/制品库 + 体积门禁 |
| P2-16 | `WtQuoteProbe` 污染 | 34 个跟踪文件 / 22.6 MB | 移出 `src/` |
| P2-17 | 许可证/SBOM | `src/API` 625 MB SDK 无清单 | 第三方 manifest + SBOM |
| P2-18 | 文档漂移 | README 113/215、`DEPLOY.md` 引用不存在脚本、`AGENTS.md` 6,178 行 | 自动校验 + ADR 拆分 |

### 7.4 统计

| 级别 | 数量 | 其中已逐行复核 |
|---|---:|---:|
| P0 | 14 | 13 |
| P1（业务/架构） | 32 | 16 |
| P1-G（治理） | 10 | 6 |
| P2 | 18 | — |
| **合计** | **74** | **35** |

> 未复核项并非“不存在”，而是需要结合现场日志/柜台上限/压测数据确认影响面。建议在阶段 1 开始前，把 `○` 项逐条转成可复现用例。

---

## 8. 改进路线图

### 8.1 优先级原则

1. **先安全与止血，再谈功能与性能**：凭据、崩溃、越界、错账优先于任何策略优化。
2. **先账本，再并发**：订单/持仓的幂等与生命周期正确之前，不要拆大锁、不要上双边报价。
3. **先可复现，再调参**：回测三次结果不一致时，任何参数结论都不采信。
4. **先补测试网，再重构**：没有回归网的拆分类重构，风险高于收益。
5. **不做大爆炸重写**：当前业务逻辑是资产，重构应“绞杀者模式”（strangler），按模块替换。

### 8.2 阶段 0：止血（0–1 周）

| 任务 | 具体动作 | 验收 |
|---|---|---|
| 安全处置 | 轮换 CTP/SSH/监控口令；`git filter-repo` 清史；加 gitleaks pre-commit | 全仓 secret 扫描 0 命中；历史不可检出 |
| 崩溃修复 | 4 个 Trader 线程改成员 + join；4 个 Parser `release()` stop/join；`WtDataWriter` join 全部线程 | 启动/停止循环 100 次不 abort |
| 越界修复 | `WtDataReader` 空判、`WtRdmDtReader` 拷贝长度、`resizeRTBlock` 判空 | ASan 跑回放/读取用例 0 报错 |
| 构建修复 | `TraderDD.cpp:902` 全角括号；删除 `TestUnits` 的 `getchar()`；`test_shm` 改参数化 | 全模块可编译；`ctest` 不挂起 |
| 快速正确性 | CTP `fetch_add(1)`；`submitExit*` 加 `price<=0`；`EventNotifier` 补 release | 相关单测/代码审查通过 |
| 基线校正 | README 测试数、构建命令、`all.sln` 说明 | 文档与 `ctest -N` 一致 |

**建议负责人**：1 名熟悉网关的工程师 + 1 名框架工程师，并行两条线（安全/构建 vs 线程/越界）。

### 8.3 阶段 1：资金安全（2–4 周）

| 任务 | 具体动作 | 验收 |
|---|---|---|
| 订单幂等 | 成交按 `(交易日, TradeID)` 去重；撤单/拒单按 `orderID` 恰好一次释放 | 重放“重复成交/重复撤单”注入测试，持仓与资金不变 |
| 持仓台账 | `TraderAdapter` 的 `_positions` 改单 owner + 快照；`WtEngine::_pos_map` 禁止跨线程遍历 | TSAN 跑 30 分钟 0 报告 |
| 平仓链路 | `submitHedgeOrder` 返回结果；仅成功置 inflight；CLOSEOUT 跳过 STP | 注入“拒单/流控/STP”三种失败，状态机回到 RETRYING |
| 组合单 | 两腿状态机；残腿撤销/重试；旧 id 保留列表 | 注入 leg2 拒单，leg1 被撤、敞口归零 |
| 对冲 | `delta_per_lot` 配置化；在途单检查；冷却基于成交/终态 | 1 手 ATM 多头只发 1 手对冲 |
| 订单槽 | `track` 查重；`cancelRetryIntervalMs` 下限 100ms 或显式开关 | 重复 id 注入不泄漏槽位 |
| 风控闸门 | delta-rate 改时间抽样；`_section_break_active` 改 per-contract；`hedge_ratio` 加样本/变化率闸门 | 回放历史行情，闸门触发次数与原设计偏差 <10% |

**验收基线**：单测新增 ≥20 个（覆盖重复回报、拒单、残腿、对冲手数、闸门边界）；`_ec_5d` 回测 Delta=0；无新增 HALT。

### 8.4 阶段 2：可信度（1–2 月）

| 任务 | 具体动作 | 验收 |
|---|---|---|
| 回测确定性 | 删除 `srand(time)`，改配置 seed；固定行情集 | 同配置连续 3 次 trades/funds/positions MD5 一致 |
| 撮合约束 | 涨跌停锁定、价格带、可成交量上限；修 `MatchEngine` 买盘量/end | 边界用例通过；回测收益回撤到合理区间 |
| 黄金基线 | `baseline/` 入库（配置 + 期望 CSV + 断言脚本） | PR 必须通过 baseline diff |
| CI | build/test/asan/tsan/backtest 五阶段；缓存依赖 | 主干绿灯；单次 <20 分钟 |
| 核心测试 | `TraderAdapter` mock `ITraderApi`；`CtaMocker`；`WtDataReader/Writer` | 核心模块行覆盖 ≥50% |
| 可观测性 | 平仓失败/仓位过夜/查询限流独立告警；二进制注入 `git describe` | 故障可在 1 分钟内定位 |

### 8.5 阶段 3：架构（3–6 月）

| 任务 | 具体动作 | 验收 |
|---|---|---|
| 构建现代化 | CMake 3.20+、Presets、target 化、显式源列表、依赖锁定 | 新机器 15 分钟内完成 build+test |
| 模块解耦 | 删除 `WtRunner→WtOptionCore` 硬链接（恢复 `./hft/` 插件加载）；两个策略只依赖 `Includes` 接口，不链接宿主引擎 | 依赖图无环；单模块可独立构建 |
| 上帝类拆分 | `TraderAdapter`/`StrategyCoordinator`/`HftOptionStrategy` 按职责拆 | 单文件 ≤1,000 行，单测可注入 |
| 并发统一 | owner-thread 断言、锁序文档、去大锁前置条件清单 | TSAN 常态化；`FUTU_CB_LOCK_BIG=0` 有验收报告 |
| 网关基类 | 抽 `TraderBase/ParserBase`；错误码统一映射 | 新网关接入 ≤300 行 |
| 仓库治理 | 移出 SDK/二进制/日志；LFS/制品库；SBOM | 仓库 <100 MB（不含 LFS） |

### 8.6 优先级矩阵（影响 × 成本）

```text
影响高 ┌────────────────────────────────────────────┐
       │ 凭据轮换/清史      │ 订单幂等/持仓台账      │
       │ 线程/解析器生命周期 │ 平仓/组合单/对冲       │
       │ 数据越界           │ 回测确定性 + 基线      │
       ├────────────────────┼────────────────────────┤
       │ CTP 错误码         │ 上帝类拆分             │
       │ 死链/全角括号      │ CMake target 化        │
       │ 测试 getchar       │ 并发模型统一           │
       └────────────────────┴────────────────────────┘
         成本低                成本高
```

**建议节奏**：阶段 0 与阶段 1 部分并行（安全线独立）；阶段 2 的 CI 越早建越好，它会让阶段 1 的修复“可验证”；阶段 3 在阶段 1/2 的测试网就位后再启动。

### 8.7 现在不建议做的事

- **不要先拆 `_cb_mtx` 大锁**：框架容器（`UftStraContext::_order_ids`）尚未线程安全，拆锁会把隐藏 UB 变成必现崩溃。
- **不要先做微优化**（`-march=native`、LTO、无锁队列）：当前瓶颈是正确性，不是纳秒。
- **不要大爆炸重写 `TraderAdapter`/`StrategyCoordinator`**：先用测试固定行为，再按职责绞杀。
- **不要继续给策略层堆单测而忽略核心框架**：215 个用例已经证明策略层覆盖不差，缺的是订单/撮合/存储的回归网。
- **不要把 `dist/` 当源码仓库**：部署物与源码分离，否则每次 `git status` 都在噪声里。

---

## 9. 附录

### 9.1 审阅方法与可复现命令

```bash
# 规模统计（排除 gtest / API）
find src -path src/API -prune -o -path src/WtQuoteProbe -prune -o \
     \( -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \) -print | xargs wc -l

# 仓库卫生
git ls-files | wc -l
git ls-files -z | xargs -0 du -ch | tail -1
git ls-files -ci --exclude-standard | wc -l

# 测试数量
grep -rho "TEST[_F]*(" src/TestUnits/*.cpp | wc -l

# 凭据扫描（示例，勿在报告中输出命中值）
gitleaks detect --no-git -v
git log -S"sshpass" --oneline -- scripts/deployment/deploy-futu.sh

# 线程对象语义
grep -n "typedef std::thread StdThread" src/Share/StdUtils.hpp
```

### 9.2 复核记录与存疑项

| 事项 | 处理 |
|---|---|
| “`ExpirationSimulator` 现金流与盯市重复计入” | **复核不成立**：`tradingPnl` 是累计现金流，`settlement/closePnl` 是当前仓位估值，合计为权益快照，`dayPnl` 取差分。已剔除。 |
| “`docs/MM_ARB_ARCHITECTURE.md`、`OPTIMIZATION_ROADMAP_V2.md` 不存在” | **不成立**：两文件均存在（252 / 707 行），审阅子任务的工作目录判断有误。 |
| “`WtDataWriter` 的 `_proc_chk` 未 join” | 已核对 `release()`（`:173`）只 join `_proc_thrd`；`_proc_chk` 为成员 `StdThreadPtr`，析构 joinable thread 触发 terminate。 |
| “`ParserUDP` release 为空导致 UAF” | 已核对 `release()` 空实现 + `_thrd_parser` 成员 + `ParserAdapter` 删除顺序；结论成立。 |
| “`TraderDD` 无法编译” | 已核对 `TraderDD.cpp:902` 全角 `）;`；该模块未纳入 Linux CMake（仅在 MSVC 工程中），因此不影响 Linux 主链路，但 Windows 构建会失败。 |
| `WtFutuCore` 的 `○` 项（PendingCommand、delta-rate、section break 等） | 需结合实盘日志/压测确认影响面，建议阶段 1 转成可复现用例。 |

### 9.3 指标口径

- **行数**：`wc -l`，含空行/注释；`src/API` 与 `WtQuoteProbe` 的 SDK 头单独说明。
- **二进制占比**：按文件扩展名与 `file` 类型判定，排除 `.csv` 等文本数据。
- **测试数**：`TEST/TEST_F` 宏计数，不含 `INSTANTIATE_TEST_SUITE_P` 展开的用例。
- **模块成熟度**：综合“功能完整度、测试覆盖、事故记录、代码复杂度”的主观评级，非精确度量。

### 9.4 审阅分工

| 范围 | 覆盖 |
|---|---|
| 核心引擎 | `WtCore`/`WtUftCore`/`WtBtCore`/`Share`/`WTSTools`/`Includes`/Runner |
| UFT 做市策略 | `WtFutuCore` 126 文件 / 38k 行 + AGENTS/README/CONFIG/docs |
| HFT 期权策略 | `WtOptionCore` 136 文件 / 25k 行 + CMake/tests |
| 数据与网关 | 数据层 7 模块 + 11 Parser + 14 Trader + 入口工具（其中 9 个网关仅 MSVC 构建） |
| 工程治理 | 构建/测试/运维/仓库/安全/文档 |
| 交叉复核 | 对 P0/P1 中 35 条逐行核对 `file:line` |

---

## 10. 结语

这个项目最值得肯定的地方，是**业务复杂度与知识沉淀**：做市报价链、delta/position 语义、B+ 订单槽、alpha/毒性、期权定价与 IV、事故复盘，都达到了可以支撑真实资金的水平；`handoff.md` 与 `docs/` 的记录密度在同类项目中很少见。

最需要改变的地方，是**把“人肉流程”换成“工程约束”**：

- 用 CI + 单测替代“改完跑一遍回测”；
- 用订单台账/状态机替代“加减法 + 约定”；
- 用 target 化构建 + 依赖锁定替代“全局变量 + GLOB”；
- 用 secret 管理 + 门禁替代“明文配置 + 手工部署”；
- 用 ADR/Runbook 替代 6,000 行的 `AGENTS.md` 流水账。

如果只能做三件事，建议是：**① 轮换并清除凭据；② 修完 14 个 P0；③ 建一条能跑单测 + ASan + 三次一致回测的 CI。** 这三件事完成后，项目就从“能跑”进入了“可运营”的门槛。

---

*报告完。本报告仅做静态审阅与建议，未修改任何源码；所有 `file:line` 均基于审阅时 HEAD `b1eab4fa`，后续代码变动可能导致行号偏移。*



