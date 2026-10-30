# WtOptionCore 代码审查报告与优化方案

- **日期**: 2026-10-30
- **审查对象**: `src/WtOptionCore`
- **代码基线**: Git HEAD `d6825448`
- **代码规模**: 约 23.6k 行生产代码，另有约 1.9k 行测试代码
- **审查方式**: 当前源码只读静态审查；重点结论均对照当前代码路径与 `文件:行号`
- **未执行事项**: 本轮未修改源码、未重新构建、未运行测试；因此本文不将旧构建日志或 README 中的历史结论视为当前验证结果

> 路径说明：下文所有 `文件:行号` 均相对仓库根目录。行号基于当前 HEAD `d6825448`，后续提交可能漂移。

---

# 一、执行摘要

WtOptionCore 已经具备较完整的期权做市功能外形：

```text
行情/交易回调
  → OptionAsyncEventProcessor
  → OptionGrid / OptionTradingGrid
  → CompositeOptionPricer / GammaScalpOptionPricer
  → OptionRisk / Signals / Scanners
  → ControllableTradingGrid
  → OptionQuoteManager
  → IHftStraCtx
  → WtRunner / TraderAdapter / 柜台
```

模块划分、插件边界和异步 worker 的方向是合理的；但当前仍应定位为：

> **功能较丰富、正在工程化加固中的期权做市策略插件，而不是已经完成生产验收的完整低延迟交易系统。**

复核后确认，当前最重要的不是先做 SIMD、无锁队列或更激进的编译优化，而是先闭合五类基础契约：

1. **事件恰好一次**：异步队列不能重复处理成交/委托/持仓，也不能静默丢失控制事件。
2. **订单统一出口**：所有订单路径必须返回并登记真实 localid，且经过统一风控。
3. **持仓与合约事实单一事实源**：昨仓+今仓、tick、multiplier、fee 不能在多个模块中各自解释。
4. **时间与停机语义可靠**：跨午夜不能破坏 TTL/TPS/cooldown，STOP 必须真正撤单。
5. **异常状态闭环**：拒单、撤单失败、combo 部分成交、重启工作单和到期状态必须有明确状态机。

在上述问题闭环前，不建议把现有 README 或组件级单测解释为“生产链路已验收”。

---

# 二、复核口径与对上一轮结论的修正

## 2.1 结论分级

本文将结论分为三级：

| 级别 | 含义 |
|---|---|
| **确认** | 当前代码路径可直接推导，不依赖运行时猜测 |
| **条件触发** | 代码缺陷明确存在，但是否造成生产事故取决于配置、品种、交易时段或事件时序 |
| **工程建议** | 当前未必已经构成 bug，但会降低可维护性、可观测性或低延迟改造空间 |

## 2.2 复核后仍完全确认的 P0 结论

以下结论经二次核对后维持原判断：

- AEP worker 条件变量谓词无锁读取 trader vector，存在数据竞争。
- trader batch 达到 1024 上限时会回插整个 batch，已消费事件可能再次执行。
- 主做市 OQM 直接调用 `IHftStraCtx`，绕过策略层统一订单出口。
- 自动 Delta hedge 将 `executeOrder()` 的成功标志 `1` 当作真实 localid。
- 动态创建的 OQM 未装配 RiskFilterChain / PositionGuard / PositionOffsetMgr。
- 总仓口径漏加 `prevol`，期权 RiskData 未用 broker 初始仓位同步。
- 期权 tickSize 默认 `1.0` 后，真实 PriceTick 因 `<=0` 条件无法覆盖。
- RiskData/PnLTracker 可能在真实 multiplier 注入前缓存错误乘数。
- 热 STOP 在无事件时可能不生效，生效后也不会主动 refresh/cancel 旧报价。
- 多个模块使用 seconds-of-day 做持续时间，跨午夜会出现负间隔。
- `daysToExpiry()` 到期后不会变成负数，过期清理条件不可达。
- `PeriodicCurveFitter` 单点/区间外 forward 存在 `end()` 解引用路径。
- 撤单失败、top-up combo、迟到成交和重启工作单尚未形成可靠闭环。

## 2.3 复核后需要更精确表述的点

### 1. FAST 完整 Black 定价是条件触发

`OptionPricer2::updateTheoreticalValues()` 在 `settleFrac < 1.0` 时会直接调用 `computeValue()`，见 `src/WtOptionCore/OptionPricer2.cpp:595-602`。

因此应表述为：

> 在常规非最终结算状态、且 `settleFrac < 1.0` 时，FAST 路径可能仍执行完整 Black 定价。

不能简单写成“FAST 永远完整定价”。

### 2. broker 绝对仓位与成交叠加不是正常启动路径下的必然双计

框架 `on_position` 主要用于交易通道连接时的初始持仓同步；正常顺序下 position 先于 trade 并不会在同一启动快照中重复。但代码同时存在：

- position 绝对覆盖；
- trade 增量叠加；
- 同一 batch 固定先 Position 后 Trade。

因此准确表述应为：

> 在通道重连、回报乱序、broker 快照与成交交错等窗口下，当前“绝对覆盖 + 无条件增量”的混合模型存在双计或覆盖风险。

### 3. PnL 跨日问题主要影响长时间运行的进程

`PnlLimitSignal` 在 session begin 会 reset，但 `PnlTracker` 的累计 realized、FIFO 和 turnover 不会同步清零。若进程每个交易日重启，风险较小；若进程跨 session 常驻，则“每日亏损限制”口径会失真。

### 4. Scanner 不能描述成“9 个成熟套利策略”

Scanner 工厂、生命周期和 dispatch 已存在，但：

- 多数 scanner 的核心信号仍是同一 `abs(theo-mid)` 模板；
- `OptionData::setStrikeData()` 未见生产调用，而 scanner 执行路径依赖 `getStrikeData()`；
- Scanner 下单绕过统一订单出口。

因此应描述为：

> Scanner 子系统完成了注册和部分生命周期验收，但策略语义和生产执行链路尚未达到可用状态。

### 5. 2026-08-24 的部分历史修复仍有效，部分属于部分修复

- 旧 B01/B02 的单腿空指针防护仍有效。
- 旧 B03 的 tick/multiplier 注入已存在，但由于默认 `tickSize=1.0` 和后置缓存，仍不是完整修复。
- 旧 B18 已支持 secondary hedge multiplier，但 primary hedge 仍自动按默认 multiplier 注册。
- 旧 B20 已重置 PnlLimit signal，但未重置 PnLTracker 的 session 状态。

---

# 三、当前架构评估

## 3.1 当前实际主链路

```text
WT on_tick / on_trade / on_order / on_position / on_channel / on_session
  → HftOptionStrategy 回调入口
  → OptionAsyncEventProcessor 入队
  → worker 按 Session/Channel/Position/Trade/Order/Timer/Tick 分桶
  → tick 按 code 去重
  → OptionGrid.onTick
  → on_batch_complete
      ├─ 同步 position / PnL / Scanner / Attribute
      ├─ OptionRisk.update
      ├─ OptionGrid.computeValues
      │    └─ CompositeOptionPricer / GammaScalpOptionPricer
      │         └─ OptionPricer2 / BlackCalc / VolCurve / Fitter
      ├─ Grid notify
      │    └─ CTG.refresh
      └─ CTG.drainPendingQuotes
           └─ OTD/UTD.updateOrders
                └─ OptionQuoteManager
                     └─ IHftStraCtx 直接发单
```

## 3.2 值得保留的设计

1. **插件边界较清晰**  
   主库没有直接链接 WtCore，符合 WonderTrader 策略插件边界。

2. **异步 worker 方向正确**  
   行情与交易回报复制后入队，避免长时间持有框架回调对象。

3. **Composite 与 GammaScalp 策略边界清晰**  
   两者共享数学内核，但业务逻辑独立，方向正确。

4. **CTG / OTG / OQM 分层有继续演进价值**  
   desired 调度、逐合约交易数据、订单生命周期分别建模，可作为后续服务化拆分基础。

5. **OQM 已具备订单状态机雏形**  
   包括订单状态、TTL、撤单看门狗、增量 diff 和部分计数器。

6. **未启用 fast-math 是正确的**  
   当前数学代码仍使用 NaN 作为部分哨兵值，不应为了微小性能收益开启 `-ffast-math`。

## 3.3 核心架构问题

当前最大问题不是“类不够多”，而是：

- 组件存在，但动态生产接线缺失；
- 同一业务存在多个事实源；
- 订单、持仓、时间和配置各自有多个解释；
- 异常路径与主路径没有完全闭环。

---

# 四、P0：实盘前阻断问题

> P0 定义：可能造成错误发单、错误持仓、风控失效、进程崩溃，或使运维停机指令不可靠。即便部分缺陷只在特定功能启用时触发，也应作为上线阻断项。

---

## P0-1 异步事件队列存在数据竞争与重复回报

### 证据

- trader 生产者持锁写入：`src/WtOptionCore/OptionAsyncEventProcessor.cpp:178-186`
- worker 在条件变量谓词中无锁读取 `_trader_queue.empty()`：`src/WtOptionCore/OptionAsyncEventProcessor.cpp:257-263`
- 批次达到 1024 后，把**整个** `trader_batch` 回插：`src/WtOptionCore/OptionAsyncEventProcessor.cpp:290-307`
- MD overflow 满时只增加 drop 计数：`src/WtOptionCore/OptionAsyncEventProcessor.cpp:141-170`
- 单个事件异常会跳过整批剩余事件：`src/WtOptionCore/OptionAsyncEventProcessor.cpp:312-360`

### 影响

- `std::vector` 无锁读/写/交换构成数据竞争和 UB。
- 已处理的 trade/order/position/session 可能再次执行。
- 成交重复记账会直接污染 position、Greeks、PnL、combo 腿和异常单统计。
- 队列满时仅计数，不触发风险状态，可能继续用不完整状态交易。

### 修复方案

1. worker 谓词短时持有 `_trader_mtx`，或维护与 push/swap/回插同步更新的原子 trader 队列计数。
2. 记录已消费下标，只回插未消费后缀。
3. trader/控制事件使用可靠队列；不得与普通 tick 使用同样的丢弃策略。
4. MD 只可按合约保留最新，溢出后进入 `Disabled/Panic`，撤单并完成 freshness gate 后恢复。
5. 事件异常至少按类型隔离；trade/position/channel/session 处理失败应进入风险安全状态。

### 必测

- `1023 MD + 3 trader` exactly-once
- `1024 MD + trader` 不回插已消费前缀
- 多 producer + worker TSan
- 队列满后不得继续正常交易
- 回调异常后关键事件仍按明确契约处理

---

## P0-2 订单提交返回值语义混乱，真实 localid 丢失

### 证据

- `executeOrder()` 成功时返回 `1`：`src/WtOptionCore/HftOptionStrategy.cpp:2227-2245`
- `OptionHedgeCoordinator` 将返回值作为 localid 插入 `m_inflight`：`src/WtOptionCore/OptionHedgeCoordinator.cpp:42-52`
- 终态回调使用真实 localid 释放：`src/WtOptionCore/HftOptionStrategy.cpp:1624-1626`
- 当前 hedge 单测 mock 返回递增真实 ID，未覆盖生产适配器：`src/WtOptionCore/tests/test_hedge_coordinator.cpp:23-43`

### 影响

启用自动 Delta hedge 后，第一笔真实对冲单通常留下 `inflight={1}`，真实终态无法清除，后续对冲长期停止。

### 修复方案

不再用 `int32_t` 同时表达“成功与否、交易笔数、localid”。建议引入：

```cpp
struct OrderSubmissionResult {
    bool accepted;
    std::vector<uint32_t> localIds;
    std::string error;
};
```

拆单时登记全部 localid；只有同一批次全部相关订单终态后才释放 in-flight。

### 必测

- 首笔真实 localid 非 1，终态后可再次对冲
- 拆单产生多个 localid
- entrust reject、order reject、terminal 丢失
- partial fill 后 cancel/reject

---

## P0-3 主做市 OQM 绕过统一订单出口

### 证据

- OQM 直接调用 `stra_quote/stra_buy/stra_sell`：`src/WtOptionCore/OptionQuoteManager.cpp:639-663`
- 价格限幅和 FillPriceChecker/AnomalyGuard 只位于 `executeQuote/executeOrder`：`src/WtOptionCore/HftOptionStrategy.cpp:2184-2254`
- 未登记订单成交会被分类为 Unknown：`src/WtOptionCore/HftOptionStrategy.cpp:1519-1538`
- Scanner combo 也直接调用 `_ctx->stra_buy/sell`：`src/WtOptionCore/HftOptionStrategy.cpp:1941-1948`

### 影响

- 主做市报价绕过涨跌停检查和异常单登记。
- 正常 OQM 成交可能无法进入 OQM 成交更新。
- FillPriceChecker 缺少发单价，成交价偏离保护失效。
- 各订单路径风控口径不一致。

### 修复方案

建立唯一 `OrderGateway`，所有以下路径必须经过：

- OQM quote/order
- Scanner combo
- Delta hedge
- Gamma hedge
- Manual order

统一处理：

- price finite / positive 检查
- 交易所价格限制
- 全部 localid 登记
- FillPriceChecker / OrderAnomalyGuard
- 结构化错误和审计日志

### 必测

- OQM 发单后成交分类必须为 Normal
- 涨跌停外报价被拦截
- action policy 拆单时全部 ID 均被跟踪
- Scanner/Gamma/manual 不再绕过统一出口

---

## P0-4 动态创建的 OQM 未装配 RiskFilter / Guard / OffsetMgr

### 证据

- 初始化顺序：`setupGrid → setupPricer → setupCTG → setupSignals → setupScanners`，见 `src/WtOptionCore/HftOptionStrategy.cpp:505-509`
- 风控组件只在 `setupCTG()` 中遍历当时已有 OQM：`src/WtOptionCore/HftOptionStrategy.cpp:1115-1144`
- 动态创建 OQM 时只注入统计、pre-trade、时钟和 position source：`src/WtOptionCore/OptionTradingGrid.cpp:93-111`
- 全模块生产路径没有其他 `setRiskFilterChain/setPositionGuard/setPositionOffsetMgr` 调用。

### 影响

启动时日志显示风控组件已创建，但首批行情动态发现的大部分合约并未持有这些组件，形成“测试通过、生产空转”。

### 修复方案

将风控组件或 factory 保存在 `OptionTradingGrid`，在每次 `onAddOption()` / `__createExpiryTradingData()` 创建 OQM 时同步注入。

### 必测

- 初始化 strategy 后再注入首个 option tick
- 断言新建 OQM 的 filter/guard/offset 非空且实际可拒单
- 后续新增 expiry/hedge 合约同样生效

---

## P0-5 持仓没有单一事实源，昨仓漏计且 RiskData 不同步

### 证据

框架持仓语义：

- `prevol` = 昨仓
- `newvol` = 今仓

见 `src/Includes/UftStrategyDefs.h:114-126`。框架总仓接口使用 `newvol + prevol`，见 `src/WtCore/TraderAdapter.cpp:425-448`。

当前策略只使用 `newvol`：`src/WtOptionCore/HftOptionStrategy.cpp:1829-1846`。`PositionOffsetMgr.cpp:8-29` 同样只使用 `newvol`。

同时：

- `OptionRiskData::setPosition()` 存在：`src/WtOptionCore/OptionRiskData.h:78-80`
- broker position 回调没有调用它
- 期权 RiskData 主要通过成交 `addFill()` 更新

### 影响

- 隔夜仓从总仓、Greeks、压力测试、对冲输入中消失。
- 可平仓数量和 offset guard 失真。
- 重启后的组合风险不完整。
- 在 broker 快照与成交乱序/重连窗口，绝对覆盖和增量叠加可能产生双计。

### 修复方案

建立唯一 `PositionBook`：

```text
broker absolute snapshot + sequence/time + 受控成交桥接
```

统一向 OptionData、OptionRiskData、OTD/UTD、OQM、PnL、Guard、OffsetMgr 提供快照。

### 必测

- 昨仓+今仓总仓正确
- long/short 双边持仓
- 启动已有期权仓位进入组合 Greeks
- broker 快照与 fill 乱序、重复 fill、重复 snapshot
- future OQM 同步收到 order/trade/position

---

## P0-6 tickSize、multiplier、fee 等合约事实注入顺序错误

### 证据

- OptionGrid 缺省 `multiplier=1.0, tickSize=1.0`：`src/WtOptionCore/OptionGrid.cpp:298-320`
- OTG 只在 `tickSize <= 0` 时修补：`src/WtOptionCore/OptionTradingGrid.cpp:47-64`
- RiskData 构造时立即缓存 multiplier：`src/WtOptionCore/OptionRiskData.cpp:23-32`
- OTD 的 PnLTracker 默认 multiplier=1：`src/WtOptionCore/OptionTradingData.h:110-113`
- primary hedge 自动注册时未传 multiplier：`src/WtOptionCore/OptionRisk.cpp:108-112`

### 影响

- 报价仍可能被取整到非法价格格。
- 组合 Greeks、PnL、费用、保证金和对冲手数量纲错误。

### 修复方案

在 OptionData 创建时、任何 listener 通知之前注入不可变 `ContractFacts`：

```text
code / product / expiry / right / strike / tickSize / multiplier / feeMode / feeRate
```

禁止后置“猜默认值再修补”。

### 必测

- tick=0.2/0.5 的合约不会保留 1.0
- multiplier=300 的期权 Greeks 与 PnL 正确
- primary/secondary hedge 均使用真实乘数
- RiskData 不缓存过期 ContractFacts

---

## P0-7 STOP 不可靠，且旧报价不会自动撤销

### 证据

- 热更新只设置 `_hotUpdatePending`：`src/WtOptionCore/HftOptionStrategy.cpp:2445-2450`
- 只在 `on_batch_complete` 应用：`src/WtOptionCore/HftOptionStrategy.cpp:1273-1277`
- STOP 只调用 `setState(Disabled)`：`src/WtOptionCore/HftOptionStrategy.cpp:2486-2505`
- refresh/drain 在 enabled 门控内：`src/WtOptionCore/HftOptionStrategy.cpp:1287-1509`

### 影响

无 tick、无 trade、无 timer 时，STOP 可能一直不执行；执行后也不会生成撤单，旧挂单仍可能成交。

### 修复方案

1. STOP/PANIC/RESUME 使用可靠控制事件，立即唤醒 worker。
2. 状态进入动作必须清空 desired market、refresh、强制 drain cancel。
3. 撤单和 terminal tracking 不受 enabled 门控。
4. PANIC 保留风险降低单，但禁止新开风险单。

### 必测

- 完全安静市场 STOP 后无残留工作单
- STOP 后迟到 fill 不触发新开单
- PANIC 可撤销报价但不误撤风险降低单
- RESUME 前满足 freshness gate

---

## P0-8 跨午夜时钟回拨破坏持续时间逻辑

### 证据

统一时间辅助返回 seconds-of-day：`src/WtOptionCore/optioncoretypes.h:47-57`。

该时间被用于：

- EMA：`src/WtOptionCore/OptionValues.h:82-84`
- 计算防抖：`src/WtOptionCore/HftOptionStrategy.cpp:1356-1370`
- TPS：`src/WtOptionCore/ControllableTradingGrid.cpp:349-353`
- TTL：`src/WtOptionCore/OptionQuoteManager.cpp:256-270`
- Delta cooldown：`src/WtOptionCore/DeltaHedgeExecutor.h:47-67`

### 影响

夜盘跨过 00:00 后，`now-last` 为负，可导致：

- TPS 窗口不复位
- TTL/cancel watchdog 不触发
- 对冲 cooldown 不结束
- EMA 持续丢弃样本
- panic/fitter 计时异常

### 修复方案

- 内部持续时间统一使用单调时钟或带日期的连续时间戳。
- 展示用 wall-clock 与内部 duration 分离。
- session/day rebase 时显式 reset EMA、TPS、TTL、panic、hedge、fitter。
- 增加跨午夜 replay 测试。

---

## P0-9 到期合约无法进入负 DTE，过期清理条件不可达

### 证据

- `countCalendarDays()` 到期后返回 0：`src/WtOptionCore/ExpiryData.cpp:65-80`
- `updateDaysToExpiration()` 直接使用该值：`src/WtOptionCore/ExpiryData.cpp:83-93`
- 过期判定依赖 `<0`：`src/WtOptionCore/OptionRisk.cpp:28-39`
- purge 依赖 `<0`：`src/WtOptionCore/OptionGrid.cpp:518-527`
- 停止报价依赖 `<0`：`src/WtOptionCore/CompositeOptionPricer.cpp:862-869`
- maturity 仍有最小值 `1/252`：`src/WtOptionCore/ExpiryData.cpp:123-126`

### 影响

到期后合约永久处于“到期当天”状态，停止报价、purge 和过期风险隔离不可达。

### 修复方案

引入显式状态：

```text
ACTIVE → EXPIRING → EXPIRED → SETTLED
```

到期后：

- 撤单
- 停止新增报价
- 停止新增风险
- 切换到 intrinsic/settlement 逻辑
- 支持结算/移仓

### 必测

- 到期前一日、到期日、到期后一日
- 长期无行情
- 周末/节假日
- 已过期合约不能发新单或继续 Black 定价

---

## P0-10 Fitter 在单点/区间外 forward 存在 UB

### 证据

- `find_range()` 可能返回 `{end, begin}` 或 `{begin, end}`：`src/WtOptionCore/PeriodicCurveFitter.cpp:60-74`
- 后续未验证可解引用就直接使用：`src/WtOptionCore/PeriodicCurveFitter.cpp:191-208`

### 影响

开盘、只有一个有效 IV 点、forward 落在全部 strike 外时，可能崩溃或产生 UB。

### 修复方案

显式处理：

- 0 点
- 1 点
- forward 小于全部 strike
- forward 大于全部 strike
- 精确命中
- 正常 bracket

单点不得进入双点插值。

### 必测

- 0/1/2/3/4 个 vol 点
- forward 低于、等于、高于全部 strike
- ASan/UBSan 通过

---

## P0-11 撤单失败、Combo top-up 和重启工作单未闭环

### 证据

- `m_cancelStuck` / PositionGuard 在 cancel-only 前返回：`src/WtOptionCore/OptionQuoteManager.cpp:31-60`
- 撤旧单失败仍可能发新单：`src/WtOptionCore/OptionQuoteManager.cpp:348-379`
- `sendCancelById()` 结果多数未校验：`src/WtOptionCore/OptionQuoteManager.cpp:691-696`
- `sendCancelAll()` 本地清空订单但未确认交易所终态：`src/WtOptionCore/OptionQuoteManager.cpp:740-747`
- SynCombo top-up 新 localid 未保存：`src/WtOptionCore/ComboOrders.cpp:354-364`
- terminal/cancelAll 只识别单个 `leg.localId`：`src/WtOptionCore/ComboOrders.cpp:402-420,449-457`
- timeout/inactive combo 立即从活动列表移除：`src/WtOptionCore/HftOptionStrategy.cpp:1486-1505`
- session begin 直接清空 anomaly guard：`src/WtOptionCore/HftOptionStrategy.cpp:1781-1782`

### 影响

- 最需要撤单时反而撤不掉。
- 撤单失败后又发新价订单，放大敞口。
- top-up 或迟到成交可能留下不可管理裸腿。
- 重启/跨 session 后旧工作单无法可靠认领。

### 修复方案

- 每腿维护全部 localid、committedQty、filledQty、terminal 状态。
- 撤单请求被接受后才进入 CancelPending。
- cancel-only 优先于所有新单 guard。
- combo timeout 后保留管理对象，直到所有腿终态并处理残差。
- 启动增加 Recovering 对账状态：认领或撤销遗留工作单后才允许新单。

### 必测

- `stra_cancel=false`
- cancel ack 丢失
- 非目标订单 terminal 不得解除 stuck
- top-up 拒单/迟到成交
- 重启时旧工作单不重复下单

---

# 五、P1：高优先级正确性与一致性问题

| 类别 | 问题 | 证据/说明 | 建议 |
|---|---|---|---|
| 期货订单 | UTD OQM 未收到 order/trade/position | `HftOptionStrategy.cpp:1527-1539,1645-1659,1287-1297` | 建立统一订单/持仓路由，按 code 同时支持 OTD/UTD |
| 行情新鲜度 | 交易所时间戳在异步路径丢失 | `TickData` 有 `updateTime`，`TickDataRef` 不携带 | 每合约维护 exchange time + local monotonic receive time；stale 后 cancel-only |
| stale 定价 | 旧 bid/ask 可继续刷新 forward 和报价 | `OptionGrid.cpp:709-715`, `CompositeOptionPricer.cpp:884-888` | 定价、拟合、对冲、报价前统一 freshness gate |
| PnL 聚合 | PnlLimit 与 RiskLimitsEx 使用不同聚合路径 | `HftOptionStrategy.cpp:1316-1325` vs `2268-2275` | 建立唯一 SessionPnlSnapshot |
| PnL 口径 | option tracker multiplier=1、费用未注入 | `OptionTradingData.h:110-113`；期权路径未见 `PnlTracker::setFeePct()` | ContractFacts 注入 multiplier/fee；PnL 支持固定费用和比例费用 |
| PnL 跨日 | signal reset 但 tracker 不 reset | `HftOptionStrategy.cpp:1783-1797`, `PnlTracker.cpp:114-118` | session begin 重建当天 realized/FIFO/turnover |
| 配置漂移 | full config 的 `slow_compute_interval=0.1` 未传给 Composite | `config/option_strategy_full.json:33-36`, `CompositeOptionPricer.h:156-159`, `HftOptionStrategy.cpp:913-948` | typed ConfigLoader + effective-value dump |
| 零利率 | 显式 `riskFreeRate=0` 被改成 3% | `HftOptionStrategy.cpp:261-263` | 0 是合法值；负值才拒绝 |
| OQM pre-trade | refPrice 固定为 0，erroneous-price 检查失效 | `HftOptionStrategy.cpp:765-769` | gateway 注入真实 reference price |
| Panic | 手工/Stress panic 可在下一周期被自动清除 | `HftOptionStrategy.cpp:1426-1449` | panic 按来源独立 latch，手工 panic 只能手工解除 |
| hard-flat | 只置标志或打日志，不真正限制新单 | `OptionQuoteManager.cpp:134-141`, `RiskFilterChain.cpp:109-119` | 进入只减仓状态机，撤新单、禁止增仓 |
| 空头限额 | 超限时买回减仓也会被拒 | `RiskFilterChain.cpp:135-152` | 降低风险方向优先放行 |
| Stress | vega 单位少乘 100 | `OptionPricer2.cpp:513`, `StressAnalyzer.cpp:39-49` | 统一“每 vol point”口径，并修正测试 |
| NaN 风控 | NaN 比较可绕过限额，Stress 返回中性 0 | `RiskLimitsEx.cpp`, `StressAnalyzer.cpp` | 所有风险入口 fail-closed |
| Power 曲线 | fit 用 `K-F`，eval 用 `log(K/F)` | `PowerVolCurve.cpp:39-79`, `PeriodicCurveFitter.cpp:216-228` | 统一训练/预测特征空间 |
| 多到期 forward | global tick 可污染其他 expiry 的 futureMid | `OptionGrid.cpp:168-190` | per-expiry underlying 与全局 ref price 分离 |
| 到期日 | 精确到期日只在创建时回填 | `OptionGrid.cpp:212-230` | 后续 tick 可补全，跨日重算 maturity/rate |
| OQM worst-case | potential position 用双边净额，filter 又重复加本单 | `OptionQuoteManager.cpp:121-126`, `RiskFilterChain.cpp:52-65` | 分别计算 worstLong/worstShort，并统一 potential 基准 |
| CTG retry | pending 为空时 retry 死区、retry 绕过 TPS、drop 可累积 | `ControllableTradingGrid.cpp:512-593` | retry/TPS/drop 独立窗口化、按 code 去重 |
| 订单事件 | 普通撤单与交易所拒单混淆，entrust reject 未进状态机 | `OptionQuoteManager.cpp:499-513`, `HftOptionStrategy.cpp:2152-2159` | 明确 Accepted/Canceled/Rejected/CancelRejected/Filled 事件模型 |
| 手工命令 | 非 B 方向默认按卖出，NaN/零数量校验不足 | `OptionCommandHandlers.cpp:31-44` | 白名单、isfinite、qty>0、合约白名单、独立限额 |
| 重启恢复 | 未见工作单认领/对账流程 | OQM 状态从空开始，channel ready 后直接 Normal | 增加 Recovering 状态与 broker order/position reconcile |

---

# 六、低延迟专项评估

## 6.1 当前结论

当前模块更接近“异步批处理型 HFT 做市策略”，尚无充分证据证明已达到严格低延迟目标。

默认 `minComputeInterval=0.02`，见 `src/WtOptionCore/HftOptionStrategy.cpp:89-90`。这是吞吐与防抖折中，本身未必错误，但不应把它等同于微秒级交易链路。

同时，`src/WtOptionCore/OptionPricer2.h:65-77` 的 `PerfCounter` 不累计实际耗时，当前日志无法证明：

- FAST/SLOW 计算耗时
- tick→theo
- theo→gateway
- gateway→localid
- cancel→ack
- fill→风险更新
- p99/p99.9/max

## 6.2 已确认的主要热点

### 1. FAST 路径在 `settleFrac < 1.0` 时可能仍完整 Black

证据：`src/WtOptionCore/OptionPricer2.cpp:595-602`。

建议：小 dF 且 maturity/curve version 未变时走增量，只有 SLOW、曲线变化、大跳动或误差超阈值时完整 Black。必须与完整 Black 做数值回归。

### 2. Vega 风险调整存在 O(N×M)

证据：`src/WtOptionCore/CompositeOptionPricer.cpp:1358-1468` 每个 option 遍历全部非零持仓，并重复查找 ATM。

建议：每个 expiry 每周期只算一次 ATM vega，并按 expiry/风险桶预聚合持仓。

### 3. 每个 batch 都全量扫描 active option

证据：`src/WtOptionCore/HftOptionStrategy.cpp:1314-1338`。

建议：

- PnL 只更新有持仓、成交或行情变化的合约；
- Scanner 只接收 dirty option 及相关结构；
- Attribute 到期 publish 周期才收集；
- 风险聚合只处理 dirty expiry/position bucket。

### 4. 慢任务与交易事件共享 worker

证据：

- IV/fit：`src/WtOptionCore/HftOptionStrategy.cpp:1733-1738`
- Stress：`1381-1422`
- CSV：`1473-1477`, `OptionValueWriter.cpp:38-70`
- Attribute collect：`1314-1346`

建议：worker 只生成不可变快照，拟合、Stress、Attribute、CSV、counter dump 移到专用慢线程。

### 5. CTG retry/TPS 正反馈

证据：`src/WtOptionCore/ControllableTradingGrid.cpp:512-593`。

建议：

- retry 按 code 去重，只保留最新 desired
- retry 纳入统一 TPS 窗口
- drop 计数按周期复位并限频
- 使用持久双缓冲，避免每轮重新分配

## 6.3 低延迟优化顺序

1. 修事件、时钟、STOP、订单、持仓正确性。
2. 建立真实延迟指标和 replay benchmark。
3. 移出 worker 慢任务。
4. 改 dirty/event-driven 更新。
5. FAST 增量化和 Vega 预聚合。
6. 再做容器、内存布局、CPU affinity、NUMA 等微优化。

不建议在 P0 未修复前优先做 SIMD、无锁化或激进多线程拆分。

## 6.4 建议指标

- `md_enqueue→worker`
- `worker→theo`
- `theo→gateway`
- `gateway→localid`
- `cancel→cancel ack`
- `fill→PositionBook`
- `fill→RiskSnapshot`
- 队列最大深度/最大事件年龄
- coalesce/drop 数
- p50/p95/p99/p99.9/max
- 稳态热路径分配次数
- worker CPU、上下文切换、页错误

没有硬件与回放基线前，不建议写绝对微秒目标。

---

# 七、代码优雅与结构债务

## 7.1 已确认的结构问题

1. **God Object 仍存在**  
   `HftOptionStrategy.cpp` 约 2543 行，同时承担配置、装配、状态机、异步路由、风控、PnL、命令、对冲和订单桥接。

2. **事实源多头**  
   position 同时分布在 `_positions`、OptionData、OptionRiskData、OQM、UTD、Guard、OffsetMgr。

3. **订单路径多头**  
   OQM 直连、Scanner 直连、Strategy executor、Gamma sender、manual order。

4. **依赖注入时机错误**  
   风控组件动态创建时未注入，ContractFacts 在 listener 缓存后才修补。

5. **生命周期所有权不清晰**  
   OTG ↔ UTD 存在 shared_ptr 环；Grid 持 Pricer/Fitter；ExpiryData 持 UTD raw pointer。

6. **存在伪指标和死代码**  
   `PerfCounter` 不累计耗时；存在写而不读集合、未使用私有方法和历史遗留接口。

7. **测试会固化错误契约**  
   PositionOffset 测试和注释将 `newvol` 当总仓，与框架语义冲突。

8. **历史修复标签过多**  
   大量 `Bxx/Pxx/Rxx` 注释保留在热路径中。建议后续将迁移历史移入 ADR，源码中保留当前业务不变量。

## 7.2 不建议的做法

- 不要为了“优雅”一次性大拆。
- 不要同时混合资金正确性修复、性能优化和大重构。
- 不要只做风格重命名或全库格式化。
- 不要在共享可变 Grid 上直接加多线程锁。

## 7.3 推荐重构顺序

```text
ConfigLoader
→ OrderGateway
→ PositionBook / ContractFacts
→ OptionBatchCoordinator
→ Pricer 内部拆分
→ OQM 状态机拆分
→ 生命周期与 ownership 清理
```

每一步只做一类职责搬移，并配对应测试。

---

# 八、目标架构建议

```text
                    ┌────────────────────────────┐
                    │ Typed ConfigLoader          │
                    │ ContractFacts / Defaults    │
                    └─────────────┬──────────────┘
                                  │
Market callbacks ───────┐         │
Trader callbacks ───────┤         │
Hot control commands ───┤         ▼
                        │  Sequenced EventRouter
                        │  - MD: coalesce by code
                        │  - Control: reliable, no drop
                        │  - Trade/Order/Position: ordered
                        └──────────┬───────────────┘
                                   │
                         Single-writer Runtime
                                   │
       ┌───────────────────────────┼───────────────────────────┐
       │                           │                           │
       ▼                           ▼                           ▼
 PositionBook               MarketState/Pricer            Risk/PnL Aggregator
 - prev+new                  - FAST incremental            - versioned snapshot
 - long/short                - SLOW full reprice           - fail-closed
 - broker/fill reconcile     - immutable curve snapshots   - session PnL
       │                           │                           │
       └───────────────┬───────────┴───────────┬───────────────┘
                       ▼                       ▼
             ControllableTradingGrid       OrderGateway
             - desired diff                - all localids
             - rank/TPS                     - price limit
             - panic handling               - guards/checkers
                       │                       │
                       └───────────┬───────────┘
                                   ▼
                          OptionQuoteManager
                          - per-contract lifecycle
                          - cancel watchdog
                          - reject classification
                                   │
                                   ▼
                              IHftStraCtx

慢路径（不可变快照）：
VolFit / Stress / Attribute / CSV / Replay / ExpirationSimulator
```

核心原则：

- 控制事件可靠、优先、不可丢。
- PositionBook、ContractFacts、SessionPnl 是唯一事实源。
- OrderGateway 是所有订单的唯一出口。
- 内部持续时间统一单调。
- 慢计算不阻塞成交、撤单和风控。
- 重启后必须经过 Recovering 对账。

---

# 九、分阶段优化方案

## 阶段 0：阻断修复

### 范围

1. AEP race / trader suffix reinsert / overflow fail-safe / event exception isolation
2. OrderSubmissionResult，修真实 localid 和 Delta hedge inflight
3. STOP/PANIC 可靠控制事件与强制撤单
4. 统一单调时钟和跨午夜 reset
5. PositionBook 第一层：昨仓+今仓、broker 初始化 RiskData、future OQM 路由
6. ContractFacts 在 listener 前注入
7. 动态 OQM 创建时装配风控组件
8. Expired 状态机
9. Fitter 边界 UB
10. Session PnL reset 与统一聚合

### 退出标准

- `1023 MD + 多 trader` exactly-once
- TSan 无报告
- 安静市场 STOP 后无工作单
- 跨午夜后 TTL/TPS/cooldown 正常
- 昨仓+今仓总仓唯一
- 重启初始仓位进入 Greeks
- 动态合约的 filter/guard/offset 生效
- 单点 Fitter ASan/UBSan 通过

---

## 阶段 1：统一生产接线

### 范围

1. 唯一 `OrderGateway`
2. OQM / Scanner / Delta / Gamma / Manual 全路径接入
3. `PositionBook` 完整替代多处 position 拷贝
4. typed `OptionConfigLoader` 与 effective-value dump
5. 明确订单事件模型：Accepted / Canceled / Rejected / CancelRejected / Filled
6. 启动 Recovering 对账状态
7. Panic / hard-flat / stress 分源 latch

### 退出标准

- OQM 成交不再 Unknown
- 涨跌停外订单被拦截
- full config 关键值与运行时一致
- 手工 panic 不会被自动清除
- hard-flat 后只能减仓
- 重启遗留工作单被认领或撤销，不重复下单

---

## 阶段 2：调度和低延迟

### 范围

1. 事件携带 sequence、exchange timestamp、local enqueue time
2. 修复 timer-before-tick 顺序
3. 独立控制定时器
4. Attribute/CSV/Stress/Fit 移出 worker
5. dirty-set 更新 PnL/Scanner/Risk
6. FAST 增量化，SLOW 完整重算
7. Vega 每 expiry 缓存和持仓风险预聚合
8. CTG retry/TPS/drop 窗口化
9. replay benchmark

### 退出标准

- 建立同硬件、同回放、同合约数的 p50/p95/p99/p99.9/max 基线
- worker 不再做同步文件 flush
- 稳态热路径无明显重复全量扫描
- FAST 增量结果与完整 Black 回归一致

---

## 阶段 3：功能补全

### 范围

1. GammaScalp 时钟、hedgeCode、乘数、在途管理
2. Scanner 真实策略语义和 strike 反链
3. Panic/Disabled 下禁止 scanner 新开组合
4. Combo 全部 localid 跟踪与残差 unwind
5. Power/GVV/Black NaN 和拟合失败 stale timeout
6. Expiration settlement/roll 状态机

### 退出标准

- Gamma、Scanner、Power 默认启用前均有端到端验收
- 到期合约不再继续报价
- combo 部分成交、拒单、迟到成交均可恢复或明确人工告警

---

## 阶段 4：结构优化与工程体系

### 范围

1. 抽 `OptionBatchCoordinator`
2. 拆 Composite scheduler / quote generator / risk shifts
3. 拆 OQM lifecycle / diff engine / throttle
4. 清理 shared_ptr 环和死代码
5. 根构建接入统一 CTest 和 `BUILD_TESTING`
6. Debug + ASan/UBSan、独立 TSan、Release CI
7. 修复 Release 下 `assert` 失效
8. CMake 编译器/架构守卫

### 退出标准

- Debug/Release 都执行真实断言
- CI 必须运行 `ctest --output-on-failure`
- sanitizer job 覆盖库与测试
- 生产构建 `BUILD_TESTING=OFF` 不生成测试目标

---

# 十、必补测试矩阵

## 10.1 异步与并发

- `1023 MD + 3 trader` exactly-once
- `1024 MD + trader` 不重复回插已消费前缀
- 多 producer + worker TSan
- 队列满时控制事件 fail-safe
- callback 异常后关键事件仍按契约处理
- 多 Strategy 实例诊断计数无数据竞争

## 10.2 订单与对冲

- 首笔真实 localid 非 1，终态后可再次 hedge
- 拆单返回多个 localid
- OQM 成交分类为 Normal
- 涨跌停外订单被拦截
- entrust-only reject、order reject、cancel reject 分类
- partial fill 后 cancel/reject/late fill

## 10.3 持仓与重启

- 昨仓+今仓总仓
- long/short 双快照
- 启动已有期权仓位进入组合 Greeks
- broker 快照与 fill 乱序
- future OQM 收到完整 order/trade/position
- 重启遗留工作单认领/撤销
- 对账完成前只允许 cancel-only

## 10.4 时间与停机

- `23:59:59.900 → 00:00:00.100`
- 跨 session 后 EMA、TPS、TTL、hedge、panic、fitter reset
- 完全安静市场 STOP 后立即撤单
- PANIC 保留风险降低单、禁止新开风险单

## 10.5 定价与到期

- Fitter 0/1/2/3/4 点
- forward 在区间外
- Black 黄金值、Greeks 有限差分、IV round-trip
- 零 vol、零 maturity、strike=0、NaN/Inf forward/discount
- 到期前一日、到期日、到期后一日
- stale tick 后 forward/报价/对冲失效并 cancel-only

## 10.6 PnL 与风险

- session rollover
- 无行情但有持仓/已实现 PnL
- multiplier=300
- 固定费用和比例费用
- Stress vega 单位
- NaN price/Greeks/PnL fail-closed
- 超空头限额时买回减仓放行

---

# 十一、测试与工程化现状复核

## 11.1 当前已有价值

以下组件已有一定单测覆盖，不应一概否定：

- RiskFilterChain
- OQM buy/sell、retry、cancel timeout
- Combo 终态
- FIFO PnL
- PowerVolCurve 基础路径
- GVV α=1 快路径
- StressAnalyzer 基础路径
- Hedge Coordinator 纯决策
- CommandBus
- TradingState flags

## 11.2 当前关键盲区

1. 没有 `OptionAsyncEventProcessor` 并发测试
2. 没有 `HftOptionStrategy + AEP + 动态 Grid/OTG/OQM` 集成测试
3. 没有动态合约创建后的 wiring 测试
4. Hedge 测试 mock 与生产返回类型不一致
5. 没有 ConfigLoader / CompositeOptionPricer / OptionGrid / CTG 主链测试
6. 没有跨午夜、重启、stale 行情、拒单和乱序故障注入
7. 没有 Black/GVV/IV 数值黄金测试
8. `test_ctg_ranking` 使用 `assert`，Release/NDEBUG 下会失效

## 11.3 CMake 与 CI

- 模块有 6 个 CTest 测试，但应由 `BUILD_TESTING` 控制。
- 根构建需要统一 `include(CTest)`，保证根级 `ctest --output-on-failure` 可发现。
- Release/RelWithDebInfo 下标准 `assert` 会消失，测试必须换自定义断言或 GoogleTest。
- 非 Debug 优化参数目前缺少编译器/架构守卫，Windows/MSVC、ARM 路径不安全。
- 未发现针对 WtOptionCore 的独立 sanitizer/warning CI 通道。

---

# 十二、与 2026-08-24 旧审查的关系

| 旧结论/修复 | 2026-10-30 复核结果 |
|---|---|
| 单腿空指针防护 | **仍有效**：`CompositeOptionPricer.cpp:1363-1369`、`PeriodicCurveFitter.cpp:157-159` |
| 期权 tick/multiplier 注入 | **部分有效但未闭环**：默认 `1.0` 使真实 tick 无法覆盖，RiskData/PnL 仍可能缓存错误 multiplier |
| secondary hedge multiplier | **已改善但 primary 未覆盖**：primary 自动注册仍默认 1.0 |
| PnlLimit 跨 session reset | **部分有效**：signal reset，但 PnLTracker session 状态未 reset |
| Scanner 工厂为空 | **已过时**：9 类 scanner 已注册；但策略语义和执行链路仍未达到可用状态 |
| Combo fill 路由 | **已改善**：已实现腿成交/终态路由；但 top-up、迟到成交、timeout 管理仍不完整 |
| OQM 计数器 | **部分改善**：有 session reset；TPS/retry/drop 窗口仍有死区和绕过问题 |
| `-march=native` | **已改为 v3**，但编译器/架构可移植性仍未处理 |
| 测试体系 | **数量增加但核心 wiring 仍缺失**，Release assert 问题仍存在 |

本文不替代旧报告；旧文件保留为历史迁移记录。

---

# 十三、建议的落地顺序

1. **先修 AEP exactly-once 与 fail-safe**
2. **修 STOP/PANIC/时钟**
3. **修 OrderSubmissionResult 与 Delta hedge localid**
4. **修动态 OQM wiring**
5. **修 PositionBook 与 ContractFacts**
6. **修 expired 状态与 Fitter UB**
7. **补生产接线集成测试和 TSan/ASan/UBSan**
8. **再做 worker 减负和延迟基线**
9. **最后做功能补全与结构性拆分**

在阶段 0/1 完成前，建议继续将以下能力视为实验或默认禁用：

- GammaScalp
- Scanner 自动执行
- Power 曲线
- 自动 Delta 对冲
- 到期自动结算/移仓

---

# 十四、最终结论

WtOptionCore 的组件基础并不差，模块化方向和异步 worker 思路都值得保留。当前真正的瓶颈是：

> **异步事件、订单出口、持仓/合约事实、时间语义和异常生命周期这五个基础契约尚未统一。**

因此优化路线不应从激进低延迟技巧开始，而应先做到：

- 事件恰好一次；
- 订单真实 ID 与统一出口；
- 持仓和 Greeks 单一事实源；
- STOP/PANIC 真正可靠；
- 跨午夜和重启状态可控；
- 到期和拒单路径闭环。

完成上述工作后，再进行 dirty-set 调度、FAST 增量化、慢任务剥离和延迟基准测试，才会形成稳定、可验证的低延迟优化收益。
