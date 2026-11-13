# EC/AO 做市策略逆向分析 & 对比 WtFutuCore MM

> **数据源**：`dist/WtRunnerFutu/data/做市报单/` 9 天报单（2026-06-01 ~ 06-11）+ `ec成交记录.rar`（7 天）
> **被分析策略**：来自外部交易系统的做市策略，账号 `220888`，未知作者/未知架构
> **对比对象**：本项目 `src/WtFutuCore/UftFutuMmStrategy`（GLFT + Alpha 信号架构）
> **结论摘要**：被分析策略是**单价位、固定心跳、双月联动、纯被动**的"傻瓜做市"；奶酪 WtFutuCore MM 在信号、风控、自适应上**架构压倒性领先**，但有 3 个具体战术细节值得借鉴/警惕。
> **生成时间**：2026-06-11

---

## 1. 数据画像（9 天）

### 1.1 量级

| 维度 | EC（外部策略） | AO（外部策略） | 备注 |
|---|---|---|---|
| 9 天报单总量 | 32.4 万 | **104.1 万** | AO 是 EC 的 3.2× |
| 日均报单 | 35,951 | **115,705** | |
| Fill ratio | **0.19%** | **0.98%** | 极低成交率 |
| 撤单率 | 99.79% | 99.02% | 高频挂撤典型 |
| 平均每单委托手数 | 2.99 | 13.62 | AO 报量明显更大 |
| 平均每成交笔实际手数 | 1.64 | 1.05 | |
| 每成交 1 笔需要报单 | **560 单** | **102 单** | EC 经济性很差 |
| 总品种（同一账号同期） | ec, ao, fu, hc, CY, sp 共 6 个 | | 不只 EC/AO |

### 1.2 严格库存中性（万分位级别）

| | EC | AO |
|---|---|---|
| 买单占比 | 50.00% | 50.00% |
| 卖单占比 | 50.00% | 50.00% |
| 偏差 | 0.06% | 0.01% |

→ 这是**显式约束**（不可能统计巧合），后台一定有双边互锁/skew 平衡器。

### 1.3 合约月份分布

| 品种 | 月份 1 | 月份 2 | 月份 3 | 月份 4 | 主力？ |
|---|---|---|---|---|---|
| EC | 2607 28.5% | 2608 35.8% | 2609 35.8% | — | 跨 3 月平摊 |
| AO | 2607 22.4% | 2608 26.9% | 2610 27.8% | 2611 22.8% | 跨 4 月平摊 |

**关键发现**：不挑主力，跨 3-4 个连续月份**同时报价**。这是和奶酪当前 WtFutuCore MM 最大的差异之一（你的策略以 `anchor_code` + LeadLag 跨合约相关性为核心，但实际报价是按合约独立的 FutuQuoter，**没有跨月联动逻辑**）。

### 1.4 日内时段

- **EC 只做日盘**（09-15），完全跳过夜盘（INE 欧线集运实际有夜盘，**主动放弃**）
- **AO 日盘 + 夜盘双高峰**，22:00 时段（15.4 万）甚至超过日盘最高峰
- **EC 14:00 是最强时段**（87k，是 09:00 的 1.18×）→ 收盘前 1 小时是高峰，**与"日内 flatten"反向**

---

## 2. 微结构指纹（Phase 2 M1-M6）

### 2.1 M1: 报价价差结构 — **单档为主，宽幅"虚单"**

| 指标 | EC | AO |
|---|---|---|
| 同秒同方向只挂 1 档价 | **90.1%** | **91.4%** |
| 挂 2 档 | 9.7% | 8.2% |
| 挂 ≥3 档 | 0.2% | 0.3% |
| 自己 best_ask − best_bid 中位数 | **10.0** | **8.0** |
| 价差 P25/P75 | 10/19.5 | 6/10 |
| 单侧档数中位数 | 1（max=4） | 1（max=5） |
| 穿越价差(<0) 占比 | 0.01% | 0.01% |
| 双边都报的 tick 占比 | 98.1% | 98.4% |

**指纹判断**：
- ✅ **单档报价为主**（>90%）→ 不做"网格/阶梯"做市
- ✅ **双边同时报**（98%+）→ 严格双边
- ⚠️ **价差 10 ticks（EC）/ 8 ticks（AO）远大于盘口最优价差**
- 几乎无穿越（0.01%）→ **不主动 take liquidity**

### 2.2 M2: 重报心跳 — **2-3 秒规律节奏**

同(date, 合约, 方向, 价位)上连续两次报单的时间间隔：

| 间隔 | EC 占比 | AO 占比 |
|---|---|---|
| ≤ 1 秒 | 22.2% | 11.6% |
| ≤ 3 秒 | 62.4% | **87.6%** |
| ≤ 10 秒 | 73.9% | 93.0% |
| > 60 秒 | 15.2% | 3.4% |
| **AO 中位数** | 2s | **2s** |
| AO P25/P75 | 2/12 | 2/3 |

**指纹判断**：**AO 87.6% 重报间隔 ≤3 秒、中位数恒等 2 秒** → 这是**定时器驱动**（不是事件驱动）的强信号。EC 同样 2s 中位但分布更分散（P75=12s），可能是流动性差导致重报间隔被拉长。

→ **结论**：报价节奏不是"on_tick 触发重报"，而是**固定 2-3s 心跳全局撤+重挂**。这种模式延迟容忍度很高但反应慢。

### 2.3 M3: EC 跨月联动 — **三连月同步**

- 每秒同时报价 2 个合约：52.8%
- 每秒同时报价 3 个合约：13.8%
- 仅报 1 合约：47.2%

→ **超过半数时段在多月份同步报价**，与"FutuQuoter 各合约独立"的奶酪架构完全不同。

跨月价差稳定性（基于自己挂的 mid）：
- ec2608 − ec2607 中位数 = **−677.5**，P25/P75 = -732/-625（**跨度仅 100 价格点**）
- ec2609 − ec2608 中位数 = **−974.2**，P25/P75 = -1047/-930

跨月价差日内 std 均值仅 **16-19 价格点**（占价差水平 ~2%），**极度稳定** → 策略本质可能是**跨月做市捕获价差的均值回归**，单合约做市只是手段。

### 2.4 M4: 库存控制 — **EC 严格、AO 失控**

| 合约 | 成交笔数 | 跨日最终净仓 | 仓位区间 |
|---|---|---|---|
| ec2607 | 174 | **−50** | [−54, +6] |
| ec2608 | 398 | **−36** | [−42, +3] |
| ec2609 | 6 | +12 | [+3, +12] |
| ao2608 | 4,605 | **−85** | [−89, +11] |
| ao2610 | 5,526 | **+489** ⚠️ | [−20, **+521**] |
| ao2607 | 28 | +59 | [+7, +93] |

**重大发现**：
- ✅ **EC 仓位严格控制在 ±50 手内** → 库存中性运行良好
- 🔴 **AO2610 跨日累积 +489 手（高达 521 手峰值）** → 这**不是纯做市**，是带方向观点的库存策略 OR 库存平衡机制在 AO2610 上失效。再加上买卖 50/50 万分位级对称，说明**是有意为之的方向暴露**。
- 这一条**严重警告奶酪 MM**：你已有 `FutuPortfolio` + `max_delta` + `anchor_hedge`，**绝不能让单合约跨日累 500 手而不告警**——查一下你的 monitor 在 AO 这种持续单边情况下是否会触发对冲。

### 2.5 M5: 成交时点 — **典型 adverse selection**

| 指标 | 全局 my_spread | 成交发生时的 my_spread |
|---|---|---|
| 中位数 | 10.0 | **1.0** |
| P25 | 10.0 | **0.0** |
| P75 | 19.5 | 5.0 |

EC 成交时 my_spread 中位数 = 1.0（甚至 P25=0 意味着**价差被压缩到 0**或行情打穿对手价才成交）→ 强烈暗示**被动 adverse selection**：策略在大部分时间挂宽价差虚单（不期望成交），只有当行情打过来时被吃。这是**典型的"消息驱动型亏损模式"** — 每次成交大概率是被信息更优的对手单吃掉。

连续同向成交 run 长度分布：
- ec2607：≥3 笔同向 run 占 39%（80 个 run 中 31 个）
- ec2608：≥3 笔同向 run 占 25%（208 个 run 中 52 个），最长 6 笔
- → **频繁出现 3-6 笔连续同向被吃**，每次都是 adverse selection 的具体证据

### 2.6 M6: 分钟级密度

- AO 22:00 段单 5min 峰值 **2,026 单** → 峰值 ~7 单/秒，但中位数仅 2-3 单/秒
- EC 单 5min 峰值 1,446 单 → 峰值 ~5 单/秒
- 日均活跃 5min 占比：EC 15.5%，AO 32.3%
- 整小时报单 std/mean ≈ 0.2-0.4 → **节奏非常稳定**，没有"突发暴量"模式

---

## 3. 反推策略主逻辑（综合 M1-M6）

基于以上 6 个指纹，外部 EC/AO 做市策略**最可能的实现**：

```
每 2-3 秒定时器触发：
  for 每个目标合约（EC 三连月 / AO 四连月）:
    1. 撤掉本合约所有未成交单
    2. 读取盘口（或仅用最新 tick mid）
    3. 计算 mid = (best_bid + best_ask) / 2 或 last_price
    4. 报 1 档单边/双边：
       bid_price = mid − spread/2   （EC: spread ≈ 10  / AO: spread ≈ 8）
       ask_price = mid + spread/2
       bid_qty  = base_qty           （EC: ~3 手 / AO: ~14 手）
       ask_qty  = base_qty
    5. 通过双边互锁保证 50/50 严格对称
    6. AO 夜盘照跑，EC 跳过夜盘
  无信号融合、无 alpha、无毒性检测、无价差自适应
  仅在收盘前 N 分钟（推测 T-5 到 T-0）停报 + 不平仓（AO2610 累 +500 手就是证据）
```

**核心特征**：
- ⚠️ **没有任何价差自适应**（10/8 ticks 全程恒定，不随波动率/盘口宽度变化）
- ⚠️ **没有信号/alpha**（M5 显示典型 adverse selection）
- ⚠️ **没有平仓机制**（AO2610 跨日累 500 手）
- ✅ **库存平衡靠"50/50 双边报"被动对称**（不是 skew 调节）
- ✅ **多月份覆盖**（跨期价差可能是真正利润来源）
- ✅ **简单可靠**，无复杂度故障点

---

## 4. 对比矩阵：外部策略 vs WtFutuCore MM

| 维度 | 外部策略 (EC/AO) | WtFutuCore MM | 优劣判定 |
|---|---|---|---|
| **报价档数** | 1 档为主（>90%） | num_levels 可配，默认 1 | 等价 |
| **报价心跳** | 定时器 2-3s | on_tick 事件驱动 | **WtFutuCore 胜**（反应快 100×） |
| **价差自适应** | 全程恒定 8-10 ticks | GLFT 模型 + SignalAggregator 动态 | **WtFutuCore 大胜** |
| **Alpha 信号** | 无 | 6 源 SignalAggregator (OFI/TradeFlow/BookImbalance/Momentum/LeadLag/Volatility) | **WtFutuCore 完胜** |
| **毒性检测** | 无 | ToxicFlowDetector (VPIN+OFI+Alpha+自成交校准) | **WtFutuCore 完胜** |
| **库存控制** | 双边 50/50 被动对称 | FutuPortfolio + delta skew（双维 portfolio+contract） | **WtFutuCore 胜**（动态 vs 静态） |
| **跨合约联动** | EC 三连月同步报价 + 跨月价差稳定 | LeadLag 信号 + CorrelationManager（但报价独立） | **平手**（不同设计） |
| **跨期套利** | 无（仅做市） | SpreadArbitrageManager（4 种策略） | **WtFutuCore 完胜** |
| **多档报价网格** | 单档为主 | 支持多档（num_levels + level_step + qty_decay） | **WtFutuCore 胜** |
| **5 级风控响应** | 无 | FutuRiskMonitor (NORMAL→WARN→ELEVATED→HIGH→CRITICAL) | **WtFutuCore 完胜** |
| **自成交防护** | 无（依靠双边互锁） | SelfTradePrevention | **WtFutuCore 胜** |
| **收盘前平仓** | 无（AO2610 累 500 手过夜） | T-15 停报 / T-10 限价对冲 / T-3 市价 | **WtFutuCore 完胜** |
| **下单路由** | 单路径 | 双路径（做市零延迟 vs 套利/对冲限速） | **WtFutuCore 胜** |
| **统一订单跟踪** | 未知 | UnifiedOrderTracker 单一真相源 | WtFutuCore 显式 |
| **跨日仓位过夜** | hedge_only（保留 leg） | hedge_only（V3 已对齐） | **平手** |
| **参数热更新** | 未知 | hotparams.yaml + on_params_updated() | WtFutuCore 显式 |
| **延迟/吞吐监控** | 无 | PerformanceMonitor (<500ns/order) | WtFutuCore 显式 |
| **代码复杂度** | 极简（推测 500 行） | 46 .h + 31 .cpp（~万行） | **外部胜**（运维简单） |
| **实战 fill rate** | 0.19% (EC) / 0.98% (AO) | 未知（无实盘对照） | 待验证 |

### 4.1 外部策略做对的 3 件事 — 奶酪可以借鉴

1. **跨月份同步报价** ★★★★★
   - EC 三连月、AO 四连月同时报，吃跨月价差稳定性（M3 跨月价差日内 std 仅 16-19 价格点）
   - 你的 `LeadLag` 信号已经准备了跨合约数据流，但 **FutuQuoter 是合约独立的**，没有"主合约报价驱动副合约同步报价"的联动逻辑
   - **建议**：在 `StrategyCoordinator.processQuoting()` 增加 `multi_month_sync` 模式，主合约 tick 触发副合约同时刷价，配合 LeadLag 信号

2. **极致严格的 50/50 双边互锁** ★★★★
   - 万分位级对称（你的 skew 是 σ-level，外部是显式 hard binding）
   - 这种"任何 buy 必有对称 sell"的强约束在**极端行情下能防止 delta 暴走**
   - **建议**：在 `FutuPortfolio` 增加一个 "obligation_pairing" 模式选项，可作为 skew 之外的 fallback hard guard

3. **定时器固定心跳的工程稳定性** ★★★
   - 2-3s 心跳意味着即使 tick 丢失/卡顿，报价依然按节奏更新
   - 你的 on_tick 驱动如果 tick parser 卡 5 秒，整个 quote 就 stuck 5 秒
   - **建议**：增加 `quote_heartbeat_watchdog` —— on_tick 距离上次 refresh > N 秒时强制触发一次 refreshQuotes()

### 4.2 外部策略明显做错的 3 件事 — 奶酪必须规避

1. **AO2610 跨日累 +500 手没对冲** 🔴 危险
   - 一个所谓"做市"策略居然让单合约累积 521 手单边持仓
   - 这要么是 hedge 模块失效，要么是策略主动方向暴露
   - **奶酪自检**：检查 `FutuPortfolio.computeHedge()` 在持续单边吃单（连续 5+ 笔同向成交）后能否触发；`anchor_hedge.trigger_portfolio_delta_util=0.8` 这个阈值在"单合约 500 手但 portfolio_delta 因为多空抵消可能仅 100 手"的场景下会**永远不触发**

2. **价差恒定不自适应** 🟡 经济性差
   - EC 10 ticks 价差全程不变，但 ec 在 06-01 价差从 3824→3895 波动 70 点（约 2%），波动率明显变化
   - 单 5min 峰值密度差 5×，但价差不动 → 高波动时 10 ticks 不够宽（吃 adverse selection），低波动时 10 ticks 太宽（不成交）
   - **奶酪自检**：你的 `SpreadOptimizer (GLFT)` + `ToxicityDetector` 是否真的在 EC 类似品种上动态调价？需要 verify_post_fix 跑一遍

3. **EC 经济性可能为负** 🟡 数据警告
   - 7 天 706 手 / 74050 元手续费 → 每手 105 元手续费
   - 假设捕获均价差 = 1 tick = 25 元/手 → 单边毛收益 25 元 ≪ 手续费 105 元
   - 即使乘以双边 +50 元，仍是负毛利
   - 必须靠：(a) 跨月价差捕获显著 > 1 tick （b) 平台返佣 （c) 非做市信息流
   - **奶酪自检**：你的 PerformanceAnalyzer 是否在每合约维度跟踪 `net_pnl_after_commission`？做市策略上线第一周必须按合约盯这个指标，否则会重蹈 EC 的覆辙

### 4.3 双方都有的盲区

| 盲区 | 外部策略 | WtFutuCore | 备注 |
|---|---|---|---|
| 主动吃 liquidity | 几乎无（穿越率 0.01%） | 取决于 SpreadArbitrage 配置 | MM 本质决定 |
| 真双边 stra_quote | 未知（数据无法判定） | `useBilateralQuote=false` 永久关 | 二者实际等价路径 |
| 毫秒级撤单速度 | 数据精度限制无法测 | 自己代码可测但无外部基准 | |

---

## 5. 优化建议（按 ROI 排序，给奶酪当前 WtFutuCore MM）

### P0（必须立即做）

**O1. AO2610 风险案例的 portfolio_delta 计算审计** 🔴
- 检查点：`FutuPortfolio::computePortfolioDelta()` 在多合约同向暴露时是否会被多空抵消导致漏报
- 验证方法：构造测试场景 — AO2610 多 500 手 + AO2608 空 100 手 + EC2608 空 50 手，看 portfolio_delta 是否仍 > anchor_hedge.trigger_portfolio_delta_util
- 如果是按 ¥-delta 加总而非各品种独立，**修正为 per-product max delta**

**O2. 每合约 net_pnl_after_commission 实时监控** 🔴
- PerformanceAnalyzer 必须按 (合约, 日) 输出 `gross_pnl` / `commission` / `net_pnl`
- 第一周必须每天 review；如某合约连续 2 天 net_pnl < 0，自动 widen spread 或暂停该合约

### P1（强烈建议 1 周内做）

**O3. quote heartbeat watchdog** ★★★
- 在 `StrategyCoordinator.processTick()` 增加：若 `now - last_refresh_time > heartbeat_ms`（默认 3000），强制触发 refreshQuotes()
- 防御 tick 流卡顿导致 quote stuck
- 实现成本：~30 行代码

**O4. 跨月份同步报价模式** ★★★★
- 在 FutuMmConfig 增加 `multi_month_sync_group`（如 `["ec2607","ec2608","ec2609"]`）
- 当 anchor 合约（如 ec2608）tick 到达时，遍历同组其他合约同步 refreshQuotes
- LeadLag 信号已就位，只需把它转化为副合约 spread/skew 调整即可
- 实现成本：~150 行代码

**O5. 配置驱动的 SpreadOptimizer 工作验证** ★★★
- 写一个单元测试或 verify_post_fix 脚本：模拟 EC 类品种（vol 2%/day, spread 10 ticks），验证 GLFT 输出的 `optimal_spread` 是否在不同波动率下显著不同
- 如果输出几乎恒定，说明 SpreadOptimizer 在低活跃度品种上不生效，需要降低 estimation_window 或换 estimator

### P2（增益型，1 个月内做）

**O6. 双边 50/50 hard pairing 兜底模式** ★★
- 在 FutuPortfolio 增加 `force_50_50_pairing` 开关
- 模式为 ON 时，任何 buy fill 必须在 X 秒内有等量 sell fill，否则触发减仓
- 作为 skew 失效时的**最后一道防线**

**O7. AO 类高活跃品种夜盘成本-收益重审** ★
- 你的 closeout.night_close_time 已经支持夜盘
- 但 EC 主动放弃夜盘说明夜盘 ROI 不一定正
- 建议：在 PerformanceAnalyzer 按 session（day/night）独立跟踪 PnL，发现夜盘负 PnL 立即关闭

**O8. EC 跨月价差捕获策略** ★
- 既然 EC 跨月价差日内 std 仅 ~2%，是绝佳均值回归场景
- 你的 SpreadArbitrageManager 已有 MeanReversion 策略
- 可以专门为 EC/AO 这类有"远月价差稳定 + 单合约做市不经济"特征的品种配置该策略

### P3（探索性，季度规划）

**O9. 反向学习外部策略 fill 时点**
- 如果你能拿到 EC 同期 tick 行情，用 M5 那份"成交时 my_spread"数据画一张 "fill spread vs market vol" 散点图
- 反推外部策略的 fill 大概率是哪类微结构事件触发（突发吃单 / mid 走穿 / 你的 best_price 被 cross）
- 用这个 ground truth 校准你自己的 ToxicityDetector

---

## 6. 数据局限与下一步

### 6.1 本次分析的硬约束

| 局限 | 影响 |
|---|---|
| 报单时间秒级精度 | 无法算毫秒级撤单存活 |
| 无原始盘口 tick 对照 | 无法判定主动/被动准确分类 |
| `本地单号` 每条记录唯一 | datatable 是最终态快照，无生命周期中间态 |
| 06-05 成交记录损坏 | 单日 ~150 笔成交不可用，但报单 06-05 完整 |
| 仅 9 天数据 | 不能覆盖月度、季度模式 |

### 6.2 如果要升级到 C' 方案

需要：
1. 拉同期 EC2607/2608/2609 + AO2607/2608/2610/2611 的 tick 数据（建议 KS S3 kline_1m 或本地 dsb）
2. 把 580 笔 EC 成交贴回盘口，判定主动/被动 + 滑点
3. 计算 adverse selection（成交后 N tick 内 mid 走向是否对策略不利）
4. 估算外部策略的真实 PnL

如奶酪需要继续到这一步，重新拉 ec 跨月数据后可启动 Phase 5。

### 6.3 推荐落地动作

按 ROI 排序的最小可行清单：
1. **本周内**：O1（portfolio_delta 审计）+ O2（per合约 net_pnl 监控） — 防止你的策略上线后重蹈 AO2610 覆辙
2. **下周**：O3（heartbeat watchdog）+ O5（SpreadOptimizer 验证）— 工程鲁棒性
3. **本月**：O4（多月同步报价）— 真正吸取外部策略最强的设计点
4. **下月**：O8（跨月价差套利专项）— 把外部策略可能赚的钱拿过来

---

**附**：原始数据汇总输出在 `/tmp/ec_mm_analysis/`（ec_ao_orders.parquet, ec_book.parquet, ao_book.parquet），如需复用可直接读取，无需重跑 800MB CSV。
