# 方案 B 优化设计 — 套利独立下单 + 持仓统一管理

> 2026-06-22 | 基于代码审计 + bisect 定位
>
> 核心原则: 保留套利独立下单, 但通过持仓统一 + 信号状态机 + 精确修复暴走源

---

## 一、发现的代码根因 (审计新发现)

### 1.1 套利持仓状态从未更新

SpreadArbitrageManager::updatePosition (L546) 定义了 leg1/leg2 持仓更新,
但**从未被调用**。SpreadState.spread_position 永远是初始值 0。

后果:
- hasPosition() 永远 false
- 每个信号都被当成 OPEN (新开仓)
- CLOSE 信号永远不触发 (没有持仓可平)
- 套利不断开新仓 → 持仓无限累积

### 1.2 套利持仓与做市持仓完全割裂

做市持仓: FutuPortfolio.position (由 stra_get_local_position 更新)
套利持仓: SpreadState.spread_position (从未更新, 恒为 0)

两者用同一合约但各自独立管理:
- 做市的 skew 不知道套利下了多少单
- 套利的 risk 不知道做市的持仓
- 总持仓 = 做市持仓 + 套利持仓, 但没人计算总持仓

### 1.3 信号无状态机

当前 generateSignal 每个 tick 检查 z-score:
- z > entry → OPEN_LONG (每 tick 都产生)
- cooldown 单位不一致 (ms vs μs)
- 没有 OPENING/HOLDING/CLOSING 状态
- 同一个 pair 可以反复开仓

---

## 二、优化方案 (五层修复)

### 2.1 持仓统一 (最关键)

```
方案: 套利成交后, 持仓写入 FutuPortfolio (统一 SSOT)

UftFutuMmStrategy::on_trade:
  → 已有 _portfolio->updatePosition(code, local_net) 更新做市持仓
  → 新增: 如果是套利订单, 调 _spread_arb_manager->updatePosition(pair_id, ...)
  → SpreadArbitrageManager 从 FutuPortfolio 读取 leg 持仓:

    double leg1_pos = _portfolio->getPosition(config.leg1_code);
    double leg2_pos = _portfolio->getPosition(config.leg2_code);
    state.leg1_position = leg1_pos;
    state.leg2_position = leg2_pos;
    state.spread_position = min(|leg1_pos|/ratio1, |leg2_pos|/ratio2) * sign;

关键: 套利的持仓就是 portfolio 的持仓, 不维护独立副本
```

### 2.2 信号状态机 (防频繁)

```
每个 pair 维护状态:
  enum class PairState { IDLE, OPENING, HOLDING, CLOSING };

  IDLE:
    z > entry_z → OPEN_LONG → 状态转 OPENING
    z < -entry_z → OPEN_SHORT → 状态转 OPENING

  OPENING:
    检查持仓: 如果 spread_position 达到目标 → 转 HOLDING
    不产生新信号

  HOLDING:
    z < exit_z → 转 CLOSING (做市平仓 or 套利平仓)
    z > stop_loss → 转 CLOSING (止损)
    不产生新开仓信号

  CLOSING:
    检查持仓: 如果 spread_position 回到 0 → 转 IDLE
    不产生新信号

cooldown: tick 计数 (不是时间)
  每 N tick 才检查一次信号 (e.g. 30 tick ≈ 10-15 秒)
```

### 2.3 暴走源修复

```
三层修复 (对应 bisect 结果):

层1 (stp_enabled): 改为独立配置
  tracker_cfg.stp_enabled = _config.order_control.use_stp;  // 独立配置
  // 不再由 use_spread_arbitrage 自动开启

层2 (pushTick 同步): 精简执行
  processTick: 每 tick 执行 (SpreadCalculator 更新, 轻量)
  processSignals: 改为每 N tick 执行一次 (不是每 tick)
    → 减少 generateSignals 的调用频率
    → _pair_states_spin 占用减少

层3 (processPendingOrders): 持仓上限校验
  executeSignal 前检查:
    if (portfolio_position + order.qty > max_position) → 拒绝
  processOrphanLegs: 加持仓上限校验
```

### 2.4 两腿下单简化

```
当前: 两腿原子提交 (req_id fetch_add(2))
问题: 一腿成交另一腿未成交 → orphan

优化: 顺序提交 + 持仓校验
  1. 先提交主动腿 (流动性较差的腿)
  2. 等待成交确认
  3. 再提交被动腿 (流动性好的腿)
  4. 如果主动腿成交但被动腿未成交:
     → 不立即 hedge (不产生 orphan)
     → 通过做市报价偏移自然消化 (做市自然在被动腿方向成交)
     → 设置持仓超时检查: N tick 后仍未对冲, 做市 skew 加大

优点: 简化原子提交的复杂性, 利用做市自然对冲
```

### 2.5 平仓管理

```
平仓信号来源:
  1. z-score 回归: |z| < exit_z → CLOSE 信号
  2. 止损: |z| > stop_loss_z → CLOSE 信号
  3. 超时: 持仓时间 > convergence_timeout → CLOSE 信号
  4. 收盘: closeout_executor 统一平仓

平仓执行:
  正常 (z-score 回归): 套利直接下单平仓 (两腿反向)
  异常 (超时/止损/收盘): 通过做市报价偏移引导平仓
    → 设置 target_pos 偏移
    → 做市 skew 自动引导到平仓
    → 不需要套利下单

平仓状态管理:
  CLOSING 状态不产生新开仓信号
  spread_position 回到 0 时自动转 IDLE
```

---

## 三、多模型支持

当前已支持 4 种策略 (per pair 独立配置):

| 策略 | 适用场景 | 建仓信号 | 平仓信号 |
|------|---------|---------|---------|
| MeanReversion | 价差均值回归 | |z| > entry_z | |z| < exit_z |
| PairsTrading | 协整配对 | cointegration p-value | |z| < exit_z |
| TrendFollowing | 价差趋势 | MA 交叉 | 趋势反转/止损 |
| StatisticalArb | 统计套利 | M-spread + volume | M-spread 回归 |

多模型管理:
  - 每个 pair 配置一个 primary_strategy
  - 不同 pair 可以用不同策略
  - 状态机统一 (IDLE/OPENING/HOLDING/CLOSING)
  - 持仓管理统一 (通过 FutuPortfolio)

---

## 四、架构改造图

```
改造前 (割裂):
  做市路径: Coordinator → FutuQuoter → stra_buy/sell
  套利路径: SpreadArbMgr → AsyncArbExec → ctx->stra_enter_long/short
  持仓管理: 做市用 Portfolio, 套利用 SpreadState (未更新)

改造后 (统一):
  做市路径: Coordinator → FutuQuoter → stra_buy/sell
  套利路径: SpreadArbMgr → AsyncArbExec → ctx->stra_enter_long/short
  持仓管理: 统一 FutuPortfolio (做市+套利持仓合并)

  新增:
  1. on_trade → _spread_arb_mgr->syncPositionFromPortfolio()
     → 每个 pair 的 SpreadState 从 Portfolio 读取持仓
  2. SpreadArbMgr 信号状态机 → 防频繁
  3. executeSignal → 持仓上限校验
  4. stp 独立配置
  5. processSignals 每 N tick 执行一次
```

---

## 五、实施计划

### 阶段 1: 持仓统一 + 暴走修复 (1 天)

| 步骤 | 内容 |
|------|------|
| 1.1 | syncPositionFromPortfolio: SpreadState 从 Portfolio 读取 |
| 1.2 | stp_enabled 改为独立配置 |
| 1.3 | processSignals 改为每 N tick 执行 |
| 1.4 | executeSignal 加持仓上限校验 |
| 1.5 | 回测验证: 持仓不暴走 |

### 阶段 2: 信号状态机 (0.5 天)

| 步骤 | 内容 |
|------|------|
| 2.1 | PairState 枚举 + per-pair 状态 |
| 2.2 | generateSignal 按状态过滤 |
| 2.3 | cooldown 改为 tick 计数 |
| 2.4 | 回测验证: 信号不频繁 |

### 阶段 3: 两腿 + 平仓优化 (0.5 天)

| 步骤 | 内容 |
|------|------|
| 3.1 | 顺序提交 + 持仓校验 |
| 3.2 | orphan 腿通过做市偏移消化 |
| 3.3 | 平仓信号 + 做市兜底 |
| 3.4 | 回测验证: 完整套利流程 |

---

## 六、与方案 A 的对比

| 维度 | 方案 A (target_pos) | 方案 B 优化版 |
|------|-------------------|-------------|
| 建仓速度 | 慢 (做市依赖) | 快 (直接下单) |
| 持仓管理 | 天然统一 | 需要同步 |
| 两腿原子 | 无问题 | 顺序+做市兜底 |
| 复杂度 | 低 | 中 |
| 暴走风险 | 低 | 需精确修复 |
| 多模型 | 只需信号→target | 信号→状态机→下单 |
| 实盘延迟 | 高 (做市引导慢) | 低 (直接下单) |

方案 B 优化后: 持仓统一 + 状态机防频繁 + 暴走源修复
→ 保留了快速建仓能力, 同时解决了当前所有问题

---

## 七、风险

| 风险 | 缓解 |
|------|------|
| 持仓同步延迟 | 每 tick 从 portfolio 读取 (不缓存) |
| 两腿不对冲 | 顺序提交 + 做市偏移兜底 |
| 状态机死锁 | 每个状态有超时自动转移 |
| 多 pair 持仓叠加 | max_total_position 全局限额 |
| 回测/实盘差异 | pushTick 同步模式保留, 实盘走异步 |
