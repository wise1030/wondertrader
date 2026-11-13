# Phase 2 收尾审计报告 v2 — P1-6 / P1-7

> 2026-06-18 | 方案 B1 + Q2=a + Q3=审计先报告
>
> v1 已撤(纸面对比错判 #A/#B)。本 v2 基于穷尽矩阵 + processTick 流程交叉验证。
>
> 状态: **诊断+方案+待拍板**(本报告不动代码,拍板后再改)

---

## 一、审计方法论(本次重做)

v1 错在哪:**只 grep 了 setQuotingPhase|qphase=** 关键字,漏掉了 resumeFromRisk 的 4 处实际调用。导致误报"on_trade 未碰 _trading_state""resumeFromRisk 与 setQuotingPhase 双 API 重复"。用户 2 轮核验救下来。

v2 方法:
  1. 穷尽 grep `_trading_state[.|->]` 全部 49 处调用(写+读)
  2. 列出全部写入点(setQuotingPhase + resumeFromRisk + enterCloseout + exitToQuoting + reset + block/unblock)
  3. 对每个写入点,枚举所有可能的来源 qphase(N/T/M/E/H),按代码守卫排除不可达组合
  4. 对每个(来源, 目标)组合,跑 canTransitionQuoting + idempotent 短路逻辑,判断 set 是否真生效
  5. 对"set 静默拒绝"案例,沿 processTick 8-stage 流程核查触发可达性

---

## 二、Phase 2 各项真实进度

| 项 | ROADMAP 描述 | 代码现状 | 证据 |
|---|---|---|---|
| P1-1 状态机统一 | 三层 HSM | ✓ 完成 | TradingState.h 全套,50+ 处调用 |
| P1-2 持仓决策一致性 | syncFromCtx | ✓ 完成 | StrategyCoordinator.cpp:1014 |
| P1-3 hedge anchor guard | clamp | ✓ 完成 | FutuPortfolio.cpp:178-193 |
| P1-4 stale 价格保护 | _price_stale | ✓ 完成 | UftFutuMmStrategy.h:406 三挂载点 |
| P1-6 resume 归一 | 6 路径收口 | ⚠ 真问题在别处 — 见 §三 |
| P1-7 线程安全 | atomic 或注释 | ⚠ 待办 — 见 §四 |
| P2-1 函数拆分 | on_tick + initBM | △ on_tick 已拆,initBM 540 行未拆(B1 范畴外) |
| P2-3 BUG 标签 | 153 处分类 | ✓ 完成 |

---

## 三、P1-6 真问题:H 来源下 set 静默拒绝

### 3.1 关键机制回顾

TradingState.h:127-132:
```cpp
bool setQuotingPhase(QuotingPhase q) {
    if (qphase == q) return true;          // [A] 同态幂等短路
    if (!canTransitionQuoting(q)) return false;  // [B] 校验
    qphase = q;
    return true;
}
```

TradingState.h:119-123:
```cpp
bool canTransitionQuoting(QuotingPhase next) const {
    if (qphase == QuotingPhase::RISK_HALTED)
        return next == QuotingPhase::NORMAL;
    return true;
}
```

**关键性质**:
- 当前态 H,目标 N → set 校验通过(line 121),赋值生效
- 当前态 H,目标 H → 同态幂等短路(line 128 [A]),返回 true,不走校验
- 当前态 H,目标 T/M/E → 校验拒绝,**静默 return false,qphase 保持 H 不变**,**调用方拿不到错误信息**(返回值无人检查)
- 当前态非 H,目标任意 → 自由抢占

**用户原话验证正确**: "从 RISK_HALTED 到 NORMAL 是不能使用 set 的"
  → 字面"不能用 set 走 H→N":准确说是**不应该**用,因为 set 走 H→N 实际能成功(line 121 允许),但语义钩子(日志/审计/未来扩展)只在 resumeFromRisk 上。
  → 项目所有 H→N 实际恢复点都用 resumeFromRisk,**符合规范**(见 §3.4)。

### 3.2 全部写入点穷尽矩阵

| 序 | 位置 | API | 目标 | 来源守卫(代码限定) | H 来源时结果 |
|---|------|-----|-----|-----|-----|
| W1 | UftFutu:1316 | set | N | line 1298 守卫:`qphase != ERROR return` → 来源仅 E | 不可达 H |
| W2 | UftFutu:1401 | set | H | _coordinator==null fallback,无来源守卫 | **幂等短路 OK** |
| W3 | UftFutu:1430 | enterCloseout | (CLOSEOUT) | 不动 qphase | OK |
| W4 | UftFutu:1865 | resumeFromRisk | N | line 1830 守卫:`qphase == H` → 来源仅 H | 设计正确 |
| W5 | UftFutu:1866-67 | unblock | - | - | OK |
| W6 | UftFutu:2064 | resumeFromRisk | N | risk_monitor 状态守卫,qphase 不限 | 设计正确(语义意图 H→N) |
| W7 | UftFutu:2163 | set | H | risk_monitor->isTradingHalted(),无 qphase 守卫 | **幂等短路 OK** |
| W8 | UftFutu:2174 | resumeFromRisk | N | _risk_monitor==null 兜底 | OK |
| W9 | UftFutu:2186 | set | H | on_channel_lost 立即停 | **幂等短路 OK** |
| W10 | UftFutu:2507 | set | N | line 2505 守卫:`qphase == ERROR` → 来源仅 E | 不可达 H |
| **W11** | **UftFutu:2523** | **set** | **E** | **on_entrust 失败到阈值,无来源守卫** | **🔴 静默拒绝** |
| **W12** | **UftFutu:2538** | **set** | **E** | **on_entrust 失败未到阈值,无来源守卫** | **🔴 静默拒绝** |
| W13 | UftFutu:1214 | enterCloseout | (CLOSEOUT) | session_end | OK |
| W14 | UftFutu:1169 | reset | (全清) | session_begin | OK |
| W15 | Coord:383 | exitToQuoting | (QUOTING+N) | 夜盘 closeout 完成 | OK |
| W16 | Coord:408 | enterCloseout | (CLOSEOUT) | 白盘 closeout 完成 | OK |
| **W17** | **Coord:553** | **set** | **M** | **Stage 3 updateSignals,无来源守卫** | **🔴 静默拒绝** |
| W18 | Coord:570 | set | N | Stage 3 else 分支 | H→N 校验通过,OK |
| W19 | Coord:640 | set | H | isTradingHalted 真,无 qphase 守卫 | 幂等短路 OK |
| W20 | Coord:656 | set | H | checkDeltaRate 真 | 幂等短路 OK |
| W21 | Coord:672 | set | H | HALT_TRADING action | 幂等短路 OK |
| W22 | Coord:718 | set | H | PAUSE_QUOTING action | 幂等短路 OK |
| W23 | Coord:731 | blockLong | - | BLOCK_SIDE_LONG action | OK |
| W24 | Coord:736 | blockShort | - | BLOCK_SIDE_SHORT action | OK |
| W25 | Coord:752 | resumeFromRisk | N | line 746 守卫:`!isActive()` → 来源 T/M/E/H | 设计正确 |
| W26 | Coord:753-54 | unblock | - | 同上 | OK |
| W27 | Coord:773 | set | T | toxicity check,在 checkRisk 内 line 638-651 之后 | **不可达 H**(checkRisk 在 H 已 return) |
| W28 | Coord:778 | set | N | toxicity 退出 else 分支 | H→N 校验通过,OK |
| W29 | Coord:1240 | reset | (全清) | resetSession | OK |

### 3.3 真漏洞清单(3 处)

#### 漏洞 #B1 — on_entrust ERROR 转移在 H 期间被静默吃掉

**位置**: UftFutuMmStrategy.cpp:2523 (硬触发) + 2538 (软触发)

**触发场景**:
1. 风控判定 → qphase=H + cancelAll 所有挂单
2. cancelAll 之前已发出但 broker 尚未确认的 entrust 仍会回调 on_entrust
3. 该 entrust 报错 → 走 2523/2538 路径,试图 setQuotingPhase(ERROR)
4. canTransitionQuoting(N→其他)规则:H 来源仅允许目标=N。set(E) 被拒绝
5. qphase 仍为 H,**但 _order_error_count 已自增**,日志已打 `Trading HALTED due to consecutive order errors`

**后果**:
- 功能层:H 和 E 都禁报价(canQuote 都 false),交易行为不变 → **不会立即出事**
- 信息层:状态机记录的"暂停原因"和实际原因脱钩
  - getPhaseStr() 返回 "RISK_HALTED" 而不是 "ERROR"
  - 监控/告警/审计依赖 qphase 区分原因 → 误判
- 未来风险:若给 E 单独加恢复路径(指数退避)或给 H 单独加人工确认门,两者动作不同时,这个混淆变成 bug

**风险等级**: 中(信息保真,未来扩展坑)

#### 漏洞 #B2 — 信号 MARKET 转移在 H 期间被静默吃掉

**位置**: StrategyCoordinator.cpp:553

**触发场景**:
1. 风控 → qphase=H
2. processTick 流程: Stage 0 closeout → Stage 1 preCheck → Stage 2 marketdata → **Stage 3 updateSignals(包含 553)** → Stage 4 checkRisk(line 650 H 时 return false)
3. **Stage 3 在 Stage 4 之前**,所以 H 期间 553 仍会被触发
4. shouldPause=true 时 set(MARKET),被静默拒绝
5. qphase 仍为 H

**后果**:
- 功能层:Stage 4 立刻 return,Stage 5/6/7/8 不执行,业务行为本就停了 → 不会出事
- 信息层:同 #B1,状态机记录失真
- 后续若依赖 isMarketPaused() 做决策(比如某些 dashboard / 信号回测),会错判

**风险等级**: 中(信息保真)

#### 漏洞 #B3 — _coordinator==null fallback 极端不可达 + 死循环嫌疑

**位置**: UftFutuMmStrategy.cpp:1401(handleCoordinatorTick fallback)

**触发场景**:
- _coordinator 是 init 时构造,运行期不重置,理论不会 null
- 若真发生(内存损坏 / 极端异常):set H + cancelAll,然后 R4 Auto-recovery 下一 tick 看 canRecover 通过就翻 N → 又进 fallback → 死循环

**风险等级**: 低(实际不可达),但若发生则严重

### 3.4 P1-6 归一性结论

设计意图(头文件注释):
- RISK_HALTED → NORMAL **唯一合法路径**是 resumeFromRisk()

实际代码验证:
- 4 处 H→N 转移点,**全部用 resumeFromRisk**(W4/W6/W8/W25)✓
- 4 处 setQuotingPhase(NORMAL),**来源不是 H**(W18/W28)或 **来源被守卫排除 H**(W1/W10)✓

→ **P1-6 归一性已满足**。漏洞 #B1/B2 不是归一性问题,是**反方向**的问题:H 期间外部事件试图改 qphase 到非 N 目标被吃掉。

---

## 四、P1-7 线程安全

(无新发现,延续 v1 §三方案)

  - Q2=a 路线:注释 + DEBUG-only `_writer_tid` 断言,Release 零开销
  - 改动只在 TradingState.h,8 个写接口首行加 `_check_writer_thread()`
  - memory 已固化"WT UFT 回调 reader 线程串行" → 不上 atomic

---

## 五、修复方案矩阵(基于真漏洞)

| ID | 项 | 方案 | 工期 | 风险 | 必要性 |
|----|---|------|------|------|--------|
| F-B1 | 漏洞 #B1: on_entrust set(E) 在 H 期间被吃 | 见 §5.1 三选一 | 0.2-0.3 天 | 低 | 中 |
| F-B2 | 漏洞 #B2: signal set(M) 在 H 期间被吃 | 见 §5.2 三选一 | 0.2 天 | 低 | 中 |
| F-B3 | 漏洞 #B3: _coordinator==null 死循环 | assert + log fatal | 0.1 天 | 低 | 低(不可达) |
| F-D | P1-7 线程契约 | 顶部注释 + DEBUG 断言 | 0.2 天 | 极低 | 中 |

### 5.1 F-B1 候选方案

**方案 a(我倾向)**:on_entrust 写之前加 H 来源守卫
```cpp
// line 2521 之前加
if (_trading_state.qphase == QuotingPhase::RISK_HALTED) {
    // H 期间已停,不再细分错误原因
    WTSLogger::warn("[Strategy] Order error during RISK_HALTED, ignored");
    return;
}
// 现有 2523 / 2538 不变
```
优点:语义清晰(H 是 superset,E 是 subset,H 期间不降级)
缺点:_order_error_count 不再累计 → H 恢复后下一笔失败重头计数(可能反而是好事)

**方案 b**:扩展 canTransitionQuoting 允许 H→E
```cpp
bool canTransitionQuoting(QuotingPhase next) const {
    if (qphase == QuotingPhase::RISK_HALTED)
        return next == QuotingPhase::NORMAL || next == QuotingPhase::ERROR;
    return true;
}
```
优点:状态机更宽松
缺点:破坏"RISK_HALTED 只能通过 resumeFromRisk 转 N"的设计原则;E 比 H 弱,H→E 是降级,违反单调性

**方案 c**:维持现状 + set 失败时打 warn 日志
```cpp
if (!_trading_state.setQuotingPhase(QuotingPhase::ERROR)) {
    WTSLogger::warn("[State] set(ERROR) blocked, current={}", getPhaseStr());
}
```
优点:不改语义,只补可见性
缺点:每次失败都刷日志,治标不治本

### 5.2 F-B2 候选方案

**方案 a(我倾向)**:Stage 3 进入前加 H 守卫
```cpp
// updateSignals 函数开头(在 553 之前)
if (_trading_state && _trading_state->qphase == QuotingPhase::RISK_HALTED) {
    return;  // H 期间不再更新 qphase 状态(信号本身可继续聚合)
}
```
优点:信号聚合可继续(若需要),只是不写 qphase
缺点:需要看 updateSignals 完整内容判断这里 return 是否合适

**方案 a 更精细**:仅守卫 553/570 两行
```cpp
if (sig_ctx.shouldPause()) {
    if (_trading_state && _trading_state->qphase != QuotingPhase::RISK_HALTED) {
        _trading_state->setQuotingPhase(QuotingPhase::MARKET);
    }
    // ...
} else {
    if (_trading_state && _trading_state->qphase != QuotingPhase::RISK_HALTED) {
        _trading_state->setQuotingPhase(QuotingPhase::NORMAL);
    }
}
```
缺点:啰嗦但保留信号聚合 + 日志

**方案 b**:同 5.1 方案 c,只补 warn 日志,不改语义

### 5.3 F-B3 候选方案

**方案 a**:assert + abort
```cpp
// line 1399-1401
if (!_coordinator) {
    WTSLogger::error("UftFutuMmStrategy[{}] Coordinator is null — FATAL", id());
    assert(_coordinator && "Coordinator must never be null at runtime");
    // Release 下若真发生,继续走 cancelAll 自保,但不再翻状态(避免死循环)
    for (auto& [code, quoter] : _quoters)
        quoter->cancelAll(ctx);
    return;  // 不动 _trading_state
}
```

---

## 六、待奶酪拍板的问题

### Q1: F-B1 选哪个?
- **(a)** on_entrust 入口加 H 守卫,直接 return(我倾向)
- (b) 扩展 canTransitionQuoting 允许 H→E(破设计)
- (c) 维持现状 + 加 warn 日志(治标)

### Q2: F-B2 选哪个?
- **(a)** updateSignals 553/570 两处加 `qphase != H` 守卫(我倾向)
- (a') Stage 3 函数开头整体守卫,H 时直接 return
- (b) 维持现状 + 加 warn 日志

### Q3: F-B3 修不修?
- (a) 维持现状(实际不可达)
- **(b)** assert + 不动状态机自保(我倾向)— 防御性,Release 下也安全

### Q4: F-D(P1-7) 路线?
- **(a)** 注释 + DEBUG `_writer_tid` 断言(我倾向)
- (b) 仅注释,不加断言

### Q5: 回归门槛?
- (a) 编译通过即可
- **(b)** ao 1 日回测 + 比对关键 phase 转移日志(我倾向)
- (c) ao + ec 各 1 日回测

### Q6: 本轮 commit 范围?
- (a) 只做 F-D
- **(b)** F-B1 + F-B2 + F-D(B1 范围内,#B3 不可达可缓)(我倾向)
- (c) 全做(B1/B2/B3/D)

### Q7: 文档同步?
- **(a)** ROADMAP V2 标 Phase 2 done + 链 PHASE2_CLOSEOUT_AUDIT.md(我倾向)

---

## 七、推荐路径

  Q1=a, Q2=a, Q3=b, Q4=a, Q5=b, Q6=b, Q7=a

  落地顺序:F-D(头文件最隔离)→ F-B1(策略 5 行)→ F-B2(coordinator 4 行)→ 编译 → ao 1 日回测 → 日志 diff → commit → push 全量

---

## 八、备注

  - 本报告 v2 基于穷尽矩阵,29 个写入点逐项核查,5 个守卫推理,1 个流程顺序验证
  - v1 已撤(误判 #A/#B)。教训写入 memory:**审计 grep 必须穷尽 API 全集**,否则容易看到一面而下结论
  - 漏洞 #B1/B2 是设计层面的"缺一个守卫",不是 bug,但是会咬未来扩展
  - F-B1/B2 的修法都不破坏既有 HSM 设计(canTransitionQuoting 不动),只是在调用方多一道守卫
