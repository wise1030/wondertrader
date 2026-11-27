# 做市义务合规修订：风险压制回归"价格表达"（2026-11-27）

> 触发问题：qmode=ON 后大量合约无报单。复核结论：v3 引入的 size 收缩
> （方向性抑制）在 |pgn|≥1.0 时把增险边 size 归零 = 不挂单，违反做市
> 双边报价义务；源项目 quantbox 无此机制。本文记录根因、源项目对照与
> 修订方案（用户已确认）。

## 1. 根因链（当日实证）

```
持仓 vega −13220（限额 10000，targ +5000 → pgn_vega ≈ −1.82）
→ v3 risk_shrink_start=0.8：增险边(卖) size 线性收缩
→ |pgn|≥1.0：size=0 → 42 腿 ask 侧全部消失（静默，无状态标记）
→ 叠加 int 截断：max_qsize=1 的月份 0.8 档即全灭（"线性"退化为硬开关）
```

## 2. 源项目对照（证据：panic-trigger-evaluation.md §1、v3 文档 §2）

| 机制 | 源项目 quantbox | 本项目（修订前） |
|---|---|---|
| 持仓风险表达 | **连续 shift**（vega_risk_norm 连续驱动报价平移，"继续报价但往降险方向倾斜"） | shift（同源保留）+ **v3 新增 size 收缩/归零** |
| pgn 分档处置 | **无**（0.8/1.0/1.2 阶梯为 v3 自创） | 有（本次撤销） |
| Panic 触发 | 仅市场信号型（PanicSignal），**持仓风险从不触发** | v4 已对齐（severity→Panic 已移除，存 pricerPanic/checkerPanic/reducingChannelFailed） |
| Panic 动作 | pricer blackout + 主月期货对冲腿保留 + 300s 自动恢复 | 同 |
| 持仓顶格 size=0 | 有（限仓=义务豁免情形） | 保留（唯一合法的 0） |

核心原则（用户确认）：**做市义务要求双边在场 + 最低手数 + 最大价差；
风险压制只能表达为"价格后退"（义务内最宽价差），不能表达为"不挂单"；
size=0 只属于 Panic / 限仓 / 无盘口等豁免状态。**

## 3. 修订内容（已实施，2026-11-27 12:04 上线）

### 代码
1. `CompositeOptionPricer::__computeQuoteSize`：
   - 增险边 `size = max(min_quote_qty, ceil(size×riskScale))` —— 不再归零；
   - **仅持仓顶格（max_pos 耗尽）返回 0**（义务豁免）；
   - ceil 修复截断（1 手档不再被收缩硬开关清零）。
2. `min_quote_qty` 配置化：`expiries[].min_quote_qty`（默认 1），
   链路 ExpiryConfig → setMinQuoteQty → ExpiryRiskConfig → 报价；
   ConfigValidator 同步。**协议数值待确认**——若 cu 期权义务最低手数
   ≥2，202702–202704（max_qsize=1）需同步上调。
3. 期货腿（非义务合约）的 delta 收缩保留不动。

### 配置（生产+本地）
- `risk_shrink_start: 0.8 → 1.0`（size 收缩禁用，回归源项目"价格独担"）；
- shift 加深：`risk_shift_distort_vega_vol 0.002→0.005`、
  `risk_shift_max_vega_vol 0.005→0.02`（ATM 增险边后退 ~220 元，随 pgn
  连续加深至封顶）；
- 各月 `min_quote_qty: 1`。

### 部署
so `45ce5277…`（.new + mv 原子替换，旧版 `.bak-20261127-obligation`）；
午休窗口重启（进程 3869885，12:04:32 登录成功）。
退出撤单实战验证：旧进程 shutdown `cancel-all issued for 6 live orders`。

## 4. 遗留（下轮）

- 义务三指标监控（双边在场率 / 手数达标率 / 价差达标率）+ UI pgn 状态；
- 深虚值腿最小后退 tick 下限（shift ∝ 腿 vega，深虚值后退过浅）；
- shift 封顶与义务最大价差公式挂钩（待协议数值）；
- 对账脚本（tools/reconcile_oqm_counters.py）挂 15:05 cron；
- 全部未提交改动整理提交。

## 5. 当日相关事故备忘（部署操作类，非代码缺陷）

- scp 覆写运行中 .so → 两次 segfault（10:06/10:40）→ **规范：scp .new + mv**；
- 本地旧 tdtraders.yaml 覆盖生产账户（048363→049641 已修正，密码以
  tdparsers.yaml 为准 clx#@10300）；
- SimNow 登录失败次数限制（ErrorID=75）：频繁重启触发，按账户锁定。
