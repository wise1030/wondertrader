# WtFutuCore 运维与开发 Todo（2026-11-02 全量重整 + 逐条评估，11-03 快照更新）

> 条目来源：AGENTS.md §16~§20.8 全量扫描 + 2026-11-02 逐条评估（已完成状态核验 /
> 继续实施价值 / 处置建议）。AGENTS.md 是权威过程记录，本文件是唯一活跃待办清单。
> 文档边界：本目录为 WtFutuCore 专属文档唯一活动区，`archive/` 为历史存档。

## 评估图例
`【关闭】` 已完成或已裁决，不再实施｜`【高/中/低】` 保留价值与优先级｜`【搁置】` 用户已裁不立项

---

## 一、当前状态快照（2026-11-09）

- 生产：双策略单进程（PID 712233，08:40 cron 重启）。**ec 周末后恢复正常交易**：
  186 笔成交、0 LOSS_CRITICAL（-50000 线未被触及）、0 ORPHAN；ao 92 笔正常。
  settlement 6 秒过（07:01 交易日切换 confirm 54 秒亦正常）。
- **批 B 闩锁升级路径首次实战（10:30 ZOMBIE 爆发）**：EXCEEDED maxHold 120s
  （停 reprobe 风暴）+ PERSISTENT ≥300s 均按设计触发 ✅。**但暴露设计盲区**：
  ec2611 闩锁 10:30:16 置位 → 10:32:03 撤单 ack 已到 → 闩锁仍 3.3h 不放
  （PERSISTENT 后 reprobe 永久停止，无降级复活机制），直到 13:52 TD 断连 /
  14:01 channel ready cancel sweep 才清僵尸 lift。期间该合约停报 3.3h。
  → 批 C 立项候选：PERSISTENT 后降频温和重探（如 5min 一次）或核查 lift
  判定（ack 已到但 tracker 僵尸态未清？）。
- **13:52 TD churn → settlement-confirm 卡 9 分钟（交易时段内）**：13:50:38
  撤单重试大面积失败（SimNow 小节开盘后会话半死）→ 13:52:24 断连重登秒成
  → confirm 卡 9min → 14:01:38 自愈（watchdog SETTLEMENT-STUCK 2 条正确告警；
  较 11-03 卡 5h 不自愈轻，但同源性待批 A 覆盖）。
- watchdog 午休豁免已修（13:30 PIPELINE-DARK 7200s 误报实证 → in_break 增
  1130-1330 窗，settlement-stuck 不豁免，备份 .bak_20261109_lunchgrace，
  伪造环境验证）。凌晨 00:00-01:02 连刷 31 对告警 = 周末 cron(1-5)不跑、
  周一首跑补报周末 45h 沉默，逻辑正确但观感噪音（低优）。
- halt_shadow 分歧 3 条（REVERSIBLE churn 产物，无 IRREVERSIBLE，与位图污染
  机制一致但程度轻）。
- 未提交：批 B + 文档迁移 + 此前已部署未提交改动。
- 未提交：批 B + 文档迁移 + 此前已部署未提交改动。

## 二、P0/P1 待办

- [x] **批 B 部署（§20.8 闩锁健壮性）**：✅ **2026-11-02 16:14 已部署，夜盘+今晨
  验收全过**（md5 a9b61057）。**价值依据（当日活例证）**：11-02 09:06 开盘撤单
  ack 风暴 → ZOMBIE → HALT 84 分钟；10:31 TD 断连 → settlement 卡死 → 日盘
  报废 4.5h（两失效模式同日全中）。**验收记录**：夜盘三观察点全过（settlement
  4 秒、退避序列 1000/2000/4000 实测、闩锁秒级自解）；**11-03 09:00 开盘复测——
  ao2611 闩锁 3 秒 lift**（对照 11-02 同场景 84 分钟），退避全天完整、20 ZOMBIE
  全自愈。B3 PERSISTENT 告警暂无实战场景（无 >5min 闩锁，被退避+自愈提前消解）。
- [ ] **【中】批 A 立项（§20.8，WtUftCore 框架层，需授权）**：①启动同步原子性
  ②启动本地账 vs 柜台偏差告警 ③wt_restart.sh kill 后等待加大。**价值评估：预防性**
  ——10-19 事故已被动消化结案，此批防复发而非救火；不紧急，但持仓账类事故单次
  损失量级大，建议本批 B 部署后择周授权立项。
- [ ] **【中·根因已确认并复核闭环，待修】P1-6 阶段 2 收尾（halt_shadow → HaltGate 激活）**：
  **根因 2026-11-06 实锤 + 11-06 复核全链路验证：`HaltReasons::_bits` 是进程级
  `static` 位图（HaltReason.h:95），双策略（ao+ec 同进程）共享一份**。完整因果链
  （每环均有代码/日志实证）：
  ① haltTrading 置位全局 RISK_HALT（FutuRiskMonitor.cpp:483），ec 的 IRREVERSIBLE
  halt 无任何清除路径 → 位图污染持续整日；
  ② **被 halt 策略的 processTick 在 checkRisk 处早退**（StrategyCoordinator
  "skipping risk check" → return，影子对比块在 processQuoting 之后不可达）→
  只有健康策略（ao）采样 → 39 条分歧（11-02~11-06）零反向形态的解释；
  ③ 相位位"恰好一致"：mirrorQPhase 先清四相位位再置位（末写者胜），halt 后 ec
  无后续写入，ao 成为唯一写入者 → ao 采样时相位位恒等于自身；
  ④ ZOMBIE_LATCH 一致：ao 采样的是自己的合约闩锁。
  实证：11-06 ec 09:02:35 halt 后 ao 持续交易（760 QUOTE/成交至 14:58）而 ec 全黑；
  11-04 两次 IRREVERSIBLE halt → 当日 8 条、11-05 一次 → 10 条、11-06 一次 → 17 条，
  分歧数与 halt 次数正相关。**"legacy 采集器漏读读点 bug"假设正式证伪**——legacy
  读取正确，是镜像层跨策略污染（P1-6 阶段 1 于 09-19 按单策略设计，09-30 ec
  双策略上线后暴露）。**修复方向（待用户确认后立项）**：位图改每策略实例
  （FutuRiskMonitor 或策略状态束成员，TradingState qphase 镜像同步实例化）；
  修复后影子期重计 ≥3 交易日零分歧 → 再评估 HaltGate union/bits 激活。
  此 bug 是 HaltGate 激活的前置硬阻塞——全局位图下激活 union 会让 ao 在 ec
  halt 时误停报。
  **附带实证（运营警报）：ec 连续 4 个交易日上午 IRREVERSIBLE halt**——11-04
  09:00:40+13:52:50（两次）、11-05 09:01:19、11-06 09:02:35，均为开盘跳空打穿
  隔夜库存触发日亏线（11-06 实证 -53,150 < -50,000，3 分钟）；ec 自 11-04 起
  基本每日即停。ec 隔夜库存来源/closeout 未平净问题需专项核查（见快照）。
- [ ] **【中】ao2612 闩锁"无 lift 日志但报价恢复"异常（11-03 新发现）**：09:00:47
  ao2612 HALT_QUOTING[zombie-cancel-latch] 置位（同日 ao2611 置位后 3 秒 lift 有
  日志），**ao2612 全天无一条 lift/quoting resumed 日志，却自 09:00:54 起持续报价、
  09:01-10:30 成交 33 笔**；10:30:16 双合约重新置位前 telemetry 仍显示 2 合约
  halted。两种可能：① lift 路径日志缺失（观测缺口）；② 门控未对该合约生效
  （功能缺口）。与 P1-6 分歧 #1 同源嫌疑（同一时刻 legacy=NONE）。处置：查
  FutuRiskMonitor lift 日志覆盖与 quoter 门控消费路径，定性后修。
- [ ] **【中·批 C 候选，11-09 首实战证】ZOMBIE 闩锁 PERSISTENT 后无复活路径**：
  批 B（11-02 部署）升级链路首次实战：10:30 ZOMBIE 爆发 → EXCEEDED maxHold 120s
  （停 reprobe 风暴）+ PERSISTENT ≥300s 均按设计触发 ✅。**但暴露设计盲区**：
  ec2611 闩锁 10:30:16 置位 → 10:32:03 撤单 ack 已到（1349469698/699 canceled）→
  闩锁仍 3.3h 不放（EXCEEDED 后 reprobe 永久停止，无降级重探；lift 判定疑似依赖
  tracker 僵尸集合清空而 ack 处理未清账），直到 13:52 TD 断连 / 14:01 channel
  ready cancel sweep 才清僵尸 lift——该合约停报 3.3h（对照：批 B 后常规闩锁
  秒级~43s）。修复方向（待立项）：PERSISTENT 后降频温和重探（如 5min 一次
  cancelAll）或核查 lift 判定链（ack 已到但 tracker 僵尸态未清？）。同时记录：
  13:52 断连根因 = 13:50:38 撤单重试大面积失败（SimNow 小节开盘会话半死）→
  断连重登 → settlement-confirm 卡 9min 自愈（SETTLEMENT-STUCK 2 条正确告警；
  较 11-03 卡 5h 轻，同源性待批 A 覆盖）。
- [ ] **【中·时机绑定 ThetaGrid 灰度】19.29 arb 批 B/批 C**：批 B=通道精确撤/价差
  时效重检/同价去重 pair+方向/首帧墙钟域差/B-3 量纲；批 C=队列满降级/死键清扫。
  arb_close 出厂仍 disabled，近期无灰度计划则可降级排期；若推进 ThetaGrid live
  （五、2）则应同批先行。

## 三、框架层待批池（WtUftCore/WtCTP，需用户明示授权）

- [ ] **【低·已裁不立项，候选保留，建议重提】TraderCTP settlement-confirm 超时
  重试**：多轮实证（09-24 87min / 午后时段性 / 11-02 日盘 4.5h / **11-03 日盘
  ~5h 第四次实证**——10:32:04 断连重登即卡、13:32 第二次断连再卡，直至 15:44
  SimNow 收盘侧断开）。10-22 用户裁决"不立项、人工重启"；11-02 实证无通知渠道
  = 无人处置；**11-03 实证通知渠道（页面告警）已建、71 条 SETTLEMENT-STUCK 全
  量推达 MonSvr 面板，日盘仍黑 5 小时**——页面告警依赖人工盯屏，无人盯 = 同
  结果。若嫌人工慢，建议重提本项（WatchDog 探针级自愈：检测 ready 闩锁超时
  N 分钟 → 进程内重走 settlement 查询或自重启，纯框架 WtUftCore 域）。
- [ ] **【低-中】ActionPolicyMgr getActionRules assert→UB 修复**：Release 下
  assert 空转返回 end() 野引用；改静态空规则组。一次性异常已沉寂近两月，修复
  便宜、防复发。
- [ ] **【中】CTP-50/51/31 拒单分类通道**：策略层 orderErrorThreshold=1000 临时
  放宽值的回收依赖此项； also 减少"单次拒单→全量停报"的运营损耗。
- [ ] **【低】membin 容量写保护**：当前日订单峰值 ~1.4 万 << 20 万容量，水位
  远；容量水位纳入每周巡检，接近 10 万再立项。
- [ ] **【低】WtUftEngine 补写 marker.json**：手工更新成本可忽略，长期再议。
- [ ] **【低】框架健壮性**：ParserAdapter::init 缺段报错、段错误处理器重抛退出。
- [ ] **【中】_ec_5d 跨时段漂移源定位**：±1.6% 漂移使一切回测 A/B 只能走统计带
  判据，影响所有参数类实验效率；策略/框架侧排查（疑墙钟/时序依赖）。

## 四、P2 遗留与观测

- [x] ~~watchdog 告警出口核查~~ **【已核查·结论反转·已修】**：11-02 评估曾误判
  "告警从未落盘"（错查 monitor/logs/ 路径）——实际 `Logs/channel_watchdog.log`
  1.1MB、每日 250-320 条，**告警通道一直正常**。当日真相：SETTLEMENT-STUCK
  正确告警 4.5 小时但无人读文件 → 日盘报废。已修：①MD-DOWN 心跳源 Parser 日志
  → 双边统计 mtime（原判定恒误报，11-02 当日实证）+ 小节开盘宽限 ②告警同步
  logger→syslog ③备份 .bak_20261102，伪造环境双用例验证。**告警通知渠道 ✅ 已闭合
  （11-02 当晚）**：用户裁决接入 MonSvr 页面——runMonSvr 增 `POST /wd/alert`
  （回环限定）→ notify 面板；watchdog alert() 三路输出（文件+syslog+页面），
  验证通过（POST 200/GET 405/WatchDog 重挂载/端到端）。今日"告警 4.5h 无人知"
  缺口闭合。**仍遗留（低）**：通知仅页面被动可见，无人盯页面时仍依赖巡检——
  如需主动推送（企业微信/邮件）另行立项**。
- [ ] **【中】closeout exhausted 终态三件套**：FAK 连败耗尽后无 latch、
  "manual intervention"每 tick 刷屏、仓位隔夜（09-24 实证：-8 空头隔夜）。
  近月未复发但缺口仍在；与批 B 同车部署可摊薄成本。
- [ ] **【低-中】QUOTE cap/auth/driver 分量观测 + 开盘 3-5s 禁缓存 mid 挂 skew 单**
  （开盘秒级缓存 mid 曾致深度穿越）。
- [ ] **【中·安全】MonSvr 鉴权核查**：root 运行 / 0.0.0.0:8080 / 疑似无 token；
  需用户确认 117.135.127.173 是否本人常用 IP 并设置 WT_MON_TOKEN。
- [ ] **【低】ec 侧遗留**：审计 read=0 时序 + 42 条 unknown 噪音（纯观测账务，
  顺手清理）；ec 开盘成交率偏高 → base_spread 按 ec tick=0.5 口径评估。
- [ ] **【低】远程 dist/ 目录治理**（archive/ 结构迁移、.bak 归档）；本地 docs
  治理已于 11-02 完成。
- [ ] **【低·一行修正】主项目根 AGENTS.md 冒烟命令**：`-c config.yaml` 已失效
  （应为 `_*.yaml`）。

## 五、ThetaGrid（§20 价差网格）推进链

- [x] ~~λ 复查~~ **【关闭】**：10-29 q80=3.32 为单日尖刺，10-30 已回落 2.06；
  10-13~10-30 全窗稳定 2.0±0.3。复查完成，带参数维持不动。附带清理（顺手）：
  lambda.csv 的 10-19 重复行（n=186 部分日样本）可删。
- [ ] **【高·P3 硬卡】费率书面确认**：转真实做市账户前向券商/交易所书面确认
  ec2612 MM 费率豁免；此前维持 shadow。
- [ ] **【高·决策项】maker 入场变体**：信号级回放已证 taker 入场净期望为负
  （≈-4/笔），maker 消耗是唯一出路；属策略形态决策，先于一切 live 工程。
- [ ] **【中·现在就能做】step-2 markout 分析**：生产 episode 行与 debug 分数行
  已积累近 2 个月（远超标定的 1-2 周），跑 `scripts/analyze_toxicity_markout.py`
  产出分布/markout/重叠率报告，为阈值数据化（step-3）供证。
- [ ] **【中】shadow 数据工程**：CSV 加日期列 + 按日轮转归档（10-16/17 样本已
  实损）；止损参数回测评估（shadow 止损:入场 ≈ 5242:4313 ≈ 1.22）。
- [ ] **【低】lagRatioMax**：维持默认 0.5 不动（shadow 无拒绝侧分布，标定不可行
  已有记录在案）。

## 六、待批/搁置项池

- [ ] **【低-中】funds.csv 日终落盘**：11-02 实测仍停在 9-29（37 字节）；影响
  资金曲线对账便利性，不影响交易。
- [ ] **【低】趋势期库存收紧**：依赖 §三.7（回测漂移源）收敛后再评估。
- [ ] **【搁置】19.31 控制文件优雅退出**：用户已裁"先不立项"；重启孤儿已由
  治理批（认领播种）兜住，重提需新论据。
- [ ] **【例行】commit 推送 origin + 本地分批 commit**：批 B / 文档治理 /
  遗留已部署批（§20.4/§20.7）建议按主题 3 个 commit；推送待用户指示。

## 七、运维例行（不入批）

- 每日：zombie/latch/拒单/覆盖率四指标；halt_shadow 分歧计数（当前未达标，
  分歧行留存待归因）；开盘风暴后长闩锁检查（今日 84min 为最新样本）。
- 每周：shadow/λ 数据核查；membin 容量水位；watchdog 日志文件存在性。
- 换月：合约滚动 runbook（远程配置为活体权威、重启前备份、按乘数折算
  delta/亏损线——ec 乘数 50 教训）；marker.json 手工维护。
