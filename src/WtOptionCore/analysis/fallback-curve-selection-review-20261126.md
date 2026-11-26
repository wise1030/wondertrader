# 曲线选择 vs Fallback 合理性核查（2026-11-26）

> 范围：`pricer.blackCalc.volCurve` 的 `type` / `curve2_type` / `volcurve_weight` /
> `fallback` 四个机制。仅分析，不改代码。结论基于当前源码 + 生产 config +
> 2026-11-26 全天日志与 UI/柜台交叉证据。

## 设计口径（2026-11-26 确认）

- **curve1（主曲线）**：`gvv` | `power`，由配置 `type` 选择；`weight=1` 时定价。
- **curve2（第二曲线）**：`linear`；`weight=0` 时定价，不需要 fallback。
- **`constant` 不准确**，不作为任何曲线的候选或兜底。
- 任一被权重要求的曲线拟合失败 → **不发布** → 冷启动保持未就绪，
  热态保留旧曲线；不换模型顶上。
- `weight` 语义：`0` → curve2(linear)；`1` → curve1；`0<w<1` 才混合。

> 该口径下，生产 `weight=0` 用 linear 定价是**设计意图**，不是配置错误。
> 当前实现的差距是：curve1 失败会阻断 curve2 发布（`AsyncVolFitter.h:203`）、
> curve2 失败会静默变成 constant（`:205-207`）、以及发布/ready 不区分权重。

## 结论

1. **"曲线类型由配置选择、不应 fallback"的原则成立**。fallback 在运行时
   静默换模型，破坏配置契约与可审计性，应移除或至少默认关闭。
2. **但 21:05 爆炸价不是 power fallback 直接造成的**。幽灵单全部是
   `cu2701`（202701）；21:05:38 的 fallback 日志只针对 **202704**，202701
   当次没有 fallback，且 `weight=0` 下由 curve2 linear 定价，却同样以约
   2.8–3 倍 vol 挂单。直接根因见 §3：**默认 ATM 尺度 0.1 与市场尺度
   ~0.24–0.30 的竞态**。
3. **生产 `volcurve_weight=0`，按设计由 curve2（linear）定价**，`type: gvv`
   的主曲线只是发布门控 + 可选平滑层。当前实现的问题是：curve1 拟合失败
   会阻断 curve2 发布，且 curve2 有 constant 静默兜底，与上述设计口径不符。
4. **只关 fallback 能挡住 202704，但挡不住 202701 这类"curve2 linear 成功
   但尺度错误"的曲线**。fallback 该移除，但必须同时修尺度竞态，否则事故
   会以 linear 单独复现。

## 1. 机制事实

| 机制 | 默认 | 生产值 | 代码位置 |
|---|---|---|---|
| `type`（主曲线） | `"gvv"` | `gvv` | `OptionPricer2.h:137`；`HftOptionStrategy.cpp:1263-1270` |
| `curve2_type`（第二曲线） | `"linear"` | 未配置→`linear` | `OptionPricer2.h:132`；`HftOptionStrategy.cpp:1277-1284` |
| `volcurve_weight`（混合权重） | `0.0` | 未配置→`0`（日志确认） | `OptionPricer2.h:171`；`CompositeOptionPricer.h:230` |
| `fallback`（主曲线失败链） | `true` | 未配置→`true` | `OptionPricer2.h:150`；`HftOptionStrategy.cpp:1285-1286` |

目标口径（已确认）：curve1 = `gvv|power`；curve2 = `linear`；`constant` 不用；
`weight=0` → linear 定价。

- 混合公式（`OptionPricer2.cpp:760-762`）：
  `theoVol = weight * 主曲线 + (1-weight) * curve2`
  → **weight=0 时 curve2 定价**，主曲线不参与 theo。
- fallback 链是硬编码的 `power → linear → constant(1.0)`
  （`AsyncVolFitter.h:185-202`），**不看 `type` 配置**；`m_fallback=true`
  为成员默认（`:230`）。
- fallback 成功即作为该 expiry 的主曲线发布；`publishFittedCurves` 同时
  置 `fitReady=true`（`OptionPricer2.cpp:259-291`），而 `computeValue`
  以 `isValuesReady()`（forwardReady && fitReady）为报价门控（`:742`）。
  所以 fallback 不是"内部参考曲线"，而是**直接解锁报价**。
- 当前 `type` 的合法值是 `gvv|linear|constant`（`IVolCurve.h:103-105`）；
  **`power` 不是合法主曲线**，只能作为 `curve2_type` 或 fallback 链成员。
  `HftOptionStrategy.cpp:1258` 的注释"both type and curve2_type accept
  gvv|power|linear|constant"与白名单不一致（注释过期）。按已确认口径，
  白名单应收敛为 curve1 `gvv|power`、curve2 `linear`。
- 关闭方式：配置 `fallback: false` 即可，但只在初始化读取
  （`HftOptionStrategy.cpp:1293-1295`），不是热参数，需重启。
- 当天 fallback 仅 5 次（202704 四次、202701 一次），全部集中在重启/数据
  断层后的"默认尺度窗口"（见 §3）；正常交易时段 GVV 拟合稳定。

## 2. 为什么 fallback 不合理

1. **破坏配置契约**：操作者配置 `type: gvv`，运行时却可能用 power/linear
   定价；出问题后无法从配置还原"当时用的什么模型"。
2. **"保证定价不中断"是伪需求**：拟合失败时 `fit()` 直接 `continue`
   （`AsyncVolFitter.h:203`），不发布新版本，`publishFittedCurves` 的
   版本门控（`f.version <= version` 跳过）会让**旧曲线继续生效**。
   热态本来就不会"没有曲线"；fallback 只在冷启动/新 expiry（version=0）
   产生差别——而那正是最不该放行的场景。
3. **冷启动时 fallback 让被拒数据也获得 ready**：21:05:37 `priced=0/150
   fwdNotReady` 证明门控按设计挡住了报价；21:05:38 若 202704 无 fallback，
   它应保持未就绪，但 fallback 把它包装成 v1。注意：cu2701 的爆炸价来自
   **202701 自己的 curve2 linear 发布**（同批数据、同一尺度错误），不是
   202704 的 fallback 曲线，也不是 202701 的 GVV 主曲线（weight=0 不参与
   theo）。fallback 的问题是"拆闸"，不是"制造 cu2701 价格"。
4. **fallback 成功还会为同一批数据的 curve2 发布背书**：主曲线成功是
   整个 expiry（含 curve2）发布的前提；GVV 拒单后 power 顶上，等于让
   同一批被 GVV 拒绝的数据继续流入定价链。

## 3. 关键修正：21:05 爆炸价的直接根因是 ATM 尺度竞态

### 机制

```
1. getExpiryInfo 初始化 m_atmvol = m_defaultVolatility = 0.1
   （OptionPricer2.cpp:176,236）
2. 21:05:38 首个 AsyncVolFitter snapshot 用 0.1 做归一化：
   points = IV / 0.1 ≈ 2.4–3.0（submitAsyncVolFit, HftOptionStrategy.cpp:4232/4303）
3. 202701 的 **curve2 linear** 拟合成功（points≈2.95，weight=0 时由它
   定价）；其 GVV 主曲线也成功（无 fallback 警告）但不参与 theo。
   202704 的 GVV 形状越界 → power fallback a=2.2972（同样与 202701 定价无关）
4. publishFittedCurves 把四个 expiry 都置 ready，并写回 atmVol=0.1
5. 下一批 compute：stillDefaultScale=true → bootstrap 把 atmVol
   seed 到市场 median ≈0.24–0.30（OptionPricer2.cpp:616-625）
   ——只改 atmVol，不重标定/不重拟合/不失效已发布曲线
6. theo = 归一化倍数(2.3–3.0) × 新 atmVol(0.24–0.30) ≈ 0.55–0.90 vol
   → 挂单价约为市场的 2.3–3 倍
7. 21:06:34 下一次拟合用已 seed 的 atmVol → a=1.0110 → theo 恢复正常
```

### 证据

| 时间 | 事件 | 数值 |
|---|---|---|
| 10:30:36 | 202704 fallback power | `a=2.4659` ≈ 0.2466/0.1 |
| 13:30:12 | 202701 / 202704 fallback power | `a=3.1116` / `a=2.4773` ≈ 0.311/0.1、0.248/0.1 |
| 21:05:38 | 202704 fallback power | `a=2.2972` ≈ 0.2297/0.1 |
| 21:05:38 | 四个 expiry 全部 `ready — vol fit published (curve v1)` | **202701 无 fallback 警告；weight=0 → 定价曲线是 curve2 linear** |
| 21:05:39-41 | 挂出 `cu2701` 爆炸价（P.90000=9404 vs 正常 3363，≈2.8x） | 202701 的 curve2 linear（0.1 尺度归一化约 2.95） |
| 21:06:34 | 同一 202704 第二次拟合（已 seed） | `a=1.0110` |
| 21:10:46 | ATM re-anchor 日志 | 202704 `0.2372→0.2434`（med=0.2434） |
| 当前 UI | 各月 atmVol | 202701 0.2948 / 202704 0.2394 |

- 5 次 fallback 中 4 次的 `a` 都 ≈ 对应月份"市场 ATM / 0.1"，第 5 次
  （21:06:34）在 seed 后为 1.0110——**不是随机拟合噪声，是归一化尺度**。
- 幽灵单（cu2701）的定价曲线是 **202701 的 curve2 linear**（weight=0），
  不是 power，也不是 202701 的 GVV 主曲线。21:05:38 的 power 属于 202704；
  把爆炸价归因于 power fallback 是错的。
- GVV 的 5 个保护分支约束的是**形状参数**（spotvol/rho/volvol/alpha），
  **不约束归一化水平**。尺度错误（0.1 vs 0.24）下任何模型都可能成功，
  weight=0 实际定价的 **linear 同样中招**；所以"某模型拟合成功"不等于
  "尺度正确"。

### 结论

- fallback 是 202704 的"放行者"，不是 cu2701 爆炸价的"制造者"。
- 即使 `fallback: false`，202701/202702/202703 的 curve2 linear 曲线仍会
  以错误尺度发布 → seed 后照样爆炸（weight=0 下它才是定价曲线）。
- 真正 P0 是：**发布前保证 snapshot atmVol 不是默认 0.1（bootstrap 已完成），
  且 atmVol 变更时曲线倍数必须同步重标定/重拟合**。

### 机制补充（2026-11-27 代码复核）：GVV 对锚变化不变，会爆炸的是"存归一化乘数"的曲线

- **GVV**：fit 在**绝对 vol 空间**（`vi = points·m_atmvol` = 绝对 IV），
  eval 返回 `vol_abs / m_atmvol`。锚从 0.1 变到 0.2948 后，eval 的分母
  同步变化，`eval × m_atmvol`（即定价用的绝对 vol）**不变** → GVV 曲线
  对 bootstrap reseed / re-anchor 天然免疫。
- **Linear / Power**：fit 直接存**归一化乘数**（IV / anchor_at_fit），
  eval 原样返回，定价再乘当前锚。锚一变、乘数不变 → theo 被动放大/缩小。
  21:05 的 202701（linear，乘数≈2.95）× 新锚 0.2948 ≈ 0.87 vol —— 这就是
  2.8 倍爆炸价的来源。
- 含义：**"重标定"只需作用于 linear/power**（实现为 `IVolCurve::rescale`，
  GVV 为 no-op）；而 L1 中位数门控对两类曲线都必要（0.1 尺度的点集
  median≈2.3–3.0，任何模型都不该再拟合并发布）。

## 4. 关闭 fallback 后的行为

| 场景 | 行为 | 是否符合"失败即未就绪" |
|---|---|---|
| 冷启动/新 expiry（version=0） | 不发布 → `fitReady=false` → 不算 theo、不报价 | ✅ 符合 |
| 热态（已有旧曲线） | `continue` 跳过 → 旧曲线 + `fitReady=true` 保持 | ⚠ 仍按旧曲线报价；不是严格"未就绪" |
| 21:05 事故重放 | 只挡住 202704；202701/2/3 的 curve2 linear 曲线仍发布并爆炸 | ❌ 不能根治 |

补充：curve2 自己还有 `ConstantVolCurve(1.0)` 静默兜底
（`AsyncVolFitter.h:205-207`）。生产 weight=0 时 curve2 就是定价曲线，
这个兜底同样绕过"配置选择"原则，且比主 fallback 更直接。

## 4.5 `weight=0` 是否仍要求主曲线成功？（已决策：不要求，去掉 L3）

**结论：去掉 L3 作为硬门控是合理的，且比保留更符合"曲线类型由配置选择"
的原则。但前提是 L1 与 linear 自身的质量检查必须真正落地；否则 L3 是当前
唯一能拦住"linear 能拟合垃圾数据"的层。**

### 为什么可以去掉 L3

1. **对 21:05 事故 L3 无效**：当时 202701 的 GVV **拟合成功**（无 fallback
   警告），真正的问题是默认 0.1 归一化，归一化点中位数 ≈2.3–3.0。
   L1 的中位数检查会直接拒绝提交，L3 不会。
2. **L3 是另一种隐式耦合**：`weight=0` 的定价模型是 linear，让一个不参与
   theo 的模型否决定价，和 fallback 一样属于"配置模型与实际模型不一致"。
3. **门控强度不可审计**：GVV 有 5 项形状保护，power 几乎没有，linear 也没有。
   L3 的保险强度随 `type` 变化，无法作为统一安全层。
4. **覆盖率成本**：202704 这类 GVV 形状越界但 linear 可拟合的月份会被无谓
   停报价，远月更明显；这是模型形式差异，不是数据质量差异。

### 去掉 L3 的前提

- L1 必须包含：atmVol 非默认且 snapshot 与当前一致；归一化点中位数
  ∈ `[0.7,1.3]`；点数/双象限/IV 有限且 >0；残差/离群检查。
- L2 中 **linear 必须有实质质量检查**（参数域、残差/R²、翼部不外推反转、
  归一化水平），不能"fit 返回 true 就算成功"。当前 `LinearVolCurve` 无任何
  检查——这是 L3 唯一的真实价值来源，必须转移到 L2。
- 主曲线仍要拟合、记录失败原因/连续失败计数并在 UI/日志可见（**观测，不阻断**）。

### 最终分层

```
L1 数据有效性（共享，模型无关；不通过则不提交/不发布）
L2 模型拟合
   - weight=0：linear 必须成功（主曲线仍拟合，仅观测）
   - weight=1：curve1(gvv/power) 必须成功
   - 0<w<1：两者都必须成功
L4 readiness（动态）
   - ready = L1 通过 && 当前 weight 实际使用的曲线可用且新鲜
   - 不使用"publish 时一次性置 fitReady"的静态写法
```

### readiness 必须动态化（去掉 L3 后的关键配套）

当前 `fitReady` 是单一布尔，`publishFittedCurves` 一旦发布就置 true
（`OptionPricer2.cpp:283-290`），而 `volcurve_weight` 是热参数：
`weight 0→1` 时不会重新判定，可能在 curve1 缺失或过期时仍 ready。建议：

- 每条曲线独立版本 + `lastSuccessTime`；`ready` 在定价/报价前按当前
  `weight` 计算，而不是发布时固化。
- `publishFittedCurves` 按曲线独立更新，不能无条件
  `ei->m_spVolCurve2 = f.volCurve2`（当前 `:267`；未来某条失败传 null 会
  清掉仍有效的定价曲线）。
- 版本号也要按曲线独立：当前 `m_curveVersion` 只按 expiry 记一个版本
  （`OptionPricer2.cpp:263`），linear-only 发布 bump 后，随后同版本的
  curve1 会被 `f.version <= version` 跳过。需 `{main, curve2}` 两个版本，
  或允许同一版本内按曲线合并。
- 当前使用的曲线超过 N 秒无成功拟合 → 不 ready（这替代 L3 的部分保护，
  且作用于真正参与定价的曲线）。

## 5. 建议（按优先级，均待确认后实施）

### P0 尺度一致性（先于 fallback）

1. `submitAsyncVolFit` 在 `atmVol == m_defaultVolatility` 时**不提交拟合**
   （或标记 bootstrap 状态），等 bootstrap seed 后再拟合。
2. `publishFittedCurves` 增加一致性检查：snapshot `atmVol` 与当前
   `ei->m_atmvol` 不一致（或为默认值）时丢弃该版本，不置 ready。
3. 若保留"发布后 seed"路径，seed 时必须**同步重标定已发布曲线**
   （乘以 old/new 比例）或强制版本失效并立即重拟合。
4. 回归测试：默认尺度首拟合不得发布/报价；seed 前后同一组点的绝对 IV
   必须连续。

### P0 共享数据门控（L1）

5. 在 `submitAsyncVolFit` / `fit()` 前增加模型无关的数据校验：
   `atmVol != 默认 0.1`、snapshot 与当前 `atmVol` 一致、归一化点中位数
   ∈ `[0.7, 1.3]`、点数/双象限/IV 有限/残差离群；不过则不提交、不发布。
6. 给 power/linear 补参数域/残差检查（GVV 已有 5 项）；GVV 的检查也需
   补尺度/水平项（当前只约束形状，挡不住 0.1 尺度）。

### P0 权重语义与发布解耦（按已确认口径）

7. 白名单收敛：curve1 = `gvv|power`；curve2 = `linear`；`constant` 不再
   作为候选（`isValidMainCurveType` / `isValidCurve2Type` 同步收紧）。
8. `VolFitExpirySnapshot` 增加有效 `weight`；`fit()` 规则：
   - `weight=0`：linear 必须成功；curve1 仍拟合但**仅观测**，失败不阻断；
   - `weight=1`：curve1(gvv/power) 必须成功；
   - `0<w<1`：两者都必须成功。
   weight=0 时 curve1 失败记录独立日志/计数（"curve1 unfit, linear pricing"），
   用于监控模型健康，不作为停报价条件。
9. `publishFittedCurves` 按曲线独立发布/独立版本；ready 在定价前按当前
   weight 动态判定（不使用静态 `fitReady` 固化），避免热切 weight 后
   用到缺失/过期的 curve1。
10. `computeValue` 只计算权重大于 0 的曲线，避免 `0*inf` 产生 NaN 误跳过
    定价；`FittedCurveSet` 允许单条曲线为空（只更新实际成功的曲线）。

### P1 fallback / constant 清理

11. 删除 curve1 fallback 链（`AsyncVolFitter.h:186-202`）、`volcurve_fallback`
    配置与 `m_fallback` 字段；失败不发布。
12. 删除 curve2 的 `ConstantVolCurve(1.0)` 兜底（`:205-207`）；linear 失败
    且 weight<1 时该 expiry 不发布、不置 ready。
13. 删除或在文档中明确废弃 `constant` 曲线类型（"不准确"，不再作为
    冷启动占位/兜底）。

### P1 热态失败策略

14. 连续 N 次拟合失败或曲线年龄超过 T 秒 → `setFitReady(false)`
    （带 hysteresis），避免旧 smile 无限期被当作有效曲线。

### P2 启动保护

15. 首版曲线发布后延迟 N 秒/要求连续 2 版一致再允许报价；重启保护
    不应只依赖"ready"。

## 6. 待决策

- L1 的具体阈值：归一化点中位数带宽（建议 `[0.7,1.3]`）、最少点数、
  残差/离群上限。
- 热态拟合失败时：保留旧曲线继续报价，还是立即降 ready 停报价？
  （建议与"当前权重实际使用的曲线"绑定：该曲线失败即降 ready。）
- `curve2_type` 是否彻底移除配置项、固定为 `linear`，还是保留字段但
  只接受 `linear`？
- `constant` 是删除类/枚举，还是保留为显式不可选的遗留实现？

## 7. 实施记录（2026-11-27，已实施并 9/9 测试通过）

以上方案已全部落地（含 §6 三项待决策的取值：curve2_type 保留字段但只接受
`linear`；constant 类删除；热态失败与"当前权重实际使用的曲线"绑定）。

### 机制层的两点实施细化（相对 §5 的偏差，均更安全）

1. **发布不做"anchor 不一致即丢弃"，改做"重定基"**：re-anchor 每
   `atm_reanchor_period` 步进 ≤`max_step`，可能恰好落在 submit→publish 之间；
   硬拒绝会造成发布反复失败。按 `snapshot_anchor / live_anchor` 重定基后
   theo 数学上不变（GVV 本就锚不变，linear/power 乘数缩放），错误尺度依旧
   进不来（L1 已在提交前拦截）。
2. **bootstrap seed 才重标定，re-anchor 不重标定**：前者是尺度纠正（市场
   没动，是我们估错了，`setATMVolReseed` 走 `rescale`）；后者是跟市场走
   （fit 间隔内 theo 应随锚移动，`setATMVol` 保持原语义）。二者语义不同，
   必须区分——统一重标定会让 re-anchor 对 linear/power 失效。

### 落地清单

| 层 | 实现 | 位置 |
|---|---|---|
| L1 数据门控 | 中位数 ∈`[0.7,1.3]`、≥4 点、双象限、有限正乘数、乘数 ∈`[0.02,20]`；不过则不拟合不发布 | `AsyncVolFitter::validateDataset` |
| L2 权重语义 | snapshot 带有效 `weight`；weight=0 只要求 linear（主曲线仍拟合、失败仅日志观测）；weight=1 只要求主曲线；混合两者都要 | `AsyncVolFitter::fit`、`HftOptionStrategy::submitAsyncVolFit` |
| L2 linear 质量 | 过滤非法点、≥3 点、y>0、翼部斜率 ≤5/单位 log-moneyness、`isLastFitOK`；空集不再越界（原 `front()` UB） | `LinearVolCurve::fit` |
| L2 power 质量 | ATM level `a+c∈[0.45,1.55]`、wing(±0.5)>0、max 残差 ≤0.4 | `PowerVolCurve::fit` |
| 发布解耦 | `FittedCurveSet.version/version2` 按曲线独立；null 不再覆盖旧曲线；anchor 重定基 | `OptionPricer2::publishFittedCurves` |
| L4 动态就绪 | 新 `ExpiryData::curvesReady`（默认 true，兼容旧测试/周期拟合路径）；ready = 当前权重所需曲线可用 && 发布次数 ≥`startup_confirm_versions` && 未超 `max_age`；publish/热改权重/每 compute pass 重算 | `OptionPricer2::refreshCurvesReady`、`hasRequiredCurves`、`setGvvWeight` |
| 尺度一致性 | bootstrap seed 走 `setATMVolReseed`（重定基）；re-anchor 保持原语义；`getAnyCurveVersion` 让 linear-only 发布也能离开 bootstrap 窗口、进入 re-anchor | `ExpiryInfo::setATMVReseed`、`initValuesCompute` |
| 权重短路 | `computeValue`/`__computeValue` 只计算权重大于 0 的曲线（0×inf NaN 防护） | `OptionPricer2.cpp` |
| fallback 清理 | 删除 fallback 链、`volcurve_fallback` 配置、`m_fallback`、curve2 constant 兜底；`ConstantVolCurve.h` 删除 | `AsyncVolFitter.h`、`HftOptionStrategy.cpp`、`ConfigValidator.h` |
| 白名单 | curve1 = `gvv|power`；curve2 = `linear`；constant 全面退出 | `IVolCurve.h`、`createVolCurveByType` |
| 观测 | UI 快照新增 `curvesReady`；主曲线失败（weight=0）有独立 INFO 计数日志 | `UiSnapshotView/Builder`、`AsyncVolFitter` |

### 新配置（`pricer.blackCalc.volCurve`）

| 键 | 默认 | 含义 |
|---|---|---|
| `startup_confirm_versions` | `2` | 新发布需连续 N 次被接受才解锁报价（重启保护；默认首版报价延迟一个拟合周期） |
| `max_age` | `900` | 距上次被接受的发布超过该秒数（compute 时钟）→ curvesReady=false，停止 theo 报价；0 关闭 |
| `fallback` | 已删除 | 配了会作为未知键被 ConfigValidator 警告 |

### 行为变化（生产视角）

- **重启后首版报价不再即时**：第一次发布只记账，第二次发布（默认约一个
  拟合周期 ~60s 后）才解锁。21:05 事故在该配置下重放：首个 0.1 尺度
  snapshot 被 L1 拒绝（median≈2.9）→ 无发布无报价；bootstrap seed 后第二
  版 median≈1.0 正常发布，第三次发布解锁。
- **热改 `volcurve_weight` 立即重判就绪**：0→1 而主曲线从未拟合成功 → 该月
  即刻停止 theo 报价（日志 `curves not ready`），改回即恢复。
- 21:05 的 cu2701 爆炸价路径被 L1 与 reseed 重定基双重封死；GVV 路径
  （weight=1）即使发布也不受锚变化影响（锚不变性）。

### 验证

- `src/WtOptionCore` 全量重编通过；**9/9 测试二进制通过**
  （test_enhancements / scanners / stress / hedge_coordinator / ctg_ranking /
  uibridge / command_handlers / async_event_processor / replay_benchmark）。
- 新增 4 个回归：L1 尺度门（0.1-vs-0.25 毒数据不得发布）、权重语义
  （linear-only 发布 + 热改权重降就绪/恢复）、启动确认 + max-age 新鲜度、
  reseed 重定基不变性 / re-anchor 跟随性。
- 部署：`.so` 已构建于 `/root/build/wtn_rel2/WtOptionCore`，**尚未部署到
  生产**（需择机重启进程）。
