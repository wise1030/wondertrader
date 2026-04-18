# WtFutuCore - 期货高频做市核心模块 (深度诊断与架构重构版)

*本报告基于真实的 L2 盘口数据、高频订单流日志以及最新代码库结构进行深度剖析。重点诊断了系统中“数据源与计算逻辑耦合”的问题，并给出了从 P0 到 P2 级别的全维度演进路线图。*

---

## 1. 项目架构 (Project Architecture)

### 🚨 核心痛点：MarketDataContext 的“上帝对象”反模式
当前的 `MarketDataContext` 违背了单一职责原则（SRP），它不仅维护了 **状态驱动** 的订单簿切片（Level 2 Bids/Asks），还通过混入 `TickTransactionInferer` 强行接管了 **事件驱动** 的逐笔成交（Trade Flow/Transactions）历史记录。
这种“大杂烩”设计导致了：
1. **数据语义污染**：订单簿代表“未成交的静态挂单”，而 Trade Flow 代表“已成交的动态行为”。将它们强行捏合，导致 `ISignalSource::update(const MarketDataContext&)` 接口语义极其混乱。
2. **多合约路由撕裂**：除了数据模块，业务模块中的 `SpreadArbitrageManager`（套利）与 `FutuQuoter`（做市）也在平行抢夺底层资源，导致自我对敲和限额穿透。

### 🛠️ 重构建议：
1. **行情数据源二元拆分 (Data Source Bifurcation)**：
   - **`OrderBookState` (静态盘口)**：仅负责维护多档位 Bids/Asks，计算 Mid Price 和静态 Book Imbalance。
   - **`TradeFlowTracker` (动态成交)**：独立订阅 `onTransaction` 或 Tick Volume 增量，专职维护净主动买卖量（Net Flow）、大单统计（Large Trades）和 VPIN。
2. **统一行情上下文 (Unified Market Data Context)**：将拆分后的对象包装为 `MarketDataSnapshot`，作为 `ISignalSource::update` 的唯一入参。
3. **统一仓位调度器 (Unified Position Router)**：剥夺套利等附加模块直接下单的权利。套利逻辑退化为“信号产生器”输出预期仓位（Desired Delta），统一由做市引擎整合意图后发单。

---

## 2. 业务功能 (Business Functionality)

### 🚨 核心痛点：对冲成本与信号断层
1. **昂贵的对冲成本**：现在的 `checkAndHedge` 依然采用纯 Taker（市价吃单）模式抹平 Delta。高频跨越买卖价差（Bid-Ask Spread）带来的隐形成本足以吞噬做市利润。
2. **Alpha 信号跳跃**：当依赖高频交易数据的子信号（如 `TradeFlow`）因为交易所数据断层瞬间失效时，系统会突然降级使用 `BookImbalance`，导致 Alpha 预测值发生跳跃，引发报价疯狂震荡（Flickering Quotes）。

### 🛠️ 重构建议：
1. **被动做市对冲 (Maker Hedging)**：将对冲逻辑分级。Delta 轻微超限时，在锚定合约本方的买卖一档挂 Maker 单赚取价差对冲；仅当 Delta 逼近强平线（Critical）时，才切换为 Taker 紧急平仓。
2. **平滑指数衰减 (EWMA Alpha Decay)**：为 `SignalAggregator` 引入信号衰减。当高质量信号源失效时，Alpha 不应瞬间切换，而是遵循 EWMA（指数加权移动平均）平滑衰减至降级信号。
3. **主动毒性卸载 (Active Toxic Unloading)**：检测到同向强毒性且持有逆向持仓时，打破“不吃单”规矩，主动跨过价差进行市价斩仓。

---

## 3. 代码 Bug 与隐患 (Code Bugs)

### 🚨 核心痛点：高频热路径的内存与时序雷区
1. **动态内存分配 (Heap Allocation)**：在热路径 `SignalAggregator::computeAlpha` 内部，每次 Tick 都会创建 `std::vector<double>` 收集有效信号。每秒数万次的 new/delete 极易引发延迟毛刺（Latency Spikes）。
2. **挂单状态时序漂移 (Desync Risk)**：在高压网络下，交易所的成交回报（`onTrade`）和状态回报（`onOrder`）极易乱序或延迟到达，导致 `_order_tracker->getPendingBuyQty()` 统计的在途挂单虚高，阻碍后续的正常发单。
3. **极值除零风险**：`SpreadOptimizer` 计算持仓占比时，若开盘第一笔未给出合理的 `contract_lots` 或价格，会引发致命的除零崩溃。

### 🛠️ 重构建议：
1. **热路径零分配 (Zero-Allocation Hotpath)**：将所有的局部 `std::vector` 提升为类的私有成员变量，每 Tick 仅执行 `.clear()` 重置大小而不释放容量（Capacity），彻底消除堆内存分配。
2. **状态调和机制 (Reconciliation Loop)**：引入一个 500ms 一次的独立定时器，强行清退超时未响应的“幽灵挂单”，确保 Tracker 状态机绝对干净。
3. **严苛的 Epsilon 校验**：在所有涉及价格、交易量、权重的除法运算前，必须强制插入 `> 1e-6` 的容错拦截。

---

## 4. 性能提升 (Performance)

### 🚨 核心痛点：虚函数与重度数学计算
1. **虚函数派发拖累 (Virtual Function Overhead)**：`SignalAggregator` 持有基类指针 `ISignalSource`，`update` 和 `result` 均是 `virtual` 函数。高频虚表查询（vtable lookup）打断了 CPU 流水线预测，阻止了编译器的内联（Inline）优化。
2. **浮点运算臃肿**：大量的 `std::pow` 和 `std::exp` 被放置在了 Tick 驱动的定价环路中。

### 🛠️ 重构建议：
1. **去虚函数化 (Devirtualization)**：利用 **CRTP (Curiously Recurring Template Pattern)** 或 C++17 的 `std::variant` 加 `std::visit` 重构 `ISignalSource` 体系。将运行期的虚表查询提前到编译期决议，压榨最后 15% 的 CPU 周期。
2. **降级近似计算 (Fast Math Approximations)**：针对非关键性的指数或幂运算，引入预计算查找表（Lookup Table, LUT）或泰勒级数近似展开（Fast Math），用内存换取宝贵的 ALU 算力。
