# ERROR 状态机审计报告 — on_entrust + handleQuotingAutoResume

> 2026-06-18 | Q1 追问触发的专项审计
>
> 用户原话:"on_entrust 关于下单错误,单次似乎不停止 quote,时间计算是否有问题,
> 重新理清楚不同交易状态以及之间切换的逻辑,给出梳理优化建议"
>
> 状态: **诊断+建议**(不动代码)

---

## 一、ERROR 状态相关代码 SSOT

| 元素 | 位置 | 含义 |
|------|------|------|
| `qphase = ERROR` | TradingState.h:40 | 子状态,处于此态时 isActive/canQuote 都 false |
| `_order_error_count` | UftFutuMmStrategy.h:421 | 连续失败计数 |
| `_quoting_paused_since` | UftFutuMmStrategy.h:423 | ERROR 进入时间戳(ms),0=未暂停 |
| `order_error_threshold` | h:199 + .cpp:240 | 阈值。**默认值不一致**:构造器=3,yaml 缺失读=10 |
| `on_entrust` 写入 | .cpp:2495-2543 | 入口 |
| `handleQuotingAutoResume` | .cpp:1296-1326 | 在 on_tick 里调,试图自动恢复 |

---

## 二、当前 ERROR 状态机流程图(实际行为)

```
                       ┌───────────┐
                       │  NORMAL   │
                       └─────┬─────┘
                             │ on_entrust(fail)
                             │ count++
                             ▼
              ┌──────────────────────────────────┐
              │   count >= threshold (硬) ?       │
              └────────┬───────────────┬─────────┘
                       │ 是             │ 否(软)
                       ▼                ▼
        ┌──────────────────┐   ┌─────────────────────────┐
        │ qphase=ERROR     │   │ qphase=ERROR            │
        │ haltTrading(R)   │   │ paused_since=now()      │
        │ cancelAll        │   │ (无 cancelAll/halt)     │
        │ paused_since=0 ⚠ │   └────────────┬────────────┘
        └────────┬─────────┘                │
                 │                          │
                 ▼                          ▼
         不进 handleQuoting          handleQuotingAutoResume(每 tick)
         AutoResume(line 1298         读 paused_ms, 算 wait_threshold:
         守卫 paused_since=0)            10s << min(count, 5),最大 60s
                 │                       │
                 │                       │ paused_ms > threshold ?
                 │                       │
                 │                       ├─ 是 ┬── count==0 ? → set(NORMAL)
                 │                       │     │       (count 在这里永远 != 0
                 │                       │     │        因为只有 on_entrust
                 │                       │     │        成功才清零)
                 │                       │     │
                 │                       │     └── count>0  → 重置 paused_since=now
                 │                       │                    (无副作用,死循环
                 │                       │                     直到 on_entrust 成功)
                 │                       │
                 │                       └─ 否 → 继续等
                 │
                 │   (硬触发等待 R4 Auto-recovery,见 Coordinator:746)
                 ▼
         R4: !isActive() && _risk_monitor->canRecover()
             → resumeFromRisk() → qphase = NORMAL
             ↑
             ⚠ 但 _order_error_count 仍非零!
             下次 on_entrust 失败仍走硬触发(因 count 已 >= threshold)
                 │
                 │   (软或硬,真正退出 ERROR 的唯一确定路径)
                 ▼
         on_entrust(success):
             count = 0
             qphase = NORMAL (set 直接走,因 ERROR→NORMAL 校验通过)
             paused_since = 0
```

---

## 三、问题清单(按严重度排序)

### 🔴 P0 BUG-1: 硬触发分支不设 _quoting_paused_since,导致 handleAutoResume 永久跳过

**位置**: UftFutuMmStrategy.cpp:2521-2534(硬触发分支)

**现象**:
```cpp
if (count >= threshold) {
    setQuotingPhase(ERROR);
    haltTrading(REVERSIBLE);
    cancelAll;
    // _quoting_paused_since 没设,保持 0
}
```

**后果**:
- handleQuotingAutoResume(line 1298)第一行:`if (qphase != ERROR || paused_since == 0) return;`
- 硬触发后 paused_since=0 → handleAutoResume 直接 return → **永远不走自动恢复路径**
- 唯一退出靠 R4(Coordinator Auto-recovery,经 _risk_monitor->canRecover)和 on_entrust 成功
- R4 路径会把 qphase 翻 NORMAL,但 **_order_error_count 仍然非零**,下次再失败立即走硬触发,陷入"halt-recover-fail-halt"震荡

**严重度**: 高(Coordinator R4 兜底但语义错乱;count 不清零导致后续每次失败都直接硬触发)

### 🔴 P0 BUG-2: handleQuotingAutoResume 的 NORMAL 分支永远不可达

**位置**: UftFutuMmStrategy.cpp:1312-1318

**现象**:
```cpp
if (paused_ms > wait_threshold) {
    if (_order_error_count == 0) {
        setQuotingPhase(NORMAL);
        _quoting_paused_since = 0;
    } else {
        // count > 0,重置 paused_since
    }
}
```

**死锁推理**:
- 进入 ERROR 的前提是 `count++` 后 ≥ 1(硬)或 ≥ 1(软)
- count 只在两处被改:
  1. on_entrust 失败:`count++`(line 2516)
  2. on_entrust 成功:`count = 0`(line 2504)
- handleAutoResume 不动 count
- → ERROR 期间 count **永远 > 0**(除非中间穿插一个 success,但 success 自己已经在 line 2507 把 qphase 翻 NORMAL,根本走不到 handleAutoResume 的恢复分支)
- → line 1316 `setQuotingPhase(NORMAL)` 死代码

**真实行为**: handleAutoResume 在软触发后只做一件事 —— 每 wait_threshold 毫秒打一次 "still paused, extending wait" 日志,刷屏。

**严重度**: 高(声称的"指数退避自动恢复"功能根本不存在)

### 🟡 P1 BUG-3: 软触发不 cancelAll,旧挂单留在交易所

**位置**: UftFutuMmStrategy.cpp:2536-2542

**现象**: 单次失败进 ERROR,但已挂的所有 quote 不撤单,留在交易所

**后果**:
- canQuote=false → 不发新单
- 但已挂订单可能被对手成交 → 在 ERROR 期间产生意外成交
- 而且这次成交会触发 on_entrust(success) → 立即翻 NORMAL,把"暂停"全部抹掉

**严重度**: 中(信息泄漏 + 状态机不稳定)

**用户原话验证**: "单次似乎不停止 quote" ✓ 确实不停止已挂单的可成交性,只是不再发新单。

### 🟡 P1 BUG-4: 时间计算 wait_threshold 算法可疑

**位置**: UftFutuMmStrategy.cpp:1302-1308

**现象**:
```cpp
uint64_t wait_threshold = 10000;  // 10s 基础
if (count > 0) {
    uint32_t shift = (count < 5) ? count : 5;
    uint64_t exp_wait = 10000ULL << shift;  // 10s, 20s, 40s, 80s, 160s, 320s
    wait_threshold = (exp_wait > 60000ULL) ? 60000ULL : exp_wait;
    // 实际: count=1→20s, 2→40s, 3→60s, 4→60s, 5→60s
}
```

**问题**:
- count=0 时 wait_threshold=10s,但前面已证 count==0 在 ERROR 期间不可达,**这条路走不到**
- count≥3 后阈值锁死 60s,后续递增无效(指数退避失效)
- shift 由"当前 count"算,但 count 从未在 handleAutoResume 中 reset → 退避其实只在第一次进入时确定一次,后续重置 paused_since 不动 wait_threshold 含义

**严重度**: 中(指数退避算法部分失效,但总体上"等 60s 然后重置"的循环还能跑)

### 🟡 P1 BUG-5: 软/硬触发都进同一个 ERROR 状态,无法区分

**位置**: UftFutuMmStrategy.cpp:2523 和 2538(目标都是 ERROR)

**问题**:
- 软触发(临时网络抖动,期待自恢复)
- 硬触发(连续失败到阈值,需重置)
- 两者都是 qphase=ERROR
- 恢复路径不同:
  - 软:期待 handleAutoResume(实际 BUG-2 死代码)→ fall back 到 on_entrust 成功
  - 硬:期待 R4 Auto-recovery(经 RiskMonitor canRecover)→ 翻 NORMAL,但 count 不清

**严重度**: 中(语义混乱,运维难以从日志区分两种状态)

### 🟢 P2 BUG-6: order_error_threshold 默认值不一致

**位置**: 
- UftFutuMmStrategy.h:202 构造器 `order_error_threshold(3)`
- UftFutuMmStrategy.cpp:240 yaml 读取默认 `readUInt32(cfg, "orderErrorThreshold", 10)`

**严重度**: 低(yaml 通常都配置,但是定义一致性问题)

### 🟢 P2 BUG-7: _order_error_count 不在 session_begin 重置

**位置**: 全局只有 line 2504(on_entrust 成功)清零

**问题**:
- 跨日:昨日累计错误带到今日
- on_session_begin(line 1160-1209) 重置了 _trading_state.reset() 和 _risk_monitor 各种 daily 状态,但漏了 _order_error_count

**严重度**: 中(若隔夜没有成功成交,count 持续累积)

### 🟢 P2 BUG-8: H 期间 set(E) 被静默拒绝(原 v2 报告 #B1)

**位置**: UftFutuMmStrategy.cpp:2521-2542

H 期间 on_entrust 失败仍会:
- count++(可被污染累计)
- haltTrading 重复调用(2528)
- cancelAll 重复执行(2532-2533)
- set(ERROR) 被吃,qphase 留 H
- 软分支还会写 _quoting_paused_since,但 qphase 是 H,数据脏

---

## 四、状态机重新梳理

### 4.1 当前 5 个 QuotingPhase 子状态语义

| 子态 | 触发 | 退出 | 是否可报价 | cancelAll | 持仓动作 |
|------|------|------|----------|-----------|---------|
| NORMAL | 默认/恢复 | 任意更高优先级抢占 | ✓ | - | - |
| TOXICITY | VPIN/OFI 信号 | 冷却时间到 | ✗ | 不 | - |
| MARKET | vol_tier=EXTREME | shouldPause=false | ✗ | 不 | - |
| ERROR | 下单失败 | on_entrust 成功 / R4 兜底 | ✗ | 软不撤,硬撤 | - |
| RISK_HALTED | 持仓/Delta超限 | resumeFromRisk()(R4等4处) | ✗ | 撤(在 HALT_TRADING 路径) | 强平(IRREVERSIBLE) |

### 4.2 转移合法性矩阵(canTransitionQuoting)

| from\to | NORMAL | TOXICITY | MARKET | ERROR | RISK_HALTED |
|---------|--------|----------|--------|-------|-------------|
| NORMAL | ✓(幂等) | ✓ | ✓ | ✓ | ✓ |
| TOXICITY | ✓ | ✓(幂等) | ✓ | ✓ | ✓ |
| MARKET | ✓ | ✓ | ✓(幂等) | ✓ | ✓ |
| ERROR | ✓ | ✓ | ✓ | ✓(幂等) | ✓ |
| RISK_HALTED | ✓(且唯一通道) | ✗ | ✗ | ✗ | ✓(幂等) |

### 4.3 优先级(隐含)

```
RISK_HALTED  >  ERROR  >  MARKET  >  TOXICITY  >  NORMAL
   (硬)        (硬)      (软)     (软)        (基)
```

所有非 NORMAL 都是 canQuote=false,所以"优先级"在功能上无意义,只是状态机记录。

---

## 五、优化建议

### 方案设计原则

1. **enter/exit 必须配对**:每个 enter 路径都要有明确的 exit 路径,且都更新 SSOT(_trading_state)
2. **指数退避真正生效**:wait_threshold 必须有逐步增加的实质效果
3. **软/硬触发分离**:软=本 tick 暂停 + 等下一笔成功;硬=cancelAll + 计入风控
4. **跨 session 状态干净**:session_begin 必须 reset count/paused_since/qphase

### 优化方案 A — 最小改动(修 BUG 不改架构)

**A1. 硬触发也设 paused_since(修 BUG-1)**

```cpp
// line 2521 段补上
if (count >= threshold) {
    setQuotingPhase(ERROR);
    haltTrading(REVERSIBLE);
    cancelAll;
    _quoting_paused_since = TimeUtils::getLocalTimeNow();  // 新增
}
```

**A2. handleAutoResume 真正能恢复(修 BUG-2)**

```cpp
if (paused_ms > wait_threshold) {
    // 不再要求 count==0,而是给一次"试探性恢复"机会
    // 同时把 count 衰减(不是清零)
    setQuotingPhase(NORMAL);
    _quoting_paused_since = 0;
    _order_error_count = std::max(0u, _order_error_count - 1);  // 衰减
    WTSLogger::info("Auto-resume after {}ms (count decayed to {})", paused_ms, count);
}
```

或者更保守:**count 不衰减,但允许试探性恢复**,试探后若再次失败立刻走硬触发的 cancelAll 路径(因为 count 已经累计)。

**A3. 软触发也 cancelAll(修 BUG-3)**

```cpp
else {
    setQuotingPhase(ERROR);
    paused_since = now;
    if (_main_ctx) {  // 新增
        for (auto& [code, quoter] : _quoters)
            quoter->cancelAll(_main_ctx);
    }
}
```

**A4. session_begin 重置 count + paused_since(修 BUG-7)**

```cpp
// on_session_begin 加
_order_error_count = 0;
_quoting_paused_since = 0;
```

**A5. H 期间 on_entrust 错误直接 return(修 BUG-8,即原 v2 F-B1)**

```cpp
if (!bSuccess && _trading_state.qphase == QuotingPhase::RISK_HALTED) {
    WTSLogger::warn("Order error during RISK_HALTED, ignored");
    return;
}
```

**A6. 阈值默认值统一(修 BUG-6)**

构造器和 yaml 默认都改 5(或都改 10),取一致。

**工期**: 0.5 天
**风险**: 低,都是局部改动

### 优化方案 B — 引入软/硬两态分离

**新增 QuotingPhase**:
```cpp
enum class QuotingPhase : uint8_t {
    NORMAL,
    TOXICITY,
    MARKET,
    ORDER_RETRY,    ///< 单次失败,试探退避(原软触发)
    ORDER_HALTED,   ///< 累计失败到阈值,需 cancelAll(原硬触发)
    RISK_HALTED,
};
```

软触发→ORDER_RETRY,硬触发→ORDER_HALTED

各自的 handleResume 独立:
- ORDER_RETRY:指数退避试探,失败 N 次后升级到 ORDER_HALTED
- ORDER_HALTED:必须 R4 或 session_begin 才能复活

**工期**: 1 天
**风险**: 中(改 enum 涉及 getPhaseStr/canTransition/StrategyCoordinator 多处)

### 优化方案 C — 把 ERROR 路径完全收口给 RiskMonitor

ERROR 不再做独立的子状态,完全等同于 RISK_HALTED:
- 软触发不进任何状态(只 cancelAll 当前合约,记 count)
- 硬触发直接 setQuotingPhase(RISK_HALTED) + haltTrading
- 恢复唯一靠 R4

**工期**: 0.5 天(代码减法)
**风险**: 中(语义改变,需要确认下单错误和风控停在业务上是否真等价)

---

## 六、推荐路径

  **A 方案**(全部 6 个修复)— 不改架构,把现有的 ERROR 状态机修对。
  
  优先级:
  - 必修(P0): A1, A2 — 否则 ERROR 状态有死锁/死代码
  - 应修(P1): A3, A4, A5 — 状态机干净度
  - 可选(P2): A6 — 一致性

---

## 七、待奶酪拍板的问题

### Q-A: 修复方案选哪个?
- **(a)** 方案 A 全部 6 项(最小改动,修 BUG)— 我倾向
- (b) 方案 B 软/硬分离(引入新 enum)
- (c) 方案 C 收口给 RiskMonitor(语义统一)

### Q-B: A2 的"试探性恢复"用哪种?
- **(a)** 退避到期后无条件试探恢复 + count 衰减 1
- (b) 退避到期后无条件试探恢复 + count 不变(下次失败直接硬触发)
- (c) 删掉这分支,只靠 on_entrust success / R4 / session_begin 恢复

  我倾向 (b):试探+保留 count 是最贴合"指数退避"语义的写法,且不会引入"衰减后忘记真实历史"的混淆。

### Q-C: A3 软触发是否 cancelAll?
- **(a)** 软触发只 cancel 失败那个合约的挂单(局部隔离)
- (b) 软触发 cancelAll 所有合约
- (c) 软触发不 cancel(维持现状)

  我倾向 (b):简单可控,且既然进 ERROR 已经 canQuote=false,挂单留着也不会被刷新。但若你觉得局部隔离更精细,选 (a)。

### Q-D: 阈值默认值统一为多少?
- (a) 3(原构造器)
- (b) 5
- **(c)** 10(原 yaml 默认)— 我倾向

  期货下单偶发失败常见,3 太敏感容易误触发硬停。

### Q-E: 是否同时把 v2 报告的 F-B2(信号 MARKET)和 F-D(P1-7 线程契约)也合在本轮?
- **(a)** 合,本轮一起做掉(F-B1=A5, F-B2=新增, F-D 线程注释)
- (b) ERROR 修复先单独一轮,F-B2/F-D 下轮

  我倾向 (a):反正都是状态机层面的清理,一起 commit 比分两批好。

### Q-F: 回归门槛?
- (a) 编译通过即可
- **(b)** ao 1 日回测 + 关键 ERROR/RISK_HALTED 转移日志 diff
- (c) ao + ec 各 1 日

  我倾向 (b)。

### Q-G: 文档?
- **(a)** 落 docs/ERROR_STATE_MACHINE.md 记录最终设计 + ROADMAP V2 标 Phase 2 done

---

## 八、备注

  - 用户观察"单次似乎不停止 quote"是准确的:软触发不 cancelAll,旧单留在交易所
  - 用户观察"时间计算可能有问题"也对了:wait_threshold + count 配合下,handleAutoResume 的 NORMAL 分支死代码,实际靠 on_entrust 成功 / R4 才能恢复
  - 这一轮审计发现 8 个问题,2 个 P0 死锁/死代码,3 个 P1 语义混乱,3 个 P2 一致性
  - 状态机层面 ERROR 子态当前是"形似而神不是" — 看起来有指数退避自恢复,实际上不工作
