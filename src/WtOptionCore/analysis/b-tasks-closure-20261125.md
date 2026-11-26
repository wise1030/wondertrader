# B 类技术债执行与闭环记录（2026-11-25/26）

范围：`outstanding-items-20261125.md` 的 B1–B14。诊断项区分“工程修复闭环”与“历史根因证据”；不以未复现冒充根因证明。

| 项 | 执行结论 | 代码 / 验证证据 |
|---|---|---|
| B1 | **闭环**：按自然到期日过滤期权订阅，禁用但未到期月份仍可热开 | `HftOptionStrategy::loadOptionExpireDates/on_init`；生产 418 条到期表、保留 150、过滤 268、`cu2612` 订阅 0。修复 discovery 日志 5 占位符/4 参数的启动回归 |
| B2 | **闭环**：到期键过滤 + session begin 统一重写日期 | 本机 `hot_params.json` 从 tdate=0/含 202612 变为 `tdate=20261126`、无陈旧键且有效值保持；生产亦已复核该文件 |
| B3 | **闭环**：PositionGuard 六字段全部 typed 接线；reconcile 保留窄 API | `positionGuard.{tolerance,alertCooldownSec,disableOnBreach,staleFreezeSec,clampLimit,undoWindowSec}`；三类构造点统一配置；负数/非有限输入拒绝；生产 effective dump 六项正确，clamp/undo/stale=0。session/channel 仍只用 reset |
| B4 | **工程闭环；历史根因证据受限** | 修 `WtKVCache` mmap 返回值悬空、已有 key 无锁覆盖、clear 空对象死锁；修 `TraderAdapter::quote/doEntrust` 空指针安全失败。Release+ASan cache 各 5/5；ASan WtRunner 45s `rc=0`、0 ASan report/0 SIGSEGV。第三方 SDK new[]/delete mismatch 单独记录并定向屏蔽。原始全线程 core 缺失，不把上述缺陷冒称为 11-24 已证实根因 |
| B5 | **闭环**：qty=5 来源已证明并消除 | 202702 缺 max_qsize → 策略默认 5 → EXPIRYENABLE 覆盖 pricer 默认 1 → 报价 5。策略与 loader 缺省统一 1；生产 202702/03/04 显式 1；effective dump 和热开启日志输出有效值。原生同价 replace 被 max_side_orders 永久自锁的问题也补增量/减量回归 |
| B6 | **闭环**：策略 `_positions` 已删除，PositionBook 单源 | 成交/快照/OptionData/OQM/PnL/hedge/margin 全读 book；新增 worker-only const view，热路径零拷贝；session begin 显式覆盖 grid/OTG 零仓位代码；session end 保留 Σabs(net)，不改 gross 语义。9/9 与生产启动通过 |
| B7 | **闭环**：纯死配置面移除 | `risk_prem_fut/rp_quote/rp_future/reduce_large_size/wgt_sizebias/interesting_ticks_* /max_size_ahead*` 等从字段、schema、示例清理；全仓库相关代码检索为 0；全量构建证明无隐藏引用。生产未使用这些键 |
| B8 | **闭环**：期权不再调用 hedge setter | 仅 `!CodeHelper::isStdChnFutOptCode` 才进入 `OptionRisk::setHedgePosition`；期权 RiskData 更新和未知期货告警保留。本机集成 `unknown hedge code=0` |
| B9 | **闭环（保留分工，不合并、不调参）** | ddAdj=原始月净 delta 线性项；L3=目标/跨月调整后二次项。生产调用同一 `QuoteRiskHelpers`，D=607/pgn=.607/legDelta=.5 数值回归通过。二者在当前参数下同向叠加的风险已记录，真实 2×2 属后续调参，不在技术债清理里改报价 |
| B10 | **闭环（保留零默认）+ 缺陷修复** | 0 明确定义为 per-expiry-only、不假设跨月抵消、无距离/wing 衰减；本月风险仍有效。无实证不臆造相关系数。混合 bucket 自持仓重复计入已修为精确 self-exclusion，并覆盖 mixed/self-only/non-self/wing 测试 |
| B11 | **闭环**：GVV 旧 alpha 反演缺陷修复 | `volvol2` 改用候选 `alpha`；新增 alpha=1.8、volvol=.27 高精度合成数据 fit→eval 回归、翼部不 fallback、窗口 clamp；测试通过。失败日志含异常/rawX/clampedX/window/F/T/atmVol/spotvol/rho/volvol/alpha，计数改原子 |
| B12 | **闭环（deprecated 兼容期）** | 修 replay 顺序到 setupPricer 后、sync_param 前；本机 UI 证明 config=3/文件=7 时运行值=7。旧 config/热键仍可写可回放但明确 deprecated；schema 保留避免 UNKNOWN 双告警；新旧公式数值不变，不做隐藏 100× 单位迁移 |
| B13 | **闭环**：Stress 独立 SingleFlight 后台化 | 新 `AsyncStress` 深值快照；后台不接触 grid/risk/state/UI；worker 在通用 panic 块后一次性发布；session/position/time/TTL 过期丢弃并尽快重算；stop drain+join，stop 后拒提交。`test_stress`、AEP 测试通过 |
| B14 | **关闭（设计决策）** | panic_fail_streak_sec/rate 继续位于 delta_hedge，loader/schema/config 一致；不迁移，避免配置漂移 |

## 统一验证结果

- Linux 原生盘 Release clean rebuild：首次遇既有 CMake 并行后置拷贝竞态（QuoteFactory 等 libWtDataStorage），续全量构建完成，最终错误数 0。
- WtOptionCore：**test_async_event_processor / test_command_handlers / test_ctg_ranking / test_enhancements / test_hedge_coordinator / test_replay_benchmark / test_scanners / test_stress / test_uibridge，9/9 PASS**。
- TestUnits：237 项中 233 PASS；4 项失败均有明确非本轮原因：
  - `test_session.test_allday`：既有 all-day 时间语义断言；
  - `test_shm.test_sharehelper`：硬编码 Windows `E:\\...` 路径；
  - `test_theta_grid.replay_ec_4d*`：缺外部 grid CSV，测试自行提示需先拉数。
- cache 专项：Release 5/5、ASan 5/5；覆盖未初始化 clear、跨扩容指针快照、2000 key 多轮扩容、reader/writer 并发与已有 key 覆盖。
- 本机 Release 75s：正常 SIGTERM 退出、0 SIGSEGV、0 unknown hedge；B2/B3/B5/B12 运行值与文件均已验证。
- 本机 ASan 45s：`rc=0`，无 AddressSanitizer 报告。仅对第三方 `thosttraderapi_se.so` 已定位的退出 alloc-dealloc mismatch 设置 `alloc_dealloc_mismatch=0`。
- UI 本轮无 TypeScript/schema 结构变更（旧键继续显示），无需以 `tsc` 代替 C++ 回归。

## 生产部署

时间：2026-11-26 09:51（Asia/Shanghai）；PID：3432119。

部署并备份：`WtRunner`、`hft/libWtOptionCore.so`、`traders/libTraderCTP.so`、`config.yaml`，备份后缀均为 `.bak-20261126-b-all`（TraderCTP 为 `config` 之外的 `.so` 对应备份）。

sha256：
- WtRunner `fe8a9255f2f2e33cf5ec1a9eac21efda9de0868c9082492eb1d09c60b89e34dd`
- libWtOptionCore `1741042e8f9cb8c8337565c86cdab9e56a16b01d27a1cbe8e7323c2caddf3ddc`
- libTraderCTP `657ea4586047d6a168de116f7a0eaef3a5520dd77730efc9d7471396d0666624`

说明：TraderCTP 源文件未修改；因其使用内联公共头 `WtKVCache.hpp`，网关库必须同步重编才能让 cache 修复生效。

部署后 90 秒复核（09:53:31）：进程存活；`segmentation violation=0`、`UNKNOWN key=0`、`GvvVolCurve::eval solve failed=0`、`unknown hedge code=0`。该时段 SimNow 交易通道尚未 ready，故生产 `STRESS=0` 只是未被触发，**不作为 B13 功能覆盖证据**；B13 由上述专项测试验收。
