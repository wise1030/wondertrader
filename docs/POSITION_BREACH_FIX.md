# POSITION_BREACH 根因与修复 — UftMocker 平今撮合穿仓

> 2026-06-20 | Phase 4 POSITION_BREACH 诊断
>
> 根因: UftMocker(WonderTrader 回测框架) on_trade 平今/平昨撮合记账缺陷
>
> 状态: **已修复,验证中**

---

## 一、现象

EC 5 天回测(e2607/ec2608/ec2609):
- POSITION_BREACH 触发 **231,751 次**
- ec2608 持仓被 CLOSE_SHORT 平到负数(short: 5→0→-1→-5→-9→-13→-16→-20...)
- adverse=0.5772(>50% 逆向选择)
- day2-5 实际交易近乎停摆(day1 持仓击穿后策略卡死)

---

## 二、根因(UftMocker 框架级 bug,非策略层)

### BUG-M1: 平今撮合不防穿仓

UftMocker.cpp on_trade() offset==2(平今)段:
```cpp
// 原代码 (L1286):
pItem._newvol -= qty;  // 无条件扣减,不检查 qty <= _newvol
```

多笔 CLOSET 订单(stra_buy 拆出的 exit_short 子单)在价格满足时同时撮合。
procOrder 逐个处理 _orders,每个 CLOSET 都按其 qty 扣减 _newvol,
**不检查剩余持仓是否够平** → 5 手空头被 4 笔各 5 手的 CLOSET 平到 -15。

procOrder(L921-989)只管"价格满足就成交",不检查持仓量。
持仓校验应在 on_trade 记账层做,但原代码没有。

### BUG-M2: 平今成交不扣减 _newavail

UftMocker.cpp on_trade() offset==0(开仓)有:
```cpp
pItem._newavail += qty;  // L1217, 开仓增加可用量
```

但 offset==2(平今)**只扣 _newvol,不扣 _newavail**。
导致 valid() = _preavail + _newavail 虚高。
stra_exit_short(L780)的 `valid >= qty` 校验放行超量平仓。

### 暴走链条(已用 Python 最小复现验证)

```
Tick 1: open short 5 → _newvol=5, _newavail=5
Tick 2: stra_buy(5) → exit_short(5) + enter_long(0)
        exit_short: _newavail=0, CLOSET_A(5) 进队列
        open short 5 成交: _newvol=10, _newavail=5
Tick 3: cancel CLOSET_A → _newavail += 5 = 5
        stra_buy(5) → exit_short(5), CLOSET_B(5) 进队列
        open short 5 成交: _newvol=15, _newavail=5
...每 tick 重复, CLOSET 订单积累在 _orders 里...

暴走时刻: 价格满足, N 笔 CLOSET 同时撮合:
  CLOSET_1(5): _newvol -= 5 → 0    ← BUG-M1 不防穿仓
  CLOSET_2(5): _newvol -= 5 → -5   ← 穿仓!
  CLOSET_3(5): _newvol -= 5 → -10
  CLOSET_4(5): _newvol -= 5 → -15  ← 持仓变负数
```

### 为什么 ao 不暴走 EC 暴走

ao bidqty 小(1-2手),即使多笔 CLOSET 叠加,总量小,procOrder 的 splitVolume
限制了单笔成交量。EC bidqty 大(4-10手),CLOSET 订单量大,穿仓幅度大。

---

## 三、修复

### 文件: src/WtBtCore/UftMocker.cpp

#### offset==2(平今)段:

```cpp
// BUG-M1 fix: 截断到实际今仓量, 防止穿仓
double actualQty = std::min(qty, pItem._newvol);
if (decimal::lt(actualQty, qty))
    log_error("CloseToday position overflow: requested {} but newvol={}, clamped to {}",
              qty, pItem._newvol, actualQty);
qty = actualQty;

// BUG-M2 fix: 平今成交同步扣减 _newavail
double availToDeduct = std::min(qty, pItem._newavail);
pItem._newavail -= availToDeduct;

pItem._newvol -= qty;  // 原行,现在 qty 已截断
```

#### offset==1(平昨)段:

```cpp
// BUG-M1 fix: 截断到实际持仓量
double totalVol = pItem.volume();
if (decimal::gt(qty, totalVol)) {
    log_error("Close position overflow: requested {} but totalvol={}, clamped to {}",
              qty, totalVol, totalVol);
    qty = totalVol;
}

// BUG-M2 fix: 平仓同步扣减可用量
double availPreDeduct = std::min(maxQty, pItem._preavail);
pItem._preavail -= availPreDeduct;
double availNewDeduct = std::min(qty - maxQty, pItem._newavail);
pItem._newavail -= availNewDeduct;
```

---

## 四、单日验证结果(ec_d1_dbg, 6/08)

| 指标 | 修复前 | 修复后 |
|------|--------|--------|
| POSITION_BREACH | 73,950(单日) | **0** |
| 持仓穿仓(to 负数) | 多次 | **0** |
| 持仓变化 | CLOSET→-1→-5→-9... | CLOSET 5→0 ✓ 正常 |

修复有效。overflow 日志(CloseToday position overflow)仍有 ~44,885 次,
是策略层反复在持仓已平完(newvol=0)时下 1 手平仓单,被 mocker 截断为 0。
这是策略层的独立问题(closeout 重试逻辑),不影响 mocker 修复的正确性。

---

## 五、备注

- 这是 WonderTrader 框架级 bug,影响所有用 UftMocker + match_this_tick 的回测
- ao 回测不暴走是因为 bidqty 小,不是因为 bug 不存在
- 此修复应该反馈给 WonderTrader 上游(如果用 fork 的话,在 fork 分支修)
- BUG-M1/M2 修复后,策略层仍有"反复超量下单"的次级问题(closeout 重试),
  需要单独排查
