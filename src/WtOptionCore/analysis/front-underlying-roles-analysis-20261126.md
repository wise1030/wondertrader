# front month / 全局标的 / per-expiry underlying 职责与简化分析（2026-11-26）

> 用户问题：front 月、全局标的分别有什么作用，是否可以简化。
> 本文只分析，不改代码。源码基准为 2026-11-26 front-underlying 修复后版本。

## 一、三个概念各自的作用

### 1. 全局标的 `m_underlyingCode`（配置 `underlyingCode: SHFE.cu.2612`）

定位：**单标的场景的唯一定价标的价格源**；在多到期月场景退化为
"路由默认值 + 行情订阅对象"。

| 用途 | 位置 | 说明 |
|---|---|---|
| 行情订阅 | HftOptionStrategy on_init | 全局标的合约本身被订阅（"subscribed underlying: SHFE.cu.2612"） |
| onTick 全局分支 | OptionGrid.cpp:158 / 248 | 该代码的 tick 直接驱动 grid 级 m_underlyingPrice；TickDataRef 版还为"无 own 的 expiries"按月匹配门控更新 future mid（P0-②） |
| per-expiry 回退绑定 | __getOrCreateExpiryData:488 | 无 own underlying 的 expiry，其 ExpiryData 绑定全局标的；月份匹配时才注册 tick 路由 |
| front 判定回退 | isFrontUnderlyingCode | front 无专属 own 时，`stdCode == m_underlyingCode` 视为 front（11-26 修复后的语义） |

### 2. grid 级 `m_underlyingPrice`（全局"当前标的价格"）

定位：**front 门控后的"策略级标的价格"**，服务信号、风控、记录等非逐月
定价的消费方。修复后 = front month 合约的最新价。

| 消费方 | 位置 | 用途 |
|---|---|---|
| 信号上下文 | CompositeOptionPricer.cpp:571 | `m_signalCtx.underlyingPrice = m_frontfut`（= grid 价）→ RollEma / FrontFutSkew / ForwardSpread |
| SLOW 重算参考价 | OptionPricer2.cpp:347 | parity OTM/ITM 配对参考 refPrice |
| B11 predictor | HftOptionStrategy.cpp:854/864 | forward 输入 + 30 秒预测评分 |
| GARCH 估计器 | HftOptionStrategy.cpp:2113 | 波动率估计输入 |
| 扫描器分发 | HftOptionStrategy.cpp:1971 | dispatchUnderlying → SyntheticFutureScanner 等 |
| 保证金估算 | HftOptionStrategy.cpp:4480 | checkMarginLimits 的 udlPx |
| AnalysisRecorder | HftOptionStrategy.cpp:2318 | pfseries 的 spot 列 |
| ExpirationSimulator | HftOptionStrategy.cpp:3223 | 无 per-expiry 价格时的回退结算价 |
| 期权行情快照 | OptionGrid.cpp:185/297/461 | mkt.underlyingPrice fallback 字段 |
| UI / batch 日志 | HftOptionStrategy.cpp:2020 等 | 展示 |
| GammaScalp pricer | GammaScalpOptionPricer.cpp:305 | 当前未启用 |
| OTG 价格回调 | HftOptionStrategy.cpp:687 | utd mid 缺失时回退 |

### 3. front month（`m_frontMonth` / `m_frontMonthExpiry`）

定位：**"哪个到期月代表近月"的调度概念**，决定 grid 价跟哪个合约、
frontForward 取哪个月。

| 用途 | 位置 | 说明 |
|---|---|---|
| grid 价门控 | isFrontUnderlyingCode | 只有 front 合约（或其 own）驱动 grid 价；11-26 修复点 |
| frontForward | OptionGrid.cpp:784-786 → CompositeOptionPricer.cpp:572 | getFrontAtmForward(m_frontMonth) → 信号 ctx.frontForward → AtmSig（atmsig×frontForward）、FrontFutSkew（roll=underlyingPrice−frontForward） |
| 到期滚动 | reevaluateFrontMonth（B6） | session begin 检查 front 是否过期，滚到最近未过期月 |
| predictor confidence | HftOptionStrategy.cpp:855 | front ed 的 values 就绪状态 |

### 4. per-expiry own underlying（实际定价真源）

| 用途 | 位置 | 说明 |
|---|---|---|
| 实际定价 forward | ed->getUnderlyingPrice / getFutureMid → parity 池 → m_atmforward | Black / FAST dF 的 F；这才是逐月定价的真源 |
| 扫描器 | HftOptionStrategy.cpp:3860 ctx.spot、:2461 fillSpot | per-expiry（G3 注释） |
| 到期结算 | ExpirationSimulator per-expiry | P2-4 |

## 二、冗余诊断

修复后，系统里存在两条**数值恒等**（都等于 front 合约最新价）但**来源独立**
的价格流：

- grid 级 `m_underlyingPrice`：onUnderlyingTick 经 isFrontUnderlyingCode 门控写入；
- front ed 的 `ed->getUnderlyingPrice()`：per-expiry tick 路由直接写入。

历史上三次 bug 全部来自"grid 价到底该跟哪个合约"的语义分歧：

| 日期 | 问题 | 根因 |
|---|---|---|
| 11/17 | grid 价在月间价差间振荡（100900↔103580） | per-expiry 路由 last-tick-won，两月合约都更新 |
| 11/18 | 远月 synthetic forward 被拖向近月期货价；P1-⑦ 修 11/17 | 全局标的被路由进所有月；grid 价缺 front 门控 |
| 11/26 | grid 价 89600↔92250 振荡 | front 滚过全局标的后，`stdCode==m_underlyingCode` 短路继续放行旧合约 |

根源：**grid 价是一个"需要被门控维护的独立状态"，而它的真源（front ed 价格）
本来就有**。门控逻辑 = 真源判定的冗余副本，副本与真源的同步规则一改配置
就失配。

## 三、简化方案

### 方案 A（推荐）：grid 价改为派生，删独立状态

`getUnderlyingPrice()` 直接返回
`front ed ? front ed->getUnderlyingPrice() : 0`，删除 onUnderlyingTick 的
独立写入、删除 isFrontUnderlyingCode 门控、删除全局分支对 grid 价的更新。

- 优点：单一事实来源（per-expiry 路由是唯一价格流）；三次历史 bug 的
  语义分歧彻底消失；tick 路由只剩一套。
- 改造面：getUnderlyingPrice() 全部 12 个消费点只是读值，行为不变；
  OptionGrid 内部集中改动；单测需重写两个 front 相关用例为新语义。
- 风险：front ed 尚未创建时 grid 价=0——消费点大多有 >0 检查，但
  refPrice（OptionPricer2:347）会直接 return false，等于启动早期 SLOW
  重算停摆（现状是全局标的 tick 先建立 grid 价）；需要"front ed 创建前用
  全局标的价临时填充"的过渡或接受早期空窗（现有 readiness gate 本来就
  阻挡报价）。指数型/单标的场景：全局 tick 经 P0-② fallback 路由维护
  front ed 价格，可行。
- 结论：语义最干净，但需一次集中改造 + 全量测试 + 短跑验证。

### 方案 B（保守）：维持代码现状，配置层收敛

约定"全局标的必须等于当月 front 合约"，on_init 加校验：front 有 own 且
own≠全局标的时打 warning，提示更新配置。

- 优点：零代码风险，今天修复已堵住漏洞。
- 缺点：依赖人工每月滚动配置；隐患仍在（配置忘改就回到 11/26 场景）。

### 方案 C（中间）：统一路由，保留双状态

onTick 所有分支（含全局分支）统一过 isFrontUnderlyingCode 门控更新 grid 价。

- 优点：堵住"全局标的不在 per-expiry 表"的残余绕过路径（诊断文档中的
  边界说明场景）。
- 缺点：仍保留两个价格状态，语义冗余未根除；属于补丁而非简化。

## 四、front month 是否还有存在的必要性（专项）

> 区分两件事：**"front month"作为业务概念**（近月基准，信号需要）vs
> **front month 作为显式状态**（`m_frontMonth` 变量 + 滚动逻辑）。

### 状态机现状（代码事实）

| 项目 | 事实 |
|---|---|
| 状态变量 | `m_frontMonth`（uint）、`m_frontMonthExpiry`（ExpiryDataPtr） |
| 赋值点 | 仅 3 处：`getFrontForward()` 惰性初始化（begin()->first）、`reevaluateFrontMonth()` 滚动、`purgeExpired()` 重置为 0 |
| `reevaluateFrontMonth()` 调用点 | 仅 session begin（OptionExpiryLifecycle.cpp:11）+ purgeExpired 内部 |
| `purgeExpired()` 调用点 | 仅 session begin（OptionExpiryLifecycle.cpp:10） |
| 死代码 | `setFrontMonth()` / `setFrontMonthExpiryData()`（OptionGrid.h:162/164）**无任何 .cpp 调用者** |
| 盘中到期 | 不触发任何滚动——`m_frontMonth` 保持指向当天到期月直到下个 session begin |
| 消费点 | 仅 4 个：grid 价门控（isFrontUnderlyingCode）、getFrontForward→信号、predictor confidence、purge 重置 |

### 关键观察

session begin 序列是 `purgeExpired()`（清 dte<0）→ `reevaluateFrontMonth()`
（滚到 dte>0 最近月）。此后 `m_expiries` 中：

- dte<0 的月已被清除；
- dte==0（今天到期）的月**留在 map 但 front 跳过它**；
- 除此之外 front == begin()->first。

即 **front 状态的唯一真正价值是处理"dte==0 当天到期月"的跳过**；其余时刻
它就是一个可以被 `begin()` 替代的缓存。而这个唯一价值也可以用查询式表达：

```cpp
// 查询式 front：与 reevaluateFrontMonth 选法完全一致，无状态
ExpiryDataPtr frontExpiry() {
    ExpiryDataPtr best = nullptr; int32_t bestDte = -1;
    for (auto& [exp, ed] : m_expiries) {
        if (!ed) continue;
        int32_t dte = ed->daysToExpiry();
        if (dte > 0 && (bestDte < 0 || dte < bestDte)) { bestDte = dte; best = ed; }
    }
    if (!best && !m_expiries.empty()) best = m_expiries.begin()->second;  // 全过期回退
    return best;
}
```

到期月数量级 4–12，`getFrontForward()` 每 batch 调用一次，O(n) 遍历成本可忽略。

### 结论

**front month 概念有必要，front month 状态没有必要。**

- 概念（近月基准）：信号层需要"近月期货价 + 近月合成远期"这一对序列
  （RollEma/FrontFutSkew/AtmSig/ForwardSpread），这个需求真实存在，保留。
- 状态（m_frontMonth + 滚动逻辑）：可整体退化为 `frontExpiry()` 纯查询。
  收益：
  1. 删掉 2 个死接口 + 2 个状态变量 + 1 个滚动函数（reevaluateFrontMonth）；
  2. 消除"滚动只在 session begin、盘中到期滞后一天"的窗口——查询式在
     到期瞬间即切换，语义更正确；
  3. 消除 purge/reevaluate/惰性初始化三处赋值与 dte 更新的同步负担；
  4. 日志可保留（查询结果前后对比打 "Front month rolled"）。

### 与方案 A 合并后的终态架构

```
唯一状态层：  per-expiry 路由表 + 各 ExpiryData 的价格/forward（真源）
派生层：      frontExpiry() = min{dte>0}（查询）
              grid 价 = frontExpiry()->getUnderlyingPrice()（查询）
              frontForward = getAtmForward(frontExpiry())
删除：        m_underlyingPrice 写入链、onUnderlyingTick（grid 级）、
              isFrontUnderlyingCode、m_frontMonth、m_frontMonthExpiry、
              reevaluateFrontMonth、setFrontMonth(死)、setFrontMonthExpiryData(死)
保留：        purgeExpired（清理 options/strikes 的真职责）、
              per-expiry tick 路由（价格真源）、m_atmFwdCache（按 expiry 缓存）
```

### 风险与工作量

- 查询式依赖 dte，dte 由 `refreshExpiryDays()`（session begin）更新——与
  状态版依赖完全相同，无新增时序风险；跨午夜夜盘的 dte 语义不变。
- 改动集中在 OptionGrid（约 80 行）+ 3 个 front 相关测试用例重写。
- `OptionExpiryLifecycle` 退化为只调 `purgeExpired()`。
- 需全量重建 + 9 测试 + ASan 短跑 + 生产灰度（建议与换月窗口错开）。

### 建议排序

1. 本轮 11-26 修复已堵住生产漏洞，短期无动作必要；
2. 下一个技术债批次实施"front 状态查询化"（本节）——它使方案 A 的
   grid 价派生化变得更小：grid 价 = `frontExpiry()->getUnderlyingPrice()`，
   两个改动一起做总成本低于分两次；
3. 配置校验 warning（上轮方案 B 的防线）可独立先做。

## 六、执行记录（2026-11-26 当日完成）

> 用户在分析确认后指示"按方案执行"，front 查询化 + grid 价派生化于当日
> 落地。本节记录实际改动，作为后续审计基准。

### 代码改动（src/WtOptionCore）

| 文件 | 改动 |
|---|---|
| ExpiryData.h | `setUnderlyingPrice(px, updateTimeMs = 0)` 增加接收时间戳（freshness 派生源）；`underlyingUpdateTime()` 访问器 |
| OptionGrid.h | 删除 `m_underlyingPrice`/`m_underlyingUpdateTime`/`m_priceMutex`、`m_frontMonth`/`m_frontMonthExpiry`、`setUnderlyingPrice`/`onUnderlyingTick`×2/`isFrontUnderlyingCode`/`reevaluateFrontMonth`/`setFrontMonth`/`setFrontMonthExpiryData`；新增 `frontExpiry() const` 查询 |
| OptionGrid.cpp | `getUnderlyingPrice()` 派生（front ed 价）；`isUnderlyingFresh()` 派生（front ed 时间戳）；`frontExpiry()` 实现（dte>0 最小，回退 begin）；`getFrontMonthExpiryData()`/`getFrontForward()` 转派生；两个 onTick 版本删除门控与 grid 价写入，全局分支改为按月匹配门控写无 own 月的价格+future mid（补上指数场景 ed 价缺口）；`purgeExpired()` 删除 front 状态重置 |
| OptionExpiryLifecycle.cpp | 删除 `reevaluateFrontMonth()` 调用（保留 refreshExpiryDays + purgeExpired） |

### 语义变化（有意为之）

1. **全局标的 tick 不再被"记住"**：派生模型下全局 tick 只写入已存在的
   无 own ed；早于 ed 创建到达的全局 tick 不再留下痕迹（原 grid 状态会
   记住）。启动空窗 = 一个行情周期，readiness gate 本来阻挡报价。
2. **指数场景 ed 价从"恒 0 靠 fallback"变为"有值"**：全局分支现在给
   无 own 月写 setUnderlyingPrice（带 P0-② 月份门控），数值与原
   fallback 等价。
3. **盘中到期即时切换**：front ed 过期瞬间 frontExpiry() 即转向下一月
   （原状态版滞后到下个 session begin）。

### 测试适配

- `test_optiongrid_market_freshness`：重排为"先建 ed 再发全局 tick"，
  并新增"无 ed 时 not fresh"断言（新语义）。
- `test_market_readiness_gate`：全局指数 tick 移到 option ticks 之后
  （同一时序约束）。
- `test_volspread_scanner_semantics`：`grid->setUnderlyingPrice` 改为
  创建 ed 后 `ed->setUnderlyingPrice`。
- `test_p0_forward_month_routing` / `test_front_roll_excludes_stale_global_underlying`
  / `test_multi_expiry_underlying_isolation`：断言零改动即通过（dte 默认 0
  → frontExpiry 回退 begin，路由行为不变）。

### 验证

- 原生盘全量构建 WtOptionCore，9/9 测试二进制 rc=0（含全部 front 相关
  用例）；TestUnits 无依赖。
- 生产部署 `libWtOptionCore.so` 8a154ffa…（旧版备份
  `.bak-20261126-refactor` 与 `.bak-20261126-frontfix`），进程重启为
  3586795，订阅与通道恢复正常。
- 日盘已收盘，夜盘（21:00）行为验证挂在 aut_21736f27f001s0IQBaRyguNp9U。

### 场景分派修正（当日复审后追加，用户指正）

> 用户指正：underlyingPrice 是每个 expiry 都有的属性，不是 frontExpiry
> 专属；指数标的/郑商所系列期权是"一个 underlying → 多个 expiry"（全场
> 共享一条价格序列）；商品期货才是分月标的（front month + 其他到期月）。

复审发现纯 front 派生实现的偏差：单标的场景把"代表价"也挂到 front ed
上，引入不必要的路径依赖——**全局 tick 早于任何 ed 创建到达时 grid 价
=0**（原实现能立即记住全局价；空窗虽被 readiness gate 覆盖，但对单标的
场景无必要）。

修正为**场景分派**：

| 场景 | 判定 | grid 代表价 |
|---|---|---|
| 单标的（指数/品种级代码，无 own 注册） | `m_expiryOwnUnderlying.empty()` | `m_globalUnderlyingPrice`（全局 tick 直接刷新，不依赖 ed 存在） |
| 商品分月（有 own 注册） | 非空 | `frontExpiry()->getUnderlyingPrice()` |

- onTick 入口（两个版本）：凡 `stdCode == m_underlyingCode` 的 tick 必
  刷新全局价状态——因为指数代码会被 `setExpiryUnderlying` 注册进
  per-expiry 路由表（gm.size()!=4 走 index 分支），实际不到达全局分支，
  状态刷新必须与路由无关。
- 全局分支保留给 ed 喂价（P0-② 月匹配门控）。
- 新增测试锁定两种场景（单标的：全局 tick 先于 ed，grid 价立即可用；
  多标的：全局 tick 不得成为代表价）。
- 部署 `libWtOptionCore.so` 6ee5d7aa…（备份 `.bak-20261126-scenedispatch`），
  进程重启为 3591455；夜盘验证 aut_217483372001bDHpmyC74Baf4i。

## 五、front month 与全局标的的本质关系（用户追问）

> 用户问题：front month 本质上应该是主力/最活跃合约月份的基础，与全局
> 标的从本质上的作用是否可以一致？是否能这么理解？

### 直接回答

**理解方向正确：两者本质上是同一职责的两种选取规则。** 但需修正一个
概念细节：当前实现的 front month 是**日历近月**（dte>0 取最早到期，
OptionGrid.cpp:649-656），**不是流动性主力月**——代码中不存在任何
持仓量/成交量驱动的月份选取（已全仓检索证实）。

### 职责对照：同一职责、两种规则

| 维度 | 全局标的 | front month |
|---|---|---|
| 本质职责 | 给非逐月消费方提供"代表标的价格" | **同一个职责** |
| 选取规则 | 配置的固定合约代码（静态） | 最早有效到期月的专属合约（动态） |
| 服务消费方 | grid 价 → 信号/风控/记录 12 个消费点 | 完全相同的 12 个消费点 |
| 当前实现位置 | onTick 全局分支直接驱动 grid 价 | per-expiry 分支经 isFrontUnderlyingCode 门控驱动 |

### "主力月"与"日历近月"的差异（概念澄清）

实务上商品期权两者几乎总是一致的：近月期权最活跃，其标的近月期货也最
活跃。差异场景仅在**近月到期前最后 1–2 个交易日**——流动性可能已移向
下月，日历规则仍选近月。但日历规则反而**更稳定**：不随流动性噪声切换
信号基准，且与到期日/定价链的绑定更清晰。若未来真要"主力月"，需引入
持仓量驱动的选取规则，那是增强而非缺陷修复。

### 统一模型（对前两次分析的印证）

把两者统一为**"基准价格"（reference price）**单一概念：

```
基准月   = frontExpiry()          // 最早有效到期月（纯查询）
基准价   = 基准月 ed 的价格        // 真源 = 该月专属合约，或全局标的回退
全局标的 = 各月无专属合约时的共享回退源 + 行情订阅对象
```

11-26 修复后的 isFrontUnderlyingCode 语义（front 有 own → 只有 own 驱动；
front 无 own → 全局标的驱动）**事实上已经是这个统一模型**——只是代码里
仍保留两个概念名、两条 tick 路由、一个门控函数。残留的是结构冗余，
不是语义分歧。

### 历史成因（概念债务如何形成）

1. 系统从单标的场景（指数期权/单一商品）起步：只有全局标的，直接驱动
   grid 价；
2. 后来支持多到期月商品期权：每月有了自己的合约（per-expiry own），
   但信号层仍需要单一代表价 → 引入 front month 作为"哪个月代表全场"的
   选择器；
3. 两个概念各自演化出路由、门控、状态维护，从未被识别为"同一职责的两
   个规则"——这正是三次同型 bug（11/17、11/18、11/26）都出现在两者边界
   上的深层原因。

### 全局标的不与 front 重叠的独立职责（统一后保留）

1. **无 own 月份的 per-expiry 价格回退源**（__getOrCreateExpiryData:488
   + P0-② 月份匹配路由）——这是定价真源的回退，与基准价无关；
2. **行情订阅对象**（全局标的合约本身被订阅）；
3. **单标的场景的唯一价格源**（此时基准月就是那个唯一月，其价格真源
   即全局标的）。

### 结论

用户的理解成立：front month 与全局标的本质作用一致，统一为"基准价格"
概念在语义上正确、在代码上已由 11-26 修复先行落地。后续简化的目标
从"删除 front 状态"进一步明确为：**消灭"两个名字一个职责"的结构冗余**——
保留"全局标的=回退源/订阅对象"的真实职责，把"代表标的价格"的职责
收敛到 frontExpiry() 派生链一个出口。前两次分析的终态架构与工作量
评估不变。
