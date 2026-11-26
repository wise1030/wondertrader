# WtOptionCore 诊断：front-underlying 跳变与交易通道断开（2026-11-26）

## 现象

1. 用户观察到 UI 上期权 theo、ordBid/ordAsk 价格严重跳变。
2. 用户感觉交易通道又断了。

## 事实时间线（Asia/Shanghai，进程 3439200，10:16:56 启动）

| 时间 | 事件 |
|---|---|
| 10:30:35 | Recovery complete；同时触发 `Market data stale (>3000 ms)`，此后未恢复 |
| 11:09:31 | `[simnow] Trading channel disconnected: 8193`；CTP 同秒 re-login 成功，发出 settlement confirm 查询后**再无响应**，策略始终未收到 `Trading channel ready` |
| 11:09:49 | `Market data fresh again`（行情恢复新鲜） |
| 11:12:21 | `[ParserCTP] Market data server disconnected: 8193`（行情通道又断，后自行恢复） |
| 11:18:05–11:28:58 | batch 日志 underlying 在 **89600–89900 ↔ 91900–92300 两簇间规律交替**（单批间隔级跳变，价差约 2500 点） |
| 11:29:32 | 上午盘收盘（Session stop / CTG::tradingStopMidDay） |
| 11:31–11:51 | 午间休市，batch 间隔拉长至 ~101s，underlying 静止在 89700 |
| 11:52 | 人工重启（部署修复版 .so） |
| 11:54:57 | 新进程（3477631）订阅完成：150 期权 + cu2612/2701/2702/2703/2704 期货 |
| 11:55:01 | `Trading channel ready`（4 秒，settlement confirm 正常） |
| 11:56:57 | 持仓快照更新，PositionGuard 初始化正常 |

## 跳变根因（已定位并修复）

`OptionGrid::isFrontUnderlyingCode()`（src/WtOptionCore/OptionGrid.cpp）中
`stdCode == m_underlyingCode → true` 的短路逻辑存在场景漏洞：

- 生产配置：全局标的 `underlyingCode = SHFE.cu.2612`（202612 的合约，已到期）。
- B1 到期订阅过滤后，202612 期权不订阅、其 ExpiryData 不创建，
  `reevaluateFrontMonth()` 把 front month 滚到 202701。
- 202701 的专属 own underlying 是 `SHFE.cu.2701`。
- 于是 cu2612 tick（per-expiry 分支，因 202612 在 on_init 注册过路由表）命中
  `stdCode == m_underlyingCode` 短路 → 判定为 front → 更新 grid 级
  `m_underlyingPrice`（89600 簇）；cu2701 tick 通过 front own 检查 → 同样更新
  （92250 簇）。两个 tick 流交替驱动同一变量 → 跳变。

这是 2026-11-17/18 修复（P1-⑦）的同型残留：当时 front=202612、全局标的与
front own 同为 cu2612，短路无害；B1 让 front 滚过全局标的配置后，短路重新
放行旧合约。11/17 观察到的 100900↔103580 与本次 89600↔92250 为同一模式
（月间价差 ~2500–2600 点）。现有测试 `test_p0_forward_month_routing` 只覆盖
"front 无专属标的、全局标的=front 合约"的场景，未覆盖本场景。

## 影响链

grid 级 `m_underlyingPrice` 被污染的下游：

1. **RollEma / FrontFutSkew / ForwardSpread 信号**（AlphaSignals.cpp，
   当前配置全部启用）：skew/roll 在 ±2500 点间跳 → risk shift 叠加到报价 →
   **theo 调整项与 ordBid/ordAsk 跳变**（用户观察到的直接现象）。
2. `CompositeOptionPricer::computeValues` 的 SLOW 参考价
   `refPrice = grid->getUnderlyingPrice()`（OptionPricer2.cpp:347）：
   parity OTM/ITM 配对参考跳变 → theo 全量重算结果抖动。
3. 所有期权行情快照的 `mkt.underlyingPrice` fallback 跳变。

影响限于内部状态与 UI 显示：11:09:31 交易通道断开后策略为 Disabled，
不发单，无实际报单风险。

## 修复

`OptionGrid::isFrontUnderlyingCode`（OptionGrid.cpp）：

- front expiry 有专属 own underlying 时：**只有该合约**可驱动 grid 级价格；
- 否则退回 `stdCode == m_underlyingCode`（单标的/回退场景，行为不变）。

新增回归测试 `test_front_roll_excludes_stale_global_underlying`
（tests/test_enhancements.cpp）：复刻生产场景（全局标的=cu2612 已过期、
front=202701 own=cu2701、202612 ExpiryData 不存在），断言 cu2612 tick 不移动
grid 价、cu2701 tick 正常驱动、交替到达时 grid 只跟随 cu2701。修复前该测试
失败，修复后通过。

## 验证

- 原生盘增量构建通过；WtOptionCore 9/9 测试二进制全部 rc=0。
- 生产部署：`hft/libWtOptionCore.so` 74f58d8a…（旧版
  1741042e… 备份为 `.bak-20261126-frontfix`），进程重启为 3477631。
- 重启后通道 4 秒 ready、持仓恢复正常。
- **13:37 开盘后核验（aut_216012a4f001FYfUfXWWqCGlNq）**：进程存活；
  13:30:15–13:37:08 共 29 个 batch 样本 underlying 全部落在
  89940–90270 单一区间连续波动，双簇跳变消失；UI 快照 202701
  fwd=90215.8 / 202702 fwd=90540.9（月间价差约 325 点），与 batch
  underlying 吻合，证明 grid 价正确跟随 front 合约 cu2701（下午铜价
  午间大变价：cu2701 由上午 ~92250 跌至 ~90200）。修复生效。
- 13:30:06 出现一次开盘后行情迟到 stale（13:38:30 自行恢复 fresh），
  为 SimNow 行情推送延迟，非策略缺陷。

## 边界说明（本次未改）

`onTick` 全局分支（`stdCode == m_underlyingCode`，OptionGrid.cpp:158/248）
仍无条件 `onUnderlyingTick`。当前生产配置 cu2612 在 per-expiry 路由表中，
走不到该分支。仅当配置删除已过期月段、使全局标的不在任何 per-expiry
注册中时，该分支才会放行 stale 全局合约。若未来清理配置中的 202612 段，
需同步把全局标的改为当月 front 合约，或给全局分支加同样的 front 门控。

## 交易通道断开说明

11:09:31 断开属于 SimNow/链路侧问题（当日 09:53 已发生同模式：8193 断开、
re-login 成功、settlement confirm 无响应）。TraderCTP 源未改（用户要求）；
策略 Disabled 不发单为正确保护。重启是有效的恢复手段（本次 4 秒 ready）。
历史全天断连列表：09:33/09:35/09:49/09:59/10:13（行情 8193）、09:53（交易 8193）、
11:09（交易 8193）、11:12（行情 8193）。
