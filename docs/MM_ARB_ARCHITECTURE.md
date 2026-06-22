# 做市+套利业务架构设计

> 2026-06-22
>
> 背景: 套利子系统回测持仓暴走 (bisect 定位三层原因)
> 需要重新审视做市+套利的业务架构, 确保持仓统一管理

---

## 一、持仓暴走 bisect 定位结果 (记录)

### 测试条件
- arb=ON, entry_z=2.0 (0 个套利信号), 单进程干净环境
- EC ec2607/08/09 单日 debug 回测

### 三层叠加原因

| 配置 | 持仓最大 | 增量 | 原因 |
|------|---------|------|------|
| arb=OFF (基线) | 10 | - | 正常做市 |
| arb=ON, pushTick 空, processSpreadArbitrage 空 | 344 | +334 | stp_enabled=true 改变 UnifiedOrderTracker 行为 |
| arb=ON, pushTick 同步执行 (processTick+processSignals) | 4803 | +4459 | _arb_manager->onTick + SpinLock 每 tick 执行 |
| arb=ON, 完整 processSpreadArbitrage | 14933+ | +10130 | processPendingOrders + processOrphanLegs |

### 结论
持仓暴走是三层叠加:
1. stp_enabled 副作用 (次要, +334)
2. pushTick 同步执行开销 (主要, +4459)
3. processPendingOrders/processOrphanLegs (最大, +10130)

根因: 套利子系统的执行路径与做市路径耦合不清晰, 持仓管理割裂.

---

## 二、当前架构问题分析

### 2.1 持仓管理割裂

```
当前架构:
  做市持仓: FutuPortfolio 管理
    → stra_buy/sell (净仓 API)
    → FutuQuoter 双边报价
    → skew 控制库存

  套利持仓: SpreadArbitrageManager 管理
    → AsyncArbitrageExecutor 下单
    → ctx->stra_enter_long/short 或 OrderRouter
    → 独立的 SpreadRiskManager

  问题: 两者用同一合约的同一持仓池, 但各自独立管理
    → 做市的 skew 不知道套利的持仓
    → 套利的 risk 不考虑做市的报价
    → 持仓叠加失控
```

### 2.2 套利信号管理问题

```
当前:
  SpreadArbitrageManager::generateSignals 每个 tick 调用
  → generateSignal 检查 cooldown (ms)
  → MeanReversion::generateSignal 产生 OPEN/CLOSE 信号
  → executeSignal 立即下单 (两腿)

  问题:
  1. 信号频繁 (cooldown 单位不一致: ms vs μs)
  2. 两腿原子提交不保证 (一腿成交另一腿可能未成交)
  3. 平仓信号依赖 spread_position (谁来更新?)
  4. orphan leg 处理增加复杂度
```

---

## 三、两种架构方案对比

### 方案 A: 套利通过 target_pos 驱动做市报价偏移

```
架构:
  SpreadArbitrageManager 计算信号
    → 不直接下单
    → 转化为 target_pos 偏移:
      OPEN_LONG_SPREAD → leg1 target_pos += qty, leg2 target_pos -= qty
      CLOSE_LONG_SPREAD → leg1 target_pos -= qty, leg2 target_pos += qty
    → target_pos 写入 FutuPortfolio
    → FutuQuoter 通过 portfolio 的 target_pos 偏移做 skew
    → 做市报价自然引导到 target_pos
    → 成交后 portfolio 自动更新

  优点:
  ✓ 持仓统一管理 (只有做市一条下单路径)
  ✓ 无两腿原子问题 (做市自然成交到 target)
  ✓ 无 orphan leg
  ✓ 无 stp 冲突 (套利不下单, 不触发自成交)
  ✓ 无 AsyncArbitrageExecutor (消除 pushTick 暴走源)

  缺点:
  ✗ 建仓速度慢 (依赖做市成交, 可能需要几分钟)
  ✗ 价差套利需要快速建仓, 做市偏移可能不够快
  ✗ target_pos 的 skew 偏移量需要精确计算

  适用: 慢速价差回归 (小时级), 不适合快速套利
```

### 方案 B: 套利独立下单但持仓统一管理 + 做市辅助平仓

```
架构:
  SpreadArbitrageManager 计算信号
    → AsyncArbitrageExecutor 直接下单 (两腿)
    → 但: 成交后持仓写入 FutuPortfolio (统一 SSOT)
    → 做市的 skew 感知套利持仓, 自动调整报价

  平仓策略 (三选一):
  B1. 套利信号反向平仓
    → z-score 回归到 exit_threshold 时产生 CLOSE 信号
    → 套利执行器下单平仓
    → 风险: 平仓也可能一腿成交另一腿未成交

  B2. 做市报价自动平仓
    → 套利建仓后, 设置做市 skew 偏移引导平仓
    → 做市自然成交到平仓目标
    → 优点: 无两腿原子问题
    → 缺点: 平仓速度依赖市场流动性

  B3. 混合: 套利信号平仓 + 做市辅助
    → 套利信号触发时尝试直接平仓
    → 如果一定时间内未完全平仓, 转为做市辅助平仓
    → 最灵活但最复杂

  优点:
  ✓ 建仓快 (直接下单)
  ✓ 持仓统一 (portfolio SSOT)

  缺点:
  ✗ 仍需 AsyncArbitrageExecutor (暴走源未消除)
  ✗ 两腿原子问题
  ✗ orphan leg 复杂度
  ✗ 需解决 stp 冲突
```

---

## 四、推荐方案: A+B 混合 (分速建仓 + 做市平仓)

### 4.1 核心设计

```
分两层:

层1 (建仓): 套利信号 → target_pos 偏移 → 做市报价引导
  - 慢速建仓, 不直接下单
  - 通过做市报价的 skew 自然引导到目标持仓
  - 无两腿原子问题, 无 orphan leg

层2 (紧急建仓/平仓): 套利信号 → 直接下单 (仅极端 z-score)
  - z-score > 3.0 (极端偏离) 时才直接下单
  - 正常波动 (1.5 < |z| < 3.0) 走层1
  - 减少 AsyncArbitrageExecutor 的调用频率

平仓: 完全通过做市报价偏移
  - 套利建仓后, 反向设置 target_pos
  - 做市报价自然平仓
  - 不需要套利信号触发平仓
```

### 4.2 信号到 target_pos 的映射

```cpp
// SpreadArbitrageManager 产生信号后, 转化为 target_pos:
void onArbSignal(const SpreadSignal& signal) {
    if (signal.type == SpreadSignalType::OPEN_LONG_SPREAD) {
        // 买 leg1 卖 leg2
        _portfolio->adjustTargetPosition(signal.leg1_code, +signal.suggested_size);
        _portfolio->adjustTargetPosition(signal.leg2_code, -signal.suggested_size);
    } else if (signal.type == SpreadSignalType::OPEN_SHORT_SPREAD) {
        _portfolio->adjustTargetPosition(signal.leg1_code, -signal.suggested_size);
        _portfolio->adjustTargetPosition(signal.leg2_code, +signal.suggested_size);
    }
    // CLOSE 信号: target_pos 回零, 做市自然平仓
    // 不需要显式 CLOSE 信号 — z-score 回归时自动反向调 target
}

// target_pos 驱动做市 skew:
// FutuQuoter 报价时, 检查 current_pos vs target_pos
// 如果 current < target → bid 侧 skew 加大 (鼓励买入)
// 如果 current > target → ask 侧 skew 加大 (鼓励卖出)
```

### 4.3 仓位管理规则

```
1. 套利持仓上限:
   每个 pair: max_spread_position (e.g. 5 手)
   总持仓: max_total_position (e.g. 20 手)

2. 做市 + 套利持仓合并计算:
   FutuPortfolio 的 position = 做市成交 + 套利成交
   skew 基于 total position vs maxPosition

3. 套利 target_pos 调整频率:
   信号 cooldown: 30 秒 (不是毫秒)
   每个 pair 最多 1 个 open position
   z-score 反转到 exit_threshold 时 target_pos 回零

4. 做市报价感知套利 target:
   skew = f(contract_position - contract_target_pos)
   做市报价自动引导到 target
```

### 4.4 信号防频繁

```
当前问题: cooldown 单位 ms vs μs 不一致
修复:
  - cooldown 用 tick 数而非时间 (回测里时间不可靠)
  - signal_cooldown_ticks = 100 (约 30-50 秒)
  - 每个 pair 维护 last_signal_tick
  - 每 tick 只在 last_signal_tick + cooldown_ticks 后才检查

信号状态机 (per pair):
  IDLE → (z > entry_threshold) → OPENING → (做市建仓完成) → HOLDING
  HOLDING → (z < exit_threshold) → CLOSING → (做市平仓完成) → IDLE
  HOLDING → (z > stop_loss) → CLOSING → IDLE (止损)

  OPENING/CLOSING 状态不产生新信号 (防止频繁报单)
```

### 4.5 与当前做市系统的集成

```
改动最小化:

1. SpreadArbitrageManager:
   - 保留信号计算 (z-score, MeanReversion)
   - 去掉 executeSignal (不直接下单)
   - 新增: signal → target_pos 转换

2. FutuPortfolio:
   - 新增 adjustTargetPosition(code, delta)
   - 已有的 skew 逻辑自动感知 target_pos

3. AsyncArbitrageExecutor:
   - 回测里禁用 (不 start, 不 pushTick)
   - 实盘里保留 (仅极端 z-score 时用)

4. UnifiedOrderTracker:
   - stp_enabled 不再由 use_spread_arbitrage 控制
   - 改为独立配置 (stp 默认 false, 需要时手动开)
```

---

## 五、对持仓暴走问题的解决

### 方案 A 直接消除暴走源:
  - 不调 pushTick (没有同步执行开销)
  - 不调 processPendingOrders (没有套利下单)
  - 不调 processOrphanLegs (没有 orphan 处理)
  - stp_enabled 不再由 arb 控制

### 套利通过 target_pos 影响:
  - target_pos 写入 portfolio → skew 调整 → 做市自然建仓
  - 持仓完全由做市的 FutuQuoter/portfolio 管控
  - maxPosition 约束自然生效

---

## 六、实施路径

### 阶段 1: 回测验证 target_pos 方案 (0.5 天)
  - SpreadArbitrageManager 信号 → target_pos
  - FutuPortfolio adjustTargetPosition
  - 回测验证持仓不暴走 + 套利建仓效果

### 阶段 2: 信号状态机 + 防频繁 (0.5 天)
  - per-pair 状态机 (IDLE/OPENING/HOLDING/CLOSING)
  - cooldown 用 tick 数
  - 平仓通过 target_pos 回零

### 阶段 3: 实盘兼容 (0.5 天)
  - AsyncArbitrageExecutor 保留 (极端 z-score 直下)
  - stp 改为独立配置
  - 实盘部署配置

---

## 七、风险与缓解

| 风险 | 缓解 |
|------|------|
| 建仓慢 (做市依赖) | 极端 z-score 时启用直接下单 |
| target_pos skew 不够强 | 调大 inventory_skew_scale |
| 做市在套利方向被吃太多 | max_spread_position 限制 |
| 信号计算开销 | 每 N tick 才算一次 (不是每 tick) |
