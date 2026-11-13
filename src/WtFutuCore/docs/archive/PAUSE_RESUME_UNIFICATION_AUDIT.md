# 暂停/恢复路径统一化审计报告

> 2026-06-18 | 用户追问:"除了 ERROR,其他几个状态也存在停止报价的情形,也存在
> 自动恢复的问题,是否可以统一恢复报价入口"
>
> 状态: **诊断+方案+待拍板**(不动代码)

---

## 一、5 个 QuotingPhase 子态的暂停/恢复全景

| 子态 | 触发位置 | 触发条件 | 暂停时撤单 | 恢复条件类型 | 恢复入口 |
|------|---------|--------|-----------|-----------|---------|
| NORMAL | - | 默认 | - | - | - |
| TOXICITY | Coord:773 | tox.is_toxic(本质)+ 冷却时间(状态机记录) | ✗ 不撤(allow_bid/ask=false) | 时间型 | Coord:778 同函数 else |
| MARKET | Coord:553 | shouldPause()(信号) | ✓ 单合约 cancelAll(573-578) | 信号型 | Coord:570 同函数 else |
| ERROR-软 | Strat:2538 | count<threshold | ✗ 不撤(BUG-3) | 时间型(BUG-2 死代码)/ on_entrust 成功 | handleAutoResume / on_entrust:2507 |
| ERROR-硬 | Strat:2523 | count≥threshold | ✓ 全部 cancelAll | 时间型(BUG-1 不设)+ 风控型 | R4 / on_entrust:2507 |
| RISK_HALTED | Coord:640+672+718 / Strat:2186+2163+1401 | 持仓/Delta/RiskMonitor halt | ✓ 全部 cancelAll + HEDGE 单 | 风控型(canRecover) | R2 / R4 / R6 / R7,共 4 处 resumeFromRisk |

---

## 二、三个维度的差异分析

### 维度 1 — 暂停时撤单策略

```
TOXICITY:  ✗ 不撤,allow_bid/ask=false 让 processQuoting 本 tick 跳过
MARKET:    ✓ 单合约 cancelAll
ERROR-软: ✗ 不撤(BUG-3,已识别)
ERROR-硬: ✓ 全部 quoter cancelAll
HALT:     ✓ 全部 quoter cancelAll + HEDGE 单 cancel
```

→ **3 套撤单策略,无统一原则**。可统一为一个 PauseScope 配置。

### 维度 2 — 恢复触发器类型

```
时间型(time-driven):  TOXICITY,ERROR-软
信号型(signal-driven): MARKET (信号刷新自动 false)
状态型(state-driven): HALT(canRecover),ERROR-硬(同时挂两套)
```

→ **三种触发器混用且 ERROR-硬挂两套** = 故障源。

### 维度 3 — 恢复 call site 数量

```
TOXICITY:  1 处(Coord:778)
MARKET:    1 处(Coord:570)
ERROR:    ≥3 处(handleAutoResume 死代码 + on_entrust:2507 + R4)
HALT:     4 处(R2/R4/R6/R7)
```

→ 总共 **9+ 个 setQuotingPhase(NORMAL)/resumeFromRisk() call site**,分散在 2 个文件 4 个函数。

---

## 三、可统一性结论

### 能统一的部分

✓ **统一恢复检查调度入口**:每 tick 跑一个 `checkPauseRecovery()`,按优先级判断该不该退出  
✓ **统一时间计算**:把"paused_since + wait_threshold"做成 PauseInfo 结构,所有时间型子态复用  
✓ **统一 set call site**:把 9+ 个 setQuotingPhase(NORMAL) 收口成一个 `tryResume(reason)` 接口

### 不该统一的部分

✗ **恢复条件**:时间/信号/状态三种本质不同,强行统一会丢业务语义  
✗ **暂停副作用**:撤单策略本就该按业务区分(TOXICITY 不撤是设计而非 bug,MARKET 单合约撤是因为 vol 是合约级)

### 当前隐性问题

⚠ **优先级跨界**:HALT 期间若 MARKET 的 else 分支(Coord:570)被执行,会试图把 H 翻 N(canTransitionQuoting H→N 校验通过)→ **意外恢复**!这是隐性 bug。

  实际上 Coord:570 的 set(NORMAL) 受 canTransitionQuoting 保护,H→N 是合法,所以**真的会发生**:
  - 上一 tick 风控触发 HALT(line 640 set H)
  - 同 tick 后续 Stage 4 checkRisk 没继续走(因 638 isTradingHalted return false 已退出)
  - **但下一 tick** Stage 3(550-571)在 Stage 4 之前!shouldPause=false → 570 set(NORMAL) → H 被翻 N!
  - 然后 Stage 4 checkRisk:line 638 isTradingHalted 仍 true → 又 set H
  - 结果是每 tick 在 H↔N 之间来回闪
  
  → 这是一个**未识别的 P1 bug**(HALT 期间的状态闪烁),用户问到统一才浮现。

### 类似的还有

⚠ Coord:778 toxicity 出冷却时,若 qphase 实际是 H/E/M 而不是 T,778 的 set(NORMAL) 也会乱翻。773 上面的 if 守卫 `tc.timestamp < _toxicity_resume_time` 是时间检查不是状态检查,**只要冷却结束就跑** else 分支 set NORMAL。

→ 用户原直觉是对的:**恢复路径不统一会引发跨态意外翻转**。

---

## 四、统一化方案

### 方案 U1 — 收口 + 优先级守卫(最小改动,我倾向)

**核心思想**: 不改子态语义,但所有 setQuotingPhase(NORMAL) 必须经过一个守卫,只有"当前态本来就该退出"时才允许翻 NORMAL。

**新增接口** TradingState.h:
```cpp
/// 尝试从指定子态恢复到 NORMAL — 只有当前态匹配 expected 时才生效
/// 返回 true=成功翻 N,false=当前态不是 expected,跳过
bool tryResumeFrom(QuotingPhase expected) {
    if (qphase != expected) return false;
    qphase = QuotingPhase::NORMAL;
    return true;
}
```

**改造 9 处 call site**:

| 原代码 | 改为 |
|-------|------|
| Coord:570 `set(NORMAL)` (MARKET 退出) | `tryResumeFrom(MARKET)` |
| Coord:778 `set(NORMAL)` (TOXICITY 退出) | `tryResumeFrom(TOXICITY)` |
| Strat:1316 `set(NORMAL)` (handleAutoResume) | `tryResumeFrom(ERROR)` |
| Strat:2507 `set(NORMAL)` (on_entrust 成功) | `tryResumeFrom(ERROR)` |
| Strat:1865 `resumeFromRisk()` | 保留(H 退出专用) |
| Strat:2064 `resumeFromRisk()` | 保留 |
| Strat:2174 `resumeFromRisk()` | 保留 |
| Coord:752 `resumeFromRisk()` | 保留 |

**收益**:
- ✓ 修了 Coord:570 / 778 的跨态闪烁 bug
- ✓ HALT 期间不再被任何"退出 else 分支"误触发恢复
- ✓ 状态机语义清晰:谁触发的谁恢复
- ✓ resumeFromRisk 仍是 H 的唯一退出,语义不动

**工期**: 0.3 天  
**风险**: 极低(纯加守卫,不会引入新行为,只会让原本错的不再错)

### 方案 U2 — 引入 PauseInfo 结构 + 统一调度器(中等改动)

**新增结构**:
```cpp
struct PauseInfo {
    QuotingPhase phase;          // 暂停的子态
    uint64_t since_ms;           // 进入时间戳
    uint64_t resume_at_ms;       // 预计恢复时间(0=信号型/状态型不用时间)
    enum class Trigger { TIME, SIGNAL, STATE } trigger;
};

// TradingState 持有
PauseInfo current_pause;  // 仅在 phase != NORMAL 时有效
```

**统一调度入口** Coordinator::checkPauseRecovery():
```cpp
void checkPauseRecovery(uint64_t now_ms, ...) {
    if (qphase == NORMAL) return;
    
    switch (qphase) {
        case TOXICITY:
            if (now_ms >= current_pause.resume_at_ms)
                tryResumeFrom(TOXICITY);
            break;
        case MARKET:
            if (!sig_ctx.shouldPause())
                tryResumeFrom(MARKET);
            break;
        case ERROR:
            if (now_ms >= current_pause.resume_at_ms)
                tryResumeFrom(ERROR);
            break;
        case RISK_HALTED:
            // 保留独立路径(canRecover 经 RiskMonitor),不在此处
            break;
    }
}
```

**收益**: 
- ✓ U1 全部收益
- ✓ "暂停信息"集中管理,不再散落在 _toxicity_resume_time、_quoting_paused_since 等独立字段
- ✓ 日志/监控可以统一展示"暂停原因+剩余时间"

**代价**:
- 改动面更大,涉及 TradingState 结构 + Coordinator 调度
- TOXICITY 的 _toxicity_resume_time、ERROR 的 _quoting_paused_since 要迁移到 PauseInfo
- 跨文件 5+ 处同步修改

**工期**: 1 天  
**风险**: 中(状态结构改动,需要回测验证)

### 方案 U3 — 仅修 Coord:570 + Coord:778 的跨态闪烁(应急修补)

只修两个守卫,不动其他:
```cpp
// Coord:570
if (_trading_state && _trading_state->qphase == QuotingPhase::MARKET) {
    _trading_state->setQuotingPhase(QuotingPhase::NORMAL);
}

// Coord:778
if (_trading_state && _trading_state->qphase == QuotingPhase::TOXICITY) {
    _trading_state->setQuotingPhase(QuotingPhase::NORMAL);
}
```

**工期**: 0.1 天  
**收益**: 修两个 bug,不重构  
**缺点**: ERROR/HALT 路径仍分散,下次审计还要重做

---

## 五、推荐路径

  **U1 + ERROR 修复(继续 Q-A 方案 A)** 联合一轮:
  1. 引入 `tryResumeFrom(expected)` 接口
  2. 把 4 处非 H 的 set(NORMAL) 改成 tryResumeFrom
  3. 把 ERROR 8 个 BUG 修掉(按之前的方案 A)
  4. resumeFromRisk 路径不动(它有自己的 canTransitionQuoting 校验)

  这样得到的最终架构:
  
  ```
  QuotingPhase 退出统一规则:
    NORMAL    : -
    TOXICITY  : tryResumeFrom(TOXICITY) 在 Coord:778
    MARKET    : tryResumeFrom(MARKET)   在 Coord:570
    ERROR     : tryResumeFrom(ERROR)    在 handleAutoResume + on_entrust 成功
    RISK_HALTED: resumeFromRisk()       在 R2/R4/R6/R7(经 canRecover/重置)
  
  入口性质:
    时间型(TOXICITY, ERROR): 退出条件=now > resume_at
    信号型(MARKET):           退出条件=信号 false
    状态型(HALT):             退出条件=canRecover 通过
  
  所有退出经守卫(tryResumeFrom 内部检查 + canTransitionQuoting):
    ✓ 不会跨态翻车
    ✓ 不会被本不该恢复的 else 分支误触发
    ✓ 优先级隐式保护(HALT 期间,任何 tryResumeFrom(非 H) 都返回 false)
  ```

---

## 六、待奶酪拍板的问题

### Q-S1: 统一方案选哪个?
- (a) U3 应急修两个守卫
- **(b)** U1 收口 + 优先级守卫 + ERROR 修复(我倾向)
- (c) U2 引入 PauseInfo 结构(更彻底,工期大)

### Q-S2: tryResumeFrom 是否要返回值?
- **(a)** 返回 bool,调用方可记录恢复成功/失败用于诊断(我倾向)
- (b) void,简单一些

### Q-S3: 把这个方案和之前 ERROR 8-BUG 修复(Q-A=方案 A)合并?
- **(a)** 合并(本质都是状态机路径清理)— 我倾向
- (b) ERROR 修一轮,统一恢复入口下一轮

### Q-S4: 是否需要新发现的"HALT/MARKET 闪烁 bug"单独验证?
- **(a)** 跑一次 ao 回测,grep "set NORMAL" 日志,看 H 期间是否真有闪烁(我倾向)
- (b) 不验证,直接修

---

## 七、备注

- 本审计补回了 v2 报告漏掉的关键问题:**MARKET/TOXICITY 的 else 分支会绕开优先级,在 HALT 期间把 qphase 翻 NORMAL**
- 用户的直觉"是否可以统一恢复报价入口"指向了真问题
- 教训:状态机审计不仅看每个写入点是否合法,还要看"高优先级期间低优先级路径会不会乱翻"
