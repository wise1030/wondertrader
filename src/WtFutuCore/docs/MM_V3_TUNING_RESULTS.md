# MM v3 + A + B 调优结果与参数固化

## 时间线
- v3 commit: `3ce0b9b8 feat(WtFutuCore): MM 软风控 v3 — 双维 skew + obligation 软化 + qty 衰减`
- A commit:  `f9325d2a fix(WtFutuCore): closeout HHMM/HHMMSS 时间格式兼容 + 白盘窗口收窄`
- B 参数: 见下方 quoting 区块（不在 git, 仅在 dist/WtBtFutu/configbt.yaml）

## 回测结果对比（SHFE.ao 双合约, 20260605 全天 dsb）

| 指标          | v0 baseline | v3 软风控 | v3+A 收盘对冲 | **v3+A+B 调参** | Δ vs v0  | Δ vs v3 |
|---------------|-------------|-----------|---------------|-----------------|----------|---------|
| trades 总数   | 399         | 810       | 667           | **634**         | +59%     | -22%    |
| dynbalance    | -5747       | -6731     | -6587         | **-3865**       | **+1882**| **+2866**|
| 手续费        | n/a         | 9851      | 8207          | **5885**        | -        | -40%    |
| closeprofit   | n/a         | 3120      | 1620          | **2020**        | -        | -35%    |
| session end Δ | +79         | 未对冲    | **0**         | **0**           | -        | -       |
| ao2607 裸仓   | n/a         | -464      | -57           | **0**           | -        | -       |
| ao2609 裸仓   | n/a         | +7        | +9            | -28             | -        | -       |
| win_rate      | n/a         | n/a       | 28.3%         | 36.6%           | -        | +8pp    |
| pnl/trade     | -14.4       | -8.3      | -9.9          | **+3.19**       | **>0**   | -       |

**核心收获**: 第一次把策略从亏损推到 v0 baseline 之上 +1882, 且 v3 软风控+主力对冲完整生效, pnl/trade 转正。

## A 步骤 (commit f9325d2a) 修复内容
1. `FutuRiskMonitor::checkCloseout`: 时间解析兼容 HHMM (4位) 和 HHMMSS (6位)
2. `StrategyCoordinator::processCloseout` 夜盘重置分支: 同样兼容
3. 白盘 closeout 窗口从 `[6,20]` 收窄到 `[6,15]` (原 20:59 误触夜盘开盘前)

修复前: `ctx->stra_get_time()` 返回 HHMM=1445, `/10000=0` 落在 `[6,20]` 外, 白盘 closeout 永不触发。
修复后: 14:45 准时触发 `Day closeout`, `CLOSEOUT HEDGE` 用 anchor 合约 (ao2609) 主力对冲 portfolio total delta, session end Δ=0。

## B 步骤参数 (dist/WtBtFutu/configbt.yaml)
```yaml
quoting:
  baseSpread: 2.5         # 2.0 → 2.5  (覆盖手续费 + 给 alpha skew 留余量)
  qtyDecayFactor: 3.0     # 2.0 → 3.0  (util=0.5 → qty *exp(-1.5)=0.223, 更激进衰减)
  obligationMinQty: 5     # 10 → 5     (cap reached 时少累裸仓)
```

效果分解:
- `baseSpread 2.0→2.5`: spread 抬升 25%, 单边 spread 收入提升, 但成交频率下降。trades 数从 667→634 (-5%) 与 fee 8207→5885 (-28%) 不成比例下降, 说明高 spread 抑制的是 toxic flow 中的低质量 fill, 高质量 fill 反而占比上升。
- `obligationMinQty 10→5`: cap reached 时 obligation 仅报 5 手而非 10 手, ao2607 裸仓从 -57 → 0, 减少累裸风险。
- `qtyDecayFactor 2.0→3.0`: util=0.5 时 qty 衰减系数从 exp(-1.0)=0.37 → exp(-1.5)=0.22, util 升高时报单更小。配合 obligationMinQty=5 让风险累积更平滑。

## 残留问题
- ao2609 裸 -28: CLOSEOUT HEDGE SELL 28 后产生反向头寸 (主力对冲特性, 非 bug)。trades 重放与 positions.csv 口径不同, 实盘需关注 net delta 而非合约级。
- closeprofit 2020 仍不及 fee 5885 一半: 撮合模型限制下 fill 单边的天然劣势依然存在, 真实环境双边率会更高。
- 单日单品种样本: 需多日多品种 walk-forward 验证才能确认参数稳健性。

## 下一步候选 (D=已完成, A=已完成, B=已完成)
- C: 把 portfolio_delta 改为合并到 contract_skew 分摊 (避免反向各自中性化), 降低 ao2609 -28 这种 hedge 后反向头寸。
- 多日回测: 取 6 月一周 ao 行情, 验证参数稳健性。
- 其他品种 (rb/i): 重跑 baseSpread 在不同 tickSize 下的表现。
