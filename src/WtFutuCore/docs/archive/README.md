# docs/archive/ — 历史存档

> 存放已被后续批次取代的审阅/评估/路线图文档。原则：只移不删（保留溯源），
> 活动文档不得再引用本目录内容作为现状依据；如确需引用，在引用处注明"存档"。

## 存档清单（2026-11-02 批次）

| 文档 | 年代 | 取代依据 |
|---|---|---|
| MM_SOFT_RISK_V3.md / MM_V3_TUNING_RESULTS.md | 2026-06 | V8-R1~R5 毒性归一化+R10 单层暂停重构 |
| ec_mm_reverse_engineering.md / EC_BACKTEST_ANALYSIS.md | 2026-06 | 19.x 全线 + ThetaGrid(§20) 取代早期 EC 分析 |
| OPTIMIZATION_ROADMAP_V2.md | 2026-06-14 | R1-R8 + 19.x 已全部执行完毕 |
| PHASE1/2/4_*.md（6 份） | 2026-06-18~21 | 对应阶段早已交付，多轮重构后口径过期 |
| ERROR_STATE_MACHINE(.md/_AUDIT) | 2026-06-18 | 批次 D ERROR 单轨化(08-24) + D1(09-01) 落地 |
| PAUSE_RESUME_UNIFICATION_AUDIT.md | 2026-06-18 | R10 单层 TOXICITY + P1-6 halt 收敛取代 |
| POSITION_BREACH_FIX.md | 2026-06-20 | UftMocker 相关修复早已合入并验证 |
| STRATEGY_SUBSYSTEM_VERIFICATION.md | 2026-06-21 | 基线(20 项测试)已失效，验证口径多轮更新 |
| ALPHA_*.md（3 份） | 2026-06-21 | ICWeightTracker 实装口径不同 + R10 毒性简化 |
| MM_ARB_ARCHITECTURE.md / MM_ARB_B_FEASIBILITY.md | 2026-06-22 | arb 子系统经 R4b/R7-c/§20 多轮重建 |
| ARB_SELF_CLOSE_DESIGN.md | 2026-07-19 | arb_close 出厂 disabled；19.29 审查为当前口径 |
| DEEP_ANALYSIS_V5~V7(+REVIEW)（4 份） | 2026-07-18~21 | V8 诊断报告及 R1-R5 已全部执行 |
| DIAGNOSTIC_REPORT_V8.md | 2026-08-20 | R1-R5 完成，结论已固化进 AGENTS.md 实施记录 |
| DIAGNOSTIC_REPORT_V10.md | 2026-08-26 | R7-a~e 已全部执行完毕 |
| BASELINE_METRICS.md | 2026-08 | 基线(20 项测试/67MB .so)已失效 |
| FUTU_CALLBACK_LOCK_EVAL.md | — | FUTU_CB_LOCK_BIG 宏已整体删除(§20.7) |

## 保留在活动 docs/ 的文档

- `CLOSEOUT_EXECUTOR_DESIGN.md` — 收盘执行设计，CloseoutExecutor.cpp/.h 头注释直接引用
- `TODO.md` — 运维/开发待办（唯一活跃清单）
- `review/` — 近期个案复核（10-18 ~ 10-29），为当前决策的事实底座
