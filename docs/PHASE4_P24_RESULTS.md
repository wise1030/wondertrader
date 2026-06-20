# P2-4 SpreadOptimizer 调参结果

> 2026-06-18 | Phase 4 P2-4
>
> 任务: 回测 toxicity_spread_factor ∈ {1.0,1.2,1.5,2.0},对比 adverse selection 改善
>
> 标的: _ec_5d.yaml (ec2607/ec2608/ec2609, 6/08-6/12 五日)
>
> 工具: dist/WtBtFutu/p24_sweep.sh + logcfg_p24.yaml

---

## 一、结果表

| TSF | adverse | capture% | fill% | trades |
|-----|---------|----------|-------|--------|
| 1.0 | 0.5772 | 136.82 | 52.55 | 165 |
| 1.2 | 0.5772 | 136.82 | 52.55 | 165 |
| 1.5 | 0.5772 | 136.82 | 52.55 | 165 |
| 2.0 | 0.5772 | 136.82 | 52.55 | 165 |

**4 组完全一致** — toxicity_spread_factor 调了等于没调。

---

## 二、根因(确定)

`tox_events=0`,is_toxic 全程 false。

SpreadOptimizer.cpp:44:
```cpp
double tox_mult = 1.0 + ctx.toxicity.toxicity_score * _params.toxicity_spread_factor;
```

toxicity_score ≈ 0 → tox_mult = 1.0 + 0×factor = 1.0,无论 factor 多大。

→ **toxicity_spread_factor 在此 EC 场景是 dead parameter**。这段行情毒性检测器从未触发,"毒性扩 spread"逻辑完全没激活。

P2-4 的命题"调 toxicity_spread_factor 改善 adverse"在此数据上**不成立**。

---

## 三、基线暴露的更严重问题(回测健康度)

P2-4 没改善 adverse,但跑出来的基线暴露了 5 个更优先的问题:

| # | 现象 | 严重度 |
|---|------|--------|
| 1 | POSITION_BREACH **231,751 次** — 持仓反复击穿 -60 (max 50) | 🔴 |
| 2 | ec2609 零成交,ec2607/ec2608 各 ~80 笔 | 🔴 |
| 3 | BILATERAL_STATS 三合约 samples=0(useBilateralQuote=false,双边统计未工作) | 🟡 |
| 4 | adverse=0.5772(>50% 成交是逆向选择) | 🔴 |
| 5 | session 6/09~6/12 PERF 完全相同(493.75/334/165),疑数据重放或击穿后退化 | 🟡 |
| 6 | avg_inv=0.0 / turnover=0.00 库存统计未生效 | 🟢 |

---

## 四、对 Phase 4 路线的影响

### 结论

P2-4 验证了 PHASE4_PLAN 的判断"需要先有量化基线",基线拿到了,但它告诉我们:
**toxicity_spread_factor 不是当前瓶颈**。当前 EC 做市的真问题是持仓击穿 + 高 adverse。

### adverse=0.5772 与 P1-8 的关系

P1-8(跨期同步报价组)的目标就是降 adverse。基线 adverse=0.5772 极高,
而且 BREACH 23 万次说明:**ec2607/08/09 三条腿的报价/持仓完全没协调**,
这恰恰是 P1-8 要解决的"非同步报价"问题的极端表现。

→ P1-8 的价值被这个基线**强化**了:adverse 这么高,正是因为跨期腿各刷各的、
   持仓在三个合约间失控。

### 但是先决条件

POSITION_BREACH 231,751 次说明在做 P1-8 之前,有一个更基础的问题:
**持仓控制为什么完全失效**。可能原因:
- maxPosition=50 但 hedge/skew 没能压住,持仓冲到 -60
- 三合约 delta 没有合并管理(又是 per-contract vs 全局问题)
- ec2609 零成交说明报价根本没在 ec2609 上有效挂出

这个比 P1-8 还基础,可能要先查。

---

## 五、待奶酪拍板的问题

### Q-R1: P2-4 结论是否接受"toxicity_spread_factor 在 EC 无效"?
- (a) 接受,P2-4 关闭,该参数保持默认 1.0(我倾向)
- (b) 换标的重测(ao 或有毒性触发的行情段)看是否真无效
- (c) 先修毒性检测器为何在 EC 不触发(可能是 EC tick 频率低导致 VPIN 不积累)

### Q-R2: 下一步方向?
- (a) 先查 POSITION_BREACH ×23 万的根因(持仓控制失效,比 P1-8 更基础)— 我倾向
- (b) 直接做 P1-8(假设同步报价能压住持仓击穿)
- (c) 先查 ec2609 零成交 + session 重复(回测数据/配置问题)

### Q-R3: 这个 EC baseline 本身可信吗?
- session 6/09-6/12 PERF 完全相同很可疑
- (a) 先确认 EC 5 天数据是否真有 4 天独立行情(查 storage)
- (b) 不管,用单日 EC 重测
- 我倾向 (a),数据可信度是一切的前提

---

## 六、交付物

- dist/WtBtFutu/p24_sweep.sh — 可复用的参数 sweep 脚本
- dist/WtBtFutu/logcfg_p24.yaml — info 级日志配置(回测加速)
- dist/WtBtFutu/p24_results/ — 4 变体 yaml + PERF 抽取日志
- 本报告

---

## 七、备注

- P2-4 名义上"完成"了(跑完 4 组对比),但结论是负面的:该参数在当前场景无效
- 真正有价值的产出是**基线数据**,它把下一步指向了"持仓控制 + adverse"而非"毒性 spread"
- toxicity_spread_factor dead 的发现本身是有用的:避免后续在无效参数上浪费调参时间
