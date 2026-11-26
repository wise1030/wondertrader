# WtOptionCore 诊断：UI 多层挂单与挂单价偏离 theo（2026-11-26 夜盘）

## 结论速览

UI 显示"max_side_orders=1 下仍多层挂单、价格严重偏离 theo"的根因是：
**SimNow 交易回报流严重不可靠（撤单确认全天丢失 2209 次，夜盘时段连新单
回报都无法到达）**，OQM 在回报缺失下持续"发单→改价→撤单→无确认→重试/
重发"，IssuedOrderRegistry 只认终态回报，导致大量 **ghost 单**（发了但
无任何回报下文）在 UI 显示为 live 并层层堆积。**max_side_orders=1 的闸在
OQM 状态机视角一直工作正常**，挡不住"柜台/通道层数"——因为那些层在
OQM 看来已死（超时/reject 清除），只是终态回报丢了。

## 重要修正（2026-11-26 深夜，代码复核后）

本文下方"高偏离单 = power fallback 曲线 v1 的产物"的归因**不准确**，修正为：

- 幽灵单全部是 **cu2701（202701）**；21:05:38 的 fallback 日志只针对
  **202704**（`a=2.2972`），202701 当次没有 fallback，且 `weight=0` 下由
  curve2 linear 定价，却同样以约 2.8–3 倍 vol 挂单。power 不是 cu2701
  爆炸价的直接来源。
- 直接根因是 **ATM 尺度竞态**：首个拟合用默认 `m_defaultVolatility=0.1`
  归一化（points=IV/0.1≈2.4–3.0），随后 bootstrap 把 `atmVol` seed 到市场
  值 ~0.24–0.30，但未重标定/重拟合已发布曲线，导致绝对 IV = 2.3–3.0 ×
  市场 ATM，theo 整体放大 2.3–3 倍。
- 该竞态**同样污染 curve2 linear**（weight=0 的实际定价曲线，202701），
  所以仅关 fallback 不能根治；fallback 只是让 202704 也通过的那道"放行"。
- 完整证据链与修复建议见
  [fallback-curve-selection-review-20261126.md](fallback-curve-selection-review-20261126.md)。

下方历史内容保留作时间线记录，其中"power fallback 导致 cu2701 爆炸价"
的表述以本节为准。

## 证据链（2026-11-26 夜盘，进程 3638717，21:05:19 启动）

1. **UI 快照**：30 个"live"单（20 卖 12 买→32 记录），同合约最多 3 层
   （如 P.84000 挂 1080/2204/7998），价格横跨 570–13986，而同期
   theo≈1458（P.84000）、市场盘口 1456/1468——偏离数百至数千点。
2. **UI orders 数据源**（HftOptionStrategy.cpp:5590-5604）：来自
   IssuedOrderRegistry，仅过滤 `done || rejected`——**无终态回报 = 永远
   显示 live**。
3. **柜台 CSV 交叉核对**（generated/traders/simnow/orders.csv，
   GBK）：
   - UI 显示的 30 个 id **全部不在 CSV**——即这 30 个单发出后**连挂单
     回报都未到**（CSV 由回报驱动）；
   - 新进程（21:05 后）CSV 已记录 2776 单：2729 "已撤销"、36 "全部成交"、
     11 "全部成交（撤单提交）"——**已收到终态的单 UI 均正确退出**。
4. **撤单确认丢失规模**：全天 `cancel ack timeout` **2209 次**（9:04 即
   出现，非夜盘特有）；21:06:51-55 批量触发
   `cancel ack lost after 3 retries, cancelAll sweep issued; instrument frozen`。
5. **CTP 报错**（GBK 解码）：`报单已全成交或已撤销，不能再撤`（21:19:58
   起反复）——撤单请求被柜台拒绝/状态错乱。
6. **OQM 容错全链路触发**：reject retry（attempt 1/3→3/3）持续刷屏；
   cancel timeout→cancelAll→instrument frozen；reject-retry 重发。

## 机制（ghost 多层形成链）

```
1. OQM 发单 A(px1) → registry 登记 live
2. theo 变动 → OQM 撤 A + 发单 B(px2)；A 等撤单确认
3. SimNow 回报流断裂：A 的撤单确认永远不来了
   （同时 B 的挂单回报也可能不来 → CSV 无记录）
4. OQM 侧：cancel 超时 3 次 → cancelAll sweep → instrument frozen；
   或 reject 路径把 A 置 Dead/erase → activeSideCount 降回 0
5. OQM 视角"该边已清空"→ max_side_orders 闸放行 → 发单 C(px3)
6. registry 视角：A/B/C 全无终态 → UI 三层并存，价格各停发出时刻
7. 价格偏离：px1/px2/px3 各自对应不同时刻 theo，
   改价链路（撤旧挂新）因确认断裂而层层残留 → 偏离当前 theo 数千点
```

## max_side_orders 的真实状态

- **OQM 视角：有效**。activeSideCount 计入 cancel-pending（11-24 修复），
  闸在 updateSide 全部发送点 enforcement；OQM 内部同时刻每边≤1 个 active。
- **UI/柜台视角：无此约束**。registry 只问"终态回报到了吗"。
  两者因回报丢失而 divergence——**这是观测层 ghost，不是层数限制失效**。
- 柜台上这些 ghost 单大概率已终结（CSV 中同时段 2729 单均"已撤销"），
  但无回报就无法收敛；也不排除个别单实际仍挂在柜台（SimNow 状态错乱）。

## 环境定性

- **SimNow 全天撤单确认丢失**（2209 次超时，日盘 9:04 即有），夜盘开盘
  时段进一步恶化到新单回报都无法落 CSV。
- 策略容错机制（超时重试→cancelAll→冻结→reject 重发）按设计工作，
  但在回报持续缺失的环境下会产生大量 ghost 登记。
- **实盘 CTP 回报可靠，此问题主要为仿真环境缺陷**；但对仿真评估的
  报撤比、义务统计有实质影响（大量撤单未确认会虚增撤单计数与拒单率）。

## 深层定位：高偏离单 = 重启初期错误 vol 曲线 v1 的产物（策略侧真问题）

追问"30 个无回报单的来历与偏离成因"后的完整因果链（日志逐秒核实）：

| 时间 | 事件 |
|---|---|
| 21:05:19 | 进程重启（夜盘开盘前人工重启） |
| 21:05:35 | Recovery complete（首个 tick 即激活，曲线未拟合） |
| 21:05:37 | `SLOW #1: priced=0/150 fwdNotReady`——priced 门控正确挡住报价 |
| 21:05:38 | AsyncVolFitter 用默认 ATM 尺度 0.1 归一化（n=15，points≈IV/0.1≈2.4–3.0）→ 202701 的 curve2 linear 成功（weight=0 实际定价）、202704 GVV 形状越界→power fallback `a=2.2972` → 四个月全部 `ready — vol fit published (curve v1)`；publish 写回 atmVol=0.1 |
| 21:05:38+ | bootstrap 检测到 atmVol 仍为默认 0.1 → seed 到市场 median ~0.24–0.30，**但未重标定/重拟合曲线** → 绝对 IV = 归一化倍数(≈2.4–3.0)×市场 ATM → theo 整体放大 2.3–3 倍 |
| 21:05:39-41 | 曲线一 ready，CTG/OQM 立即按**爆炸 theo** 挂出第一批单（Trader 日志 Smtting 确认发出：id 920-926，价格 10172/13986/9404/8384/13312...） |
| 21:06:27-34 | 第二版拟合（`a=1.0110` 正常）→ theo 回落 → 改价撤旧 |
| 21:05:40 起 | **撤单确认大面积丢失**（全天 2209 次超时）→ 第一批高偏离单成为 ghost |
| 21:23-21:39 | 回报通道中断：期间发的单既无 CSV 记录也无终态 |
| 21:39+ | 回报恢复，ghost 逐步收敛 |

**两个问题的答案**：

1. **单是哪来的**：全部是本进程 OQM 正常做市发单。registry 在**发单瞬间**
   登记（IssuedOrderRegistry::record 于 doEntrust 路径）；orders.csv 由
   **回报驱动**——21:23-21:39 回报中断使这批单"发了但无下文"，并非
   没有发单。发单动作有 Trader 层 Smtting 日志实证。

2. **价格为何严重偏离**：偏离 4800–6500 的 11 个卖单（P/C 各价位构成
   完整但错误的期权曲线形态）来自 **21:05:38 第一版曲线的 ATM 尺度错误**：
   该版拟合用默认 `m_defaultVolatility=0.1` 归一化（points≈IV/0.1≈2.4–3.0），
   随后 bootstrap 把 `atmVol` seed 到市场值 ~0.24–0.30，但已发布曲线未
   重标定 → 绝对 IV = 2.4–3.0 × 市场 ATM，theo 整体放大 2.3–3 倍。
   这些卖单是 **cu2701（202701）**，weight=0 下由 **curve2 linear** 定价
   （其 GVV 主曲线也未触发 fallback，但不参与 theo）；
   202704 的 power fallback `a=2.2972` 只是同一尺度问题的伴随产物，不是
   cu2701 的直接定价曲线。21:06:34 第二版拟合用已 seed 的 atmVol
   （`a=1.0110`）→ theo 正常；撤单确认丢失使旧价单残留 UI。

**策略侧真正待修的问题**（区别于 SimNow 环境缺陷）：

1. **重启初期 ATM 尺度竞态（P0，比 fallback 更优先）**：首个拟合用默认
   0.1 归一化，bootstrap 随后把 atmVol seed 到市场值却未重标定/重拟合
   曲线 → 所有模型（含实际定价的 linear）的倍数整体放大 2.3–3 倍。发布前必须
   要求 atmVol 非默认；atmVol 变更时曲线必须同步重标定或强制重拟合。
2. **fallback 绕过配置契约**：GVV 失败被 power 静默替换并解锁 ready。
   按配置选择原则，失败应不发布（冷启动）/保留旧曲线（热态），fallback
   应移除或默认关闭；但它只能挡住"被 GVV 拒绝"的月份，挡不住第 1 条。
3. **Recovery 激活偏早**：首个 tick 即 Recovery complete，与曲线拟合
   的竞态只有 1 秒（21:05:37 fwdNotReady → 21:05:38 错误曲线 ready →
   21:05:39 错误价挂出）。激活条件应包含"首版曲线尺度/质量达标"。

环境侧（撤单确认丢失、回报中断）加剧并固化了 ghost 呈现，但**高偏离
价格的来源是尺度错误的曲线 v1（默认 0.1 归一化 + seed 后未重标定）**，
这是策略可以在任何环境复现的问题。

## 处置建议

1. **短期（仿真运维）**：接受此环境限制；UI 侧可对"超时无终态回报"的
   registry 记录灰显（issuedTime 超龄，如 >60s 无终态），区分"真 live"
   与"ghost live"；对外提供做市义务统计时需剔除无确认报单的口径说明。
2. **中期（工程）**：
   a. registry 增加"presumed-dead"收敛——cancelAll sweep
      或 cancel 超时耗尽时，将涉及 localid 标记终态（done=cancelled-unknown），
      UI 不再显示 live；迟到回报到达时以"late-after-done"计数告警。
   b. **ATM 尺度一致性（P0）**：`submitAsyncVolFit` 在 atmVol 仍为默认
      0.1 时不提交；`publishFittedCurves` 校验 snapshot atmVol 与当前
      atmVol 一致，不一致/默认则丢弃且不置 ready；bootstrap seed 后必须
      同步重标定曲线或强制重拟合。详见
      [fallback-curve-selection-review-20261126.md](fallback-curve-selection-review-20261126.md)。
   c. **移除 fallback 的配置契约**：删除 fallback 链或默认关闭；只有
      配置模型成功才允许 ready；power/linear 需补与 GVV 等价的质量检查。
   d. Recovery 激活条件加入"首版曲线尺度/质量达标"（并保留 ready 后
      延迟 N 秒/连续 2 版一致的启动保护）。
3. **验证建议**：仿真材料引用报撤数据时，以柜台 CSV（回报驱动）为准，
   不以 UI/registry 快照为准。
