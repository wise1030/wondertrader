# §20 ORPHAN FILL 修正方案合理性复核（只读）

> 复核对象：`src/WtFutuCore/AGENTS.md` §20“ORPHAN FILL 修正方案（详细版）”
> 复核时间：2026-10-18 夜（非交易时段）
> 远端：生产机 `ubuntu@<prod-host>`，只读巡检，未改任何进程/配置/文件
> 代码：本地 HEAD，仅阅读；本轮未改代码

## 1. 结论

**方向正确，证据基础强；但方案不完整，不能直接按原批次上线。**

- 远端 10-13/10-14 的 15 笔 ORPHAN 与“盘中重启 + 重启前仍有存活单”强相关，方案对事实的描述基本准确；
- 但方案的核心动作是“重启后认领 + 抑制 ORPHAN + 修 undone”，它**没有阻止幽灵单继续存活/成交**，也没有让策略侧知道这些单；
- 远端 10-14 14:50 的证据表明：策略在 ORPHAN 成交后仍以 `local_net=1/3/-10/10` 恢复交易，方案不会改变这一注入；
- 方案未覆盖一个更直接的幽灵来源：`cancel()` 缓存未命中时**合成 volume=0 的终态单**（`TraderAdapter.cpp:2330-2350`），日志中每日 1.8k–3.8k 条；
- 方案的 `onSessionBegin` 清 Dedup 在 `TraderAdapter` 中没有对应回调；`onRspOrders` 的“首个 localid 水位”也没有明确的初始化点。

**评分（1–10）**：证据可信度 8，根因覆盖 5，实施安全 5，可观测/回退 7，综合约 **6/10**。建议先补 P0 缺口，再考虑上线。

## 2. 远端实证

### 2.1 ORPHAN 分布与重启相关性

| 日期 | 盘中重启（日志 `Trading day ... begun`） | ORPHAN | 关键证据 |
|---|---|---|---|
| 10-13 | 00:19:15、14:16:24、14:24:01、14:33:33 | 5 | 00:23:47/49、00:59:52/54/57；ID 1230668134/141/135/137/139 |
| 10-14 | 11:32:39、14:44:49 | 10 | 13:30:11-13 三笔；14:50:19 七笔；ID 1237155880/81/82、1237958402…426 |
| 10-15 | 仅 08:40、20:40（非盘中） | 0 | — |
| 10-16 | 仅 08:40、20:40（非盘中） | 0 | — |
| 10-17 | 夜盘，无盘中重启 | 0 | 仅 9 笔成交 |

- 10-13 有 6 次启动，但只有 00:19 那次产生 5 笔；14:16/14:24/14:33 三次重启为 0。说明“重启”是必要条件，**不是充分条件**；更准确的表述应是“重启时仍有存活单/可成交残留单”。
- 10-15/16 无盘中重启，ORPHAN 为 0；当前生产进程已连续运行约 2 天 1 小时（`ps etime 2-01:27`），10-17 至今无 ORPHAN。相关性成立。

### 2.2 10-14 14:50 的完整时序

```text
14:44:43  UnifiedOrderTracker: cancel ack timeout (1000ms), retry #1 ...
14:44:43  cancel-error resolved: synthesized terminal order ... (insert lost, no exchange record)
14:44:49  Trading day 20261014 begun          ← 盘中重启
14:50:19  INE.ec2611/ec2612/ao2611/ao2612 undone quantity 22
14:50:19  7 笔 ORPHAN FILL（ao2611 1、ao2612 1+10、ec2611 1+10、ec2612 1+10）
14:50:24  Trading channel ready
14:50:24  Position sync: portfolio=0 -> local_net=1/3/±10
14:50:24  Trading resumed after channel ready
```

关键点：

1. 幽灵单是**重启前已存在**的存活单，重启后失认领；14:50:19 的 `undone=22` 说明订单查询确实看到了在途单。
2. 这 7 笔成交发生在 `Trading channel ready` **之前**，策略尚未完成本地恢复；成交已经进入柜台/适配器持仓。
3. 14:50:24 策略仍以 `local_net=1/3/-10/10` 恢复交易，说明**策略侧并不知道这些单是上一进程的**；方案批 2 只改适配器 `_known_localids`，不会改变这个事实。
4. 10-14 11:32 重启后的 13:30 三笔同理：`undone=1/12/1/1` 的在途单在午休后开盘成交。

### 2.3 当前生产版本

```text
WtUftRunner          5ed33e19c6c041098ff5a120f80ea644
libWtUftCore.so      4e29fc359103ee205dedb89a44d36a91
uft/libWtFutuCore.so 2b0480bb40c5249abd05a3885d30bf0f
```

- Runner 仍是 9 月 13 日构建的 v3.2 对子；`libWtUftCore.so` 与 AGENTS 文档中记载的 9d587429/a0c03695 不同，说明生产上还有未入档的 hotfix/构建。
- 方案若给 `TraderAdapter.h` 增加 `_traded_ids`、水位、日期成员，`WtUftRunner` 必须同树重编译、成对部署；这一点方案已写明，但上线前必须以**当前**生产 md5 为基线，不能按旧文档假设。

## 3. 逐批判定

### 批 1：TradeID 去重

**合理部分**

- CTP `WTSTradeInfo` 确实有 `TradeID`（`src/Includes/WTSTradeDef.hpp:456`），实时 `onPushTrade` 与初始化 `onRspTrades` 是两条路径；去重放在 `onPushTrade` 不会误杀回放。
- 重复推送会双计仓位/sink，去重方向正确。

**问题**

1. **TradeID 作用域未定义**：CTP `TradeID` 是“交易所成交编号”，同一交易日内跨交易所（SHFE/INE）可能重复。方案只用 `std::unordered_set<std::string>`，没有 exchange/code 维度，存在**误杀合法成交**的风险。远端同时交易 SHFE.ao 与 INE.ec，不能假设全局唯一。
2. **清理点不存在**：方案写“onSessionBegin 清空”，但 `TraderAdapter.h` 没有 `onSessionBegin` 回调，只有 `onRspAccount/Position/Orders/Trades/Entrust`。照方案实现会找不到落点。
   - 更稳妥：用 `WTSTradeInfo::getTradeDate()` 做日期切换清理（该字段存在），或把日期并入 key。
3. **没有证据证明重复存在**：远端 `Trade notified` 行不打印 TradeID，15 笔 ORPHAN 也都是唯一 ID；方案没有 shadow 统计。直接启用去重，误杀无法在日志中定位。
4. `onRspTrades` 只更新统计，不触发 `onPushTrade`；这点方案判断正确。

**建议**：先 shadow（只统计 `dup` 与冲突 key，不 return），key 用 `exchange + tradeID` 或 `tradeDate + exchange + tradeID`；确认零误杀后再启用。

### 批 2：重启边界认领（核心）

**合理部分**

- 在 `onRspOrders` 的 `_orderids.insert` 同处插入 `_known_localids`，位置正确（`TraderAdapter.cpp:1690`，循环内 `_orderids.insert`）；只认 prefix 匹配且 `isAlive()` 的单，方向正确。
- 认领后 `_close_freeze.seed` 与 `updateUndone` 的基线自洽；对重启孤儿的 `undone` 修正是必要的。
- 未知外来单跳过 `updateUndone`，可消除负漂；方向正确。

**关键问题**

1. **没有“首个 localid”的初始化点**：`makeLocalOrderID()` 是 lazy 的（`TraderAdapter.cpp:38-48`），第一次调用发生在 `doEntrust`；`onRspOrders` 通常发生在首笔报单之前。方案说“仅播种 localid <= 本进程首个 makeLocalOrderID() 的单”，但此时“首个”尚未产生。必须在 adapter 构造/`onRspOrders` 入口显式捕获一次并保存水位，否则水位比较无意义。
2. **同秒/重叠重启会误拒**：new 进程水位按“当前秒 × 50”计算；若重启发生在同一秒，旧进程已发出的 localid 可能大于新水位，旧孤儿不会被认领。需要留出同秒窗口，或使用“进程启动时间 + 在途上限”的水位，而不是严格首 ID。
3. **更早启动的同 prefix 实例会被误认领**：水位只挡“更新的外来单”，挡不住“更早启动、同 prefix、仍在运行”的另一实例。当前生产还有 Python 客户端，但其 usertag 不同；若未来同 prefix 多实例，方案会误认领。方案只写“留观测口”，不足以称为“不认领外来单”。
4. **认领后不撤单**：方案只 `insert` 已知集合 + `seed` 台账，没有取消这些存活单。它们仍可在盘中成交（10-14 的 7 笔就是认领窗口内的存活单），策略仍然不知道。**这是方案最大的行为缺口**。
5. **认领没有通知策略**：`onRspOrders` 只更新适配器 `_orders`/台账/统计，不向 sink 推送订单，也不更新策略本地持仓。10-14 14:50:24 的 `local_net=1/3/±10` 证明策略仍带着幽灵库存恢复交易；认领后 ORPHAN 被抑制，反而**降低了可见性**。
6. **成交先于查询的竞态**：若 `onPushTrade` 在 `onRspOrders` 之前到达，订单尚未被认领，仍会 WARN；方案“预期归零”不成立。可以接受（真未知），但验收标准应允许这种 race，不能以 ORPHAN=0 作为唯一门槛。
7. **查询范围假设**：方案依赖 `onRspOrders` 能看到隔夜/上一交易日残留单。SimNow 日单可能跨日存续，但 CTP 查询通常只返回当前交易日订单；若查询不含隔夜单，认领不会发生。需要在生产验证，或补 `queryOrders` 之外的订单查询路径。

**建议**：认领后立即走撤单链（`cancelOrder`/`cancelAll`），等终态；同时发出 `ADOPTED` 告警并把 `adopted>0` 作为策略“暂停新开仓/待对账”的门禁。认领不能只“知道”，必须“处置”。

### 批 3：重启纪律

**这是方案中收益最高、风险最低的一批。**

- 远端直接证明：10-14 14:44:43 撤单超时 → 14:44:49 重启 → 14:50 成交；10-14 11:29 撤单异常 → 11:32 重启 → 13:30 成交。盘中重启是幽灵单的直接制造者。
- 10-15/16 只有 08:40/20:40 计划重启，ORPHAN=0；反向验证强。
- 控制文件优雅退出（全撤→等 `undone=0`→退出）方向正确，且是策略层+脚本，无 ABI 风险。

**缺口**

- 方案批 3 只覆盖“部署重启”，没有覆盖**每日 session end 的 cancelAll fire-and-forget**。10-13/10-14 的幽灵单正是节末/重启前撤单未确认留下的；如果每天收盘仍不等待撤单确认，即使不做盘中重启，隔夜/次日开盘仍可能产生残留单。
- 应在 `onSessionEnd` 的 cancelAll 后等待 ack/`undone=0`（超时升级/阻断退出），或至少在收盘后核对 `undone` 并告警。这个入口比“禁止盘中重启”更根本。

### 批 4：观测与验收

- `trade-guard` 巡检行、`adopted/dup/orphan/undone_min` 方向正确；但 `undone_min` 当前远端日志没有负值样本，负漂是代码层推断，需要上线后验证。
- 验收矩阵只测 `TradeIdDedup` 组件，**没有 adapter 级测试**。方案自己也承认“适配器报单路径零测试覆盖”（§13 死锁事故教训），这次必须补：
  - `onRspOrders` 认领 vs 不认领；
  - 成交在查询前/后；
  - 同秒水位、较早同 prefix 实例；
  - TradeID 同值不同交易所；
  - 认领后撤单、终态、undone 归零；
  - 日期切换清 dedup。

## 4. 方案未覆盖的更重要根因

### 4.1 `cancel()` 缓存未命中时合成终态单

远端每日 `synthesized terminal order ... (insert lost, no exchange record)`：

| 日期 | synth 次数 | live-left |
|---|---:|---:|
| 10-13 | 3,559 | 434 |
| 10-14 | 3,847 | 0 |
| 10-15 | 1,812 | 209 |
| 10-16 | 3,255 | 51 |

代码在 `TraderAdapter.cpp:2330-2350`：缓存未命中时直接构造 `volume=0 / WOS_Canceled` 的骨架单并 `onPushOrder`，日志称“no exchange record”，但**并未向交易所查询**。真实订单可能仍存活；这类单之后成交时，因为 `_known_localids` 已在 `doEntrust` 登记，**不会触发 ORPHAN**，可能静默造成持仓/台账漂移。

这不是 §20 批 1/2 能解决的：批 2 只处理“重启后查询到的存活单”，批 1 只处理重复成交。建议把该路径列为同批 P0：缓存未命中时不得直接合成终态；应重试撤单/查询订单，或至少保持 pending 并发出 `CANCEL-UNCONFIRMED` 告警。

### 4.2 策略侧没有对账门禁

- 10-14 14:50:19 的 ORPHAN 成交进入适配器持仓；14:50:24 策略 `Position sync` 后直接 `Trading resumed`。
- 方案明确“不拦截持仓账/sink 转发”，因此策略仍会带着幽灵库存报价/平仓；10-14 14:59 的 `CLOSEOUT Max retries` 就是在这个基础上发生的。
- 认领 + 抑制 ORPHAN 会让这条链更安静，而不是更安全。必须在策略侧加“启动/重连后仓位对账门禁”：`adopted>0` 或 `|策略本地仓位-柜台|>阈值` 时暂停新开仓，先人工/自动对账，不能直接恢复。

### 4.3 收盘平仓失败是独立 P0

10-14 14:59 `Max retries (10) exhausted` 与 10-16 的 closeout 失败说明：即使没有幽灵单，收盘平仓也可能留下仓位。§20 的影响链把亏损归因到孤儿，但方案没有修 closeout；归因和修复应分开，不能用一个 ORPHAN 方案承担。

## 5. 建议的修订顺序

1. **P0-预防（先做）**
   - 盘中重启禁令 + 部署窗口规程；
   - `onSessionEnd` cancelAll 等待确认/`undone=0`，超时阻断退出；
   - 审计并修正 `cancel()` cache-miss 合成终态路径。
2. **P0-处置（再做）**
   - 启动捕获 localid 水位；
   - `onRspOrders` 认领存活 prefix 单后**立即撤单并等终态**；
   - 发 `ADOPTED` 告警；`adopted>0` 触发策略“暂停新开仓/待对账”，不自动恢复。
3. **P1-去重（shadow 后启用）**
   - `exchange + tradeID`（或 `tradeDate + exchange + tradeID`）key；
   - 按 `getTradeDate()` 清空；
   - 先只统计 dup，日志打印 TradeID，确认零误杀再 return。
4. **P1-验收**
   - adapter 级单测（上述矩阵）；
   - 用 10-14 14:50 的时序做回放/注入；
   - 生产 A/B 至少覆盖一次“计划重启 + 一次模拟残留单”；
   - Runner/lib 成对 md5。

## 6. 本轮实施进展（2026-10-18 本地，未部署）

- `TradeIdDedup`：`交易日|交易所|TradeID` 去重，空 ID 跳过，已接入 `onPushTrade`；5 个单测通过。
- 未知成交不再扣 `updateUndone`；ORPHAN 日志补 `dir/off`。
- `cancel()` 缓存未命中不再合成假终态，改 `CANCEL-UNCONFIRMED`。
- `UftStraContext::stra_cancel_all` 改为策略级；`FutuRuntimeOps` zombie 清扫改按 localid 精确撤单。
- 构建：WtUftCore/WtFutuCore/WtUftRunner/TestUnits 成功；远端未部署。
- 未完成：legacy 子账、session-end 撤单确认、策略 ID/owner 持久化、账户 residual。

## 7. 明确结论

- 该方案可以保留为“适配器侧孤儿认领与去重”的基础设计，但**不能单独作为 10-14 类事故的修复方案**。
- 若只按原批次上线，最可能的结果是：ORPHAN 告警减少、`undone` 负漂减少，但幽灵单仍可能成交、策略仍带着错误库存恢复、收盘仍可能被动平仓——**风险被降噪而非消除**。
- 建议按“预防 → 认领并撤单/策略门禁 → 去重 shadow”的顺序重排，并把 `cancel()` 合成终态、session-end 撤单确认、策略对账门禁列为同批 P0。
