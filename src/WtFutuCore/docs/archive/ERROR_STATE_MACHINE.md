# ERROR State Machine — 最终设计

> 2026-06-18 | Phase 2 收尾交付物
>
> 替代历史方案。本文档是 ERROR / RISK_HALTED / TOXICITY / MARKET 子态的最终设计 SSOT。

---

## 一、QuotingPhase 5 子态

| 子态 | 触发 | 暂停期撤单 | 退出条件类型 | 退出 API |
|------|------|-----------|------------|---------|
| NORMAL | 默认/全恢复 | - | - | - |
| TOXICITY | tox.is_toxic | ✗ (allow_bid/ask=false) | 时间型(冷却) | tryResumeFrom(TOXICITY) @ Coord:778 |
| MARKET | sig_ctx.shouldPause() | ✓ 单合约 | 信号型(shouldPause=false) | tryResumeFrom(MARKET) @ Coord:570 |
| ERROR | on_entrust 失败 | 软不撤 / 硬撤全部 | 时间退避/on_entrust 成功 | tryResumeFrom(ERROR) @ handleAutoResume + on_entrust:2507 |
| RISK_HALTED | 持仓/Delta超限 / 系统故障 | ✓ 全部 + HEDGE 单 | 状态型(canRecover) | resumeFromRisk() @ R2/R4/R6/R7 |

---

## 二、转移合法性(canTransitionQuoting)

```
       │ to: N │ T │ M │ E │ H
  -----│-------│---│---│---│---
  from N│  ✓    │ ✓ │ ✓ │ ✓ │ ✓
       T│  ✓    │ ✓ │ ✓ │ ✓ │ ✓
       M│  ✓    │ ✓ │ ✓ │ ✓ │ ✓
       E│  ✓    │ ✓ │ ✓ │ ✓ │ ✓
       H│  ✓    │ ✗ │ ✗ │ ✗ │ ✓
```

H 来源只能转 NORMAL。试图 set H→T/M/E 会**静默拒绝**(返回 false)。
所有非 H 的退出必须用 tryResumeFrom(expected) 守卫,防止 high-priority
状态期间被 low-priority 的 else 分支误翻 NORMAL。

---

## 三、ERROR 子态时序图

```
on_entrust(success) ──────────────────────────────────────┐
                                                          │
                                                          ▼
NORMAL ─────[count++ on fail]─────┐                  count=0
                                  │                  tryResumeFrom(ERROR)
                                  ▼
                           count >= threshold(10)?
                          /                       \
                       NO (软触发)              YES (硬触发)
                          │                       │
                          ▼                       ▼
                   ERROR(软)                ERROR(硬)
                   paused_since=now         paused_since=now [BUG-1 修复]
                   ⚠ 不 cancelAll           cancelAll 全部
                   ⚠ 不调 haltTrading        调 haltTrading(REVERSIBLE)
                          │                       │
                          │                       └──> R4 Coord:752
                          │                          (canRecover 通过)
                          │                          → resumeFromRisk
                          │                          但 _order_error_count 仍 >= 10
                          │                          下次失败立刻硬触发
                          │
                          ▼
                handleQuotingAutoResume (每 tick @ on_tick)
                paused_ms > wait_threshold ?
                wait_threshold = 10s << min(count,5),max 60s
                          │
                          ▼ 是
                tryResumeFrom(ERROR) ── 试探恢复 [BUG-2 修复]
                count 不衰减 [Q-B 选 b]
                若再次失败:on_entrust fail → 继续 ERROR

退出 ERROR 的两条路径:
  ① on_entrust(success) → count=0 + tryResumeFrom(ERROR)
  ② handleAutoResume 退避到期 → tryResumeFrom(ERROR) (count 留底)
  ③ R4 Auto-recovery(经 RiskMonitor)→ resumeFromRisk
     (但 count 不重置,在 ERROR 是次要恢复路径)
  ④ session_begin → reset() + count=0 [BUG-7 修复]
```

---

## 四、Phase 2 P1-6/P1-7/ERROR 修复清单

### U1: 统一恢复入口 (P1-6)

新增 TradingState::tryResumeFrom(expected)
  - 仅当 qphase==expected 时翻 NORMAL
  - 防止跨态闪烁 bug(HALT 期间 MARKET/TOXICITY 的 else 分支误翻 N)

迁移 4 处 set(NORMAL) → tryResumeFrom:
  - StrategyCoordinator.cpp:570 MARKET 退出
  - StrategyCoordinator.cpp:778 TOXICITY 退出
  - UftFutuMmStrategy.cpp:1316 handleAutoResume (兼修 BUG-2)
  - UftFutuMmStrategy.cpp:2507 on_entrust 成功

H 退出仍走 resumeFromRisk(语义独立,保留)

### P1-7: 线程契约

TradingState.h 顶部加 THREADING CONTRACT 注释块
8 个写接口加 DEBUG-only `_check_writer_thread()` 校验
Release 零开销,Debug fail-fast

### ERROR 8-BUG 修复(Q-A 方案 A,6/8)

| ID | 修复 | 位置 |
|----|------|------|
| BUG-1 | 硬触发也设 paused_since,handleAutoResume 不再 perma-return | UftFutu:2535 |
| BUG-2 | handleAutoResume 真正能恢复,删 count==0 死代码 | UftFutu:1311-1322 |
| BUG-3 | Q-C 选 c,软触发不 cancelAll(维持语义) | - |
| BUG-6 | order_error_threshold 默认值统一为 10 | UftFutu.h:202 |
| BUG-7 | session_begin 重置 count + paused_since | UftFutu:1171-1175 |
| BUG-8 | H 期间 on_entrust 错误直接 return | UftFutu:2502-2511 |

未修:BUG-4(wait_threshold count≥3 锁 60s,行为可接受),BUG-5(软硬同 ERROR 仅信息层差异,Phase 4 再议)。

---

## 五、回测验证

环境: dist/WtBtFutu/configbt.yaml (ao2607/ao2609 单日 20260605)
结果:
  ✓ P1-7 无 assert 失败(reader 线程串行验证)
  ✓ RISK_HALTED enter/exit 正常(QUOTING_PAUSED ↔ resumed)
  ✓ Closeout 流程不变(DRAINING → ASSESS → EXECUTE → 成交)
  ✓ session_begin/session_end 干净
  ✓ ERROR 路径未触发(error_rate=0,符合预期)
  ✓ 行情温和,TOXICITY/MARKET 未触发

回归:无功能影响。

---

## 六、未做项(下一轮)

  - U2 PauseInfo 结构 + 调度器(预留 Phase 4 milestone)
  - P2-1 initBusinessModules 拆分(540 行,Phase 3 范畴)
  - per-contract qphase 重构(U2 暴露的隐藏问题,Phase 4)
  - ERROR 软/硬分离(BUG-5,引入 ORDER_RETRY + ORDER_HALTED,Phase 4)
