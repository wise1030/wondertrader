# Phase 4 业务增强 — 诊断与方案

> 2026-06-18 | Phase 2 收尾后进入 Phase 4
>
> 范围: ROADMAP V2 Phase 4 两项 — P1-8 跨期同步报价组, P2-4 SpreadOptimizer 调参
>
> 状态: **诊断+方案+待拍板**(不动代码)

---

## 一、P1-8 跨期同步报价组 — 诊断

### 1.1 当前架构事实

**报价驱动是 per-contract 串行**:

```
on_tick(ctx, stdCode, tick)                    ← WT 引擎按合约逐个回调
  └─ handleCoordinatorTick(ctx, stdCode, tick)
       └─ processTick(ctx, stdCode, tick)
            └─ Stage 5 processQuoting(ctx, tc, tick)
                 └─ _quoters->find(tc.code)     ← 只取当前 tick 合约的 quoter
                      └─ refreshQuotes(...)      ← 只刷新这一个合约
```

证据:
- UftFutuMmStrategy.cpp:1516 on_tick 签名 per-contract
- StrategyCoordinator.cpp:803 `_quoters->find(tc.code)` 单合约
- StrategyCoordinator.cpp:975 `it->second->refreshQuotes(...)` 单合约刷新

### 1.2 问题

跨期合约(如 ec2607/ec2608/ec2609)价格高度相关,但 tick 异步到达:
- t0: ec2607 tick 到 → 刷新 ec2607 报价(基于 t0 的 ec2607 mid)
- t1: ec2609 tick 到 → 刷新 ec2609 报价(基于 t1 的 ec2609 mid)
- **t0→t1 之间 ec2609 的报价是 stale 的**(基于更早的价格)

当 ec2607 价格跳动时,ec2609 报价还没跟上 → **跨期价差关系被打破** → 套利者/方向性流可以吃掉那条没更新的腿 → adverse selection。

这是做市跨期组合的经典问题:**非同步报价 = 给对手一条免费的统计套利腿**。

### 1.3 现有的部分缓解(已存在)

- LeadLag 跨合约推送(on_tick:1533 handleLeadLagPush)— 把 A 合约的价格变化推给 B 合约的信号源,但**只更新信号,不刷新报价**
- SpreadArbitrage(on_tick:1539)— 是套利模块,不是做市同步

所以现状是:信号层有跨合约联动,但**报价刷新仍是各刷各的**。

---

## 二、P1-8 候选方案

### 方案 A — sync_group 同步刷新(ROADMAP 原设计)

**思路**: 配置 sync_group,组内任一合约 tick 到达时,刷新**全组**报价。

```yaml
quoting:
  sync_group: "ec_main"   # ec2607/ec2608/ec2609 同组
```

```cpp
// processQuoting 改造:不只刷 tc.code,刷整组
if (in_sync_group(tc.code)) {
    for (auto& code : sync_group_members(tc.code)) {
        refreshQuotesForContract(ctx, code, ...);  // 用各自最新 mid
    }
} else {
    // 原单合约逻辑
}
```

**关键细节**:
- 组内其它合约刷新时,用**它们各自缓存的最新 mid**(_last_mid[code]),不是触发合约的 mid
- 触发频率:组内总 tick 数 = 各合约 tick 之和,刷新次数 ×N(N=组大小)
  → 需要节流,否则报价单量暴增

**工作量**: 1.5 天(含节流设计)
**风险**: 中
  - 刷新频率 ×N,撤单/报单量上升,可能触发交易所流控
  - 组内合约的 mid 时效不同(B 合约 mid 可能是 100ms 前的),同步刷新用 stale mid 反而可能更差
**解锁**: 跨期报价时序一致

### 方案 B — 触发节流 + 仅价差显著时同步

**思路**: 不是每个 tick 都全组刷,而是当**组内价差关系变化超阈值**时才触发全组同步刷新。

```cpp
// 当 ec2607 mid 变化导致 spread(2607,2609) 偏离上次同步刷新时的值 > threshold
if (sync_spread_drift(group) > drift_threshold) {
    refresh_all_in_group(ctx, group);
    record_sync_snapshot(group);
}
```

**工作量**: 2 天
**风险**: 中(阈值需要回测调，drift 计算要正确)
**解锁**: 同步刷新 + 频率可控(只在真正需要时同步)

### 方案 C — 暂不做 P1-8,先做 P2-4(调参)

**理由**:
- P1-8 是真架构变更,涉及报价频率/流控/mid 时效，需要谨慎
- per-contract qphase 问题(U2 审计时发现)和 sync_group 是同一层面的"全局 vs 合约级"问题,应该一起设计
- P2-4 是纯回测调参,低风险,能立刻产出 adverse selection 改善数据

**工作量**: 0.5 天(P2-4)
**解锁**: 拿到 toxicity_spread_factor 调参曲线,为后续 P1-8 提供 adverse 基线

---

## 三、P2-4 SpreadOptimizer 调参 — 诊断

### 3.1 现状

SpreadOptimizer.cpp:44:
```cpp
double tox_mult = 1.0 + ctx.toxicity.toxicity_score * _params.toxicity_spread_factor;
```

- `toxicity_spread_factor` 默认 1.0(SpreadOptimizer.h:74)
- 可通过 yaml `toxicitySpreadFactor` 配置(SpreadOptimizer.h:100)
- 可热更新(UftFutuMmStrategy.cpp:2591 HP_TOXICITY_SPREAD_FACTOR)

### 3.2 P2-4 任务

ROADMAP 定义:
1. 回测不同参数组合(1.2/1.5/2.0)对比 adverse selection 指标
2. 确认 ec 成交时 my_spread 中位数(当前 1.0)在调参后是否改善

这是**纯回测调参**,不改架构:
- 跑 toxicity_spread_factor ∈ {1.0, 1.2, 1.5, 2.0} 四组回测
- 比对 [PERF] 日志里的 adverse={:.4f} 指标(UftFutuMmStrategy.cpp:1259)
- 看 BILATERAL_STATS 的 avg_spread 变化

### 3.3 P2-4 的依赖

需要一个稳定的回测脚本能批量跑参数 + 抽取 adverse 指标。
之前 cta_spread_arb 有 run_matrix.py(wtpy 侧),但 WtFutuCore 是 C++ 回测(WtBtRunner),
参数在 yaml 里。需要确认有没有现成的 batch 脚本。

---

## 四、推荐路径

  **建议顺序: P2-4 先行 → P1-8 后做**

  理由:
  1. P2-4 低风险纯调参,先拿到 adverse selection 基线数据
  2. P1-8 的收益就是"降低 adverse selection",没有基线无法量化 P1-8 是否有效
  3. P1-8 涉及 per-contract vs 全局的架构问题,和 U2 PauseInfo 重构同源,应一起设计
  4. 符合你"架构变更前先量化分析"的偏好 —— P2-4 产出的就是量化基线

---

## 五、待奶酪拍板的问题

### Q-P1: Phase 4 先做哪项?
- **(a)** P2-4 调参先行(拿 adverse 基线)→ 再 P1-8(我倾向)
- (b) P1-8 先做(直接上同步报价)
- (c) 两项并行

### Q-P2: 若先做 P2-4,回测批量怎么跑?
- (a) 手动改 yaml 跑 4 次,手动抽 adverse 指标
- (b) 写个 batch 脚本(bash 循环改 toxicitySpreadFactor + 跑 WtBtRunner + grep PERF)
- 我倾向 (b),可复用

### Q-P3: P2-4 用哪个回测标的?
- (a) ao 单日(configbt.yaml,温和行情,adverse 可能不明显)
- (b) ec 多日(_ec_5d.yaml,跨期 + 有 toxicity 场景,更能体现调参效果)
- 我倾向 (b),ec 才是 toxicity_spread_factor 真正起作用的场景

### Q-P4: 若做 P1-8,选方案 A/B/C?
- (a) 方案 A 全组同步刷新(简单但频率 ×N)
- (b) 方案 B 价差漂移触发(频率可控,但要调阈值)
- (c) 先不定,等 P2-4 基线出来再选
- 我倾向 (c)

### Q-P5: P1-8 是否和 U2/per-contract qphase 一起设计?
- (a) 一起(都是"全局 vs 合约级"问题,统一架构)
- (b) 分开(P1-8 只管报价同步,不碰状态机)
- 我倾向 (a),避免两次改同一层

---

## 六、备注

- Phase 4 与前三个 Phase 不同:P1-8 是真新功能 + 架构变更,不是修 bug
- per-contract qphase 问题(U2 审计发现)在 P1-8 会再次浮现:sync_group 的报价同步
  和 qphase 的全局/合约级是同一个架构决策
- 建议把 Phase 4 当成一个"做市跨期组合"的完整 milestone,而不是零散两项
