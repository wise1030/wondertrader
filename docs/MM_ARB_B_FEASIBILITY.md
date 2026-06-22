# 方案 B 可行性确认 + 对做市模块影响分析

> 2026-06-22 | 代码审计确认

---

## 一、五个关键问题确认

| 问题 | 确认结果 | 详情 |
|------|---------|------|
| 套利下单 API | ✓ stra_buy/sell | OrderRouter 调 ctx->stra_buy/sell, 和做市相同 API |
| 持仓更新 | ✓ 天然统一 | on_trade → portfolio->onPositionUpdate(code, local_net), 自动含套利仓 |
| skew 感知 | ✓ 天然集成 | getPosition = position - target_position, 含做市+套利 |
| 风控拦截 | ✓ 天然生效 | checkRiskLimits(portfolio) 检查总 delta/exposure |
| cancelAll 误撤 | ✓ 隔离正确 | FutuQuoter 只撤自己的 levels, 不碰套利单 |

**结论: 方案 B 在技术上可行, 做市基础设施天然支持套利持仓统一管理.**

---

## 二、对做市模块的影响

### 正面影响 (无需改动)

- **持仓统一**: Portfolio 已是 SSOT, 套利成交后持仓自动合并
- **skew 集成**: 做市 skew 基于 (position - target_position), 天然感知套利仓
- **风控集成**: checkRisk 基于 portfolio getTotalDelta, 天然拦截
- **cancelAll 隔离**: FutuQuoter 只撤自己的报价订单 ID

### 需要修复的问题

| 问题 | 影响 | 修复 |
|------|------|------|
| updatePosition 从未调 | spread_position 恒 0, 无限开仓 | 从 Portfolio 读实际持仓 |
| stp_enabled 由 arb 控制 | 改变 tracker checkSelfTrade 路径 | 改为独立配置 |
| pushTick 每 tick 同步 | 增加 tick 处理开销 | processSignals 降频 |

### 核心矛盾: target_position 语义

**发现**: Portfolio 的 target_position 默认 0, 做市 delta = position - 0 = position.
套利下单让 position 变成 5 → 做市 skew 认为"多了 5 手" → ask 加大 → 自动平仓.

**问题**: 套利建仓后被做市 skew 自动平仓!

**解决 (B-1)**: 套利下单后调整 target_position
```
套利建仓: position += qty, target_position += qty
  → delta = (position+qty) - (target+qty) = 不变
  → 做市报价不受影响
  → 套利持仓完全由套利信号管理

套利平仓: position -= qty, target_position -= qty
  → 做市报价继续不受影响
```

---

## 三、方案 B 修订后的完整架构

```
套利信号 → 状态机检查 (IDLE/OPENING/HOLDING/CLOSING)
  → executeSignal (OrderRouter → stra_buy/sell)
  → 成交后:
    → WT 引擎 local_position 更新
    → on_trade → portfolio->onPositionUpdate (持仓统一)
    → 新增: portfolio->adjustTargetPosition (target 跟随)
    → 新增: spread_arb_mgr->syncPositionFromPortfolio (spread_position 更新)

做市路径不受影响:
  → SpreadOptimizer 用 delta = position - target_position
  → delta 因 target 跟随而不变
  → skew 不偏移 → 不自动平仓套利仓
  → 正常双边报价
```

---

## 四、方案 B 可行性最终判定

**可行** — 做市基础设施天然支持, 核心矛盾 (target_position) 有清晰解决方案.
