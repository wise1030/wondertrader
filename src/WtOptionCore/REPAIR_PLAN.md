# WtOptionCore 修复完善计划 (V3)

> 基于源码逐条验证，对照 quantbox 三层继承架构（OptionGrid ← OptionTradingGrid ← ControllableTradingGrid）。
> V3 核心修正：引入 OptionTradingGrid 中间层，补齐三层架构。
> 设计原则：组合优于继承（适配 WT 框架，不用 longbeach 继承体系）。

## 总体原则

1. **三层架构对齐 quantbox**：OptionGrid(数据) → OptionTradingGrid(交易对象) → ControllableTradingGrid(执行控制)
2. **组合优于继承**：CTG 持有 OTG 指针（不继承），通过 OTG 访问 OTD/UTD/ETD
3. **功能不简化**：完整实现 quantbox optiontrader 所有业务功能
4. **回测=实盘**：所有修复在回测中验证，不用 fallback
5. **逐层验证**：每阶段完成后编译+回测

## 架构对比

### quantbox 三层（继承链）
```
OptionGrid (1445行)              ← 行情接收+定价调度+合约发现
  ↑ 继承
OptionTradingGrid (714行)        ← 创建OTD/UTD/ETD+OptionRisk+OM Factory
  ↑ 继承
ControllableTradingGrid (1133行) ← combineMarkets+refresh+rank+updateOrders+onFill
```

### WtOptionCore 目标架构（组合）
```
OptionGrid (686行)               ← 已有，已接线
  ↑ 持有(指针)
OptionTradingGrid (新建~400行)   ← 新建中间层
  │ 持有: OptionRisk + OTD/UTD/ETD 表
  │ 方法: onAddOption→创建OTD, __createExpiryTradingData→创建ETD+UTD
  │       computeValues(委托grid), getTradingData, getUnderlyingTradingData
  ↑ 持有(指针)
ControllableTradingGrid (494行)  ← 已有，改走 OTG
  │ 持有: OTG 指针 + OptionTraderContext
  │ 方法: combineMarkets+refresh+rank+drainPendingQuotes
  │ 新增: onFillWithFees+onOptionHit+onSetQMode+tradingStopMidDay
```

### WtOptionCore 当前（缺中间层）
```
OptionGrid (686行)               ← 已有
ControllableTradingGrid (494行)  ← 直接持有 Grid，绕过 OTG
UftOptionStrategy                ← 直接操作 Grid+CTG+Pricer
```

## 阶段划分

| 阶段 | 目标 | 工作量 | 验证标准 |
|---|---|---|---|
| Phase 1 | 引入 OptionTradingGrid 中间层 | 大 | OTG 创建, onAddOption→OTD, ETD/UTD 表 |
| Phase 2 | per-contract OptionQuoteManager | 大 | 每合约独立OM, 订单生命周期, is_crossed |
| Phase 3 | OptionRisk 接线 + 风控闭环 | 小 | OptionRisk 实例化, update() 被调 |
| Phase 4 | CTG 改走 OTG + combineMarkets 激活 | 中 | combineMarkets 非 no-op, OTD->multiMarket |
| Phase 5 | 防自成交 + onFill 回调闭环 | 中 | STP 生效, on_trade→onFill→deactivate |
| Phase 6 | 参数配置统一 + alpha 激活 | 中 | wgt_* 从 config, alpha 非 0 |
| Phase 7 | QuoteMode + rankOption6因子 + 标的报价 | 中 | CLOSE/FLAT, rank 6因子, UTD 接线 |
| Phase 8 | Scanner + 运维功能 | 中 | onOptionHit, onSetQMode, tradingStop |
| Phase 9 | 清理 + 回归验证 | 小 | 0 cout, 回测全链路 |

---

## Phase 1: 引入 OptionTradingGrid 中间层

**核心**：补齐 quantbox 三层架构的中间层。

### 1.1 新建 OptionTradingGrid.h/.cpp

对标 quantbox OptionTradingGrid.h(184行)/.cc(530行)，用 WT 类型替换 longbeach。

```cpp
// OptionTradingGrid.h
namespace wt_option {

class OptionTradingGrid {
public:
    OptionTradingGrid(OptionGridPtr grid);
    
    // --- OTG 职责: 创建交易对象 ---
    void onAddOption(const OptionDataPtr& od);      // 创建 OTD + 绑定
    void onAddExpiry(const ExpiryDataPtr& ed);       // 创建 ETD + UTD
    
    // --- 访问器 ---
    OptionTradingDataPtr getTradingData(const std::string& code) const;
    OptionTradingDataPtr getTradingData(uint32_t exp, strike_t stk, OptionRight right) const;
    UnderlyingTradingDataPtr getUnderlyingTradingData(const std::string& code) const;
    UnderlyingTradingDataPtr getFrontMonthTradingData() const;
    ExpiryTradingDataPtr getExpiryTradingData(uint32_t exp) const;
    
    // --- 委托 OptionGrid ---
    void computeValues(IOptionPricer* pricer);
    OptionGridPtr getOptionGrid() const { return m_spGrid; }
    
    // --- OptionRisk ---
    const OptionRiskPtr& getPositionRisk() const { return m_spPositionRisk; }
    void setPositionRisk(OptionRiskPtr risk) { m_spPositionRisk = risk; }
    
    // --- GridListener 转发 ---
    // OTG 注册为 Grid 的 listener, onAddOption/onAddExpiry/onComputeValuesCompleted
    
private:
    OptionGridPtr m_spGrid;
    OptionRiskPtr m_spPositionRisk;
    
    // 交易对象表
    // OTD 存在 OptionData::m_tradingData 里 (Phase 1.2 新增)
    typedef std::map<std::string, UnderlyingTradingDataPtr> UnderlyingTable;
    UnderlyingTable m_tblUnderlyingTradingData;
    typedef std::map<uint32_t, ExpiryTradingDataPtr> ExpiryTable;
    ExpiryTable m_tblExpiryTradingData;
    UnderlyingTradingDataPtr m_spFrontMonthTradingData;
    
    // executor hooks (从策略传入, OTD 创建时绑定)
    QuoteExecutor m_quoteExec;
    OrderExecutor m_orderExec;
    CancelExecutor m_cancelExec;
    PositionProvider m_positionProvider;
    
    ExpiryTradingDataPtr __createExpiryTradingData(const ExpiryDataPtr& ed);
};

using OptionTradingGridPtr = std::shared_ptr<OptionTradingGrid>;

} // namespace wt_option
```

### 1.2 OptionData 增加 OTD 持有

```cpp
// OptionData.h 新增:
private:
    OptionTradingDataPtr m_tradingData;
public:
    void setTradingData(OptionTradingDataPtr otd) { m_tradingData = otd; }
    OptionTradingDataPtr getTradingData() const { return m_tradingData; }
```

### 1.3 OTG::onAddOption 实现

对标 quantbox OTG::onAddOption (L250-289)：

```cpp
void OptionTradingGrid::onAddOption(const OptionDataPtr& od) {
    if (!od || od->getTradingData()) return;  // 已创建
    
    // 1. 创建 OTD
    auto otd = std::make_shared<OptionTradingData>();
    otd->init(od);
    od->setTradingData(otd);
    
    // 2. 绑定 RiskData
    if (m_spPositionRisk) {
        auto rd = m_spPositionRisk->get(od->getCode());
        // otd 绑定 rd (通过 setPositionProvider)
    }
    
    // 3. 绑定 executors
    otd->setQuoteExecutor(m_quoteExec);
    otd->setOrderExecutor(m_orderExec);
    otd->setCancelExecutor(m_cancelExec);
    
    // 4. 创建/获取 ETD
    getExpiryTradingData(od->getExpiry());
    
    // 5. enable + setActive(false) (等 channel_ready)
    otd->enable();
    otd->setActive(false);
}
```

### 1.4 OTG::__createExpiryTradingData 实现

对标 quantbox OTG::__createExpiryTradingData (L305-330)：

```cpp
ExpiryTradingDataPtr OptionTradingGrid::__createExpiryTradingData(const ExpiryDataPtr& ed) {
    auto etd = std::make_shared<ExpiryTradingData>();
    
    // 创建标的 UTD
    auto utd = std::make_shared<UnderlyingTradingData>(ed->getHedgeCode(), ed->getExpiry());
    etd->setPrimaryUnderlier(utd);
    m_tblUnderlyingTradingData[ed->getHedgeCode()] = utd;
    
    // 绑定到期 Greeks
    if (m_spPositionRisk) {
        auto egreeks = m_spPositionRisk->getExpiryGreeks(ed->getExpiry());
        etd->setExpiryGreeks(egreeks);
    }
    
    return etd;
}
```

### 1.5 OTG 注册为 Grid Listener

```cpp
// UftOptionStrategy::on_init:
_grid->addListener(_otg.get());  // OTG 收到 onAddOption/onAddExpiry/onComputeValuesCompleted
```

OTG 收到 onAddOption → 创建 OTD/ETD/UTD。
OTG 收到 onComputeValuesCompleted → 转发给 OptionRisk + CTG。

### 1.6 UftOptionStrategy 改用 OTG

```cpp
// 当前:
_grid = make_shared<OptionGrid>(...);
_grid->addListener(_pricer.get());
_ctg = make_shared<CTG>(_grid, _traderCtx);

// 改为:
_grid = make_shared<OptionGrid>(...);
_otg = make_shared<OptionTradingGrid>(_grid);
_grid->addListener(_otg.get());      // OTG 收到 grid 事件
_grid->addListener(_pricer.get());   // Pricer 也收到
_ctg = make_shared<CTG>(_otg, _traderCtx);  // CTG 通过 OTG 访问一切
```

### 1.7 CTG 持有 OTG 指针（组合替代继承）

```cpp
// ControllableTradingGrid.h:
private:
    OptionTradingGridPtr m_otg;  // 替代 m_grid
public:
    ControllableTradingGrid(OptionTradingGridPtr otg, OptionTraderContextPtr ctx);
    // 通过 m_otg->getOptionGrid() 访问 grid
    // 通过 m_otg->getTradingData(code) 访问 OTD
```

### 1.8 验证

- OTG 实例存在
- onAddOption 创建 OTD：`od->getTradingData() != nullptr`
- ETD 表有数据：`m_tblExpiryTradingData.size() > 0`
- UTD 表有数据：`m_tblUnderlyingTradingData.size() > 0`
- 编译通过，回测无 segfault

---

## Phase 2: per-contract OptionQuoteManager

**核心**：quantbox 每个合约有独立的 QuoteOrderManager（订单生命周期管理）。WtOptionCore 当前用 executor lambda 替代，丢失了订单追踪/精确撤单/防自成交/交易所分支/GetFlat 等全部功能。

**原则**：不简化功能。完整迁移 QuoteOrderManager 的订单生命周期管理，用 WT 的 stra_quote/stra_cancel/stra_buy/sell/on_order/on_trade 作为底层 API。

### 2.1 新建 OptionQuoteManager.h/.cpp

对标 quantbox QuoteOrderManager(760行) + DefaultOrderManager(1607行) 的核心业务逻辑。

```cpp
// OptionQuoteManager.h
namespace wt_option {

class OptionQuoteManager {
public:
    struct Config {
        uint32_t max_orders = 1;           // max orders per side
        uint32_t max_position = 1;
        double time_in_force_ms = 45000;   // order TTL
        bool enable_quote_api = false;     // 交易所报价API vs 限价单
        bool avoid_trade = false;          // 避免主动成交
        bool check_potential_position = false;
        std::string exchange;              // SHFE/CZCE/DCE/CFFEX/INE
        double tick_size = 0.5;
    };

    OptionQuoteManager(const std::string& code, const Config& cfg,
                       IUftStraCtx* ctx);

    // --- 核心接口 (对标 IOrderManager) ---
    int32_t updateOrders(const MultiMarket& desired, bool cancel_only = false);
    int32_t updateQuoteOrders(const MultiMarket& desired);
    void setPosition(int32_t pos) { m_position = pos; }

    // --- 回调 (从策略 on_order/on_trade 转发) ---
    void onOrderStatusChange(uint32_t localid, bool isBuy,
                              double totalQty, double leftQty,
                              double price, bool isCanceled);
    void onFill(uint32_t localid, bool isBuy, double fill_px, uint32_t fill_qty);

    // --- 查询 ---
    bool isActive() const { return m_active; }
    void setActive(bool b) { m_active = b; }
    const MultiMarket& getCurrentMarket() const { return m_orderMarketTracker; }
    const MultiMarket& getLastDesiredMarket() const { return m_lastDesired; }
    int32_t getPosition() const { return m_position; }
    int32_t getNumCancel() const { return m_numCancel; }
    int32_t getNumReject() const { return m_numReject; }
    int32_t getNumFill() const { return m_numFill; }

private:
    // --- WT API (通过 IUftStraCtx) ---
    uint32_t sendQuote(double bidP, uint32_t bidQ, double askP, uint32_t askQ);
    uint32_t sendLimitOrder(bool isBuy, double price, uint32_t qty);
    bool sendCancel(uint32_t localid);
    bool sendCancelAll();

    // --- 撤单 (精确) ---
    int32_t cancelAll(int dir = -1);  // -1=both, 0=buy, 1=sell
    int32_t cancelByPrice(int dir, double price);
    int32_t cancelById(uint32_t localid);

    // --- 防自成交 ---
    bool is_crossed(double bidP, double askP) const { return bidP >= askP; }

    // --- 交易所分支 ---
    bool isQuoteApiExchange() const;  // SHFE/CZCE/INE = 顶单, DCE/CFFEX = 报价API
    void cancelAndResend(const MultiMarket& desired);

    // --- per-contract 状态 ---
    std::string m_code;
    Config m_cfg;
    IUftStraCtx* m_ctx;
    bool m_active = false;
    int32_t m_position = 0;
    int32_t m_numCancel = 0, m_numReject = 0, m_numFill = 0;

    // 订单追踪
    struct OrderState {
        uint32_t localid = 0;
        bool isBuy = false;
        double price = 0;
        uint32_t qty = 0;
        uint32_t filled = 0;
        bool active = false;
        bool cancelPending = false;
        uint64_t issueTimeMs = 0;
    };
    std::vector<OrderState> m_bidOrders;  // 买单列表
    std::vector<OrderState> m_askOrders;  // 卖单列表

    MultiMarket m_orderMarketTracker;  // 实际挂单 (从回报更新)
    MultiMarket m_lastDesired;          // 上次 desired
};

using OptionQuoteManagerPtr = std::shared_ptr<OptionQuoteManager>;

} // namespace wt_option
```

### 2.2 updateQuoteOrders 核心逻辑

对标 quantbox QuoteOrderManager::updateQuoteOrders (L595-700)：

```cpp
int32_t OptionQuoteManager::updateQuoteOrders(const MultiMarket& desired) {
    // 1. desired == current → 不操作
    if (desired == m_orderMarketTracker) {
        if (!m_cfg.avoid_trade) return 0;
    }

    // 2. 防自成交检查
    MarketLevel mkt = desired.getBest();
    if (is_crossed(mkt.getBid().px(), mkt.getAsk().px())) {
        WTSLogger::log_by_cat("strategy", LL_WARN,
            "STP: {} bid {} >= ask {}", m_code, mkt.getBid().px(), mkt.getAsk().px());
        return 0;
    }

    // 3. 撤旧单 (交易所分支)
    if (isQuoteApiExchange()) {
        // SHFE/CZCE/INE: 顶单 → 直接更新(stra_quote 覆盖)
        // WT stra_quote 会自动替换旧单
    } else {
        // DCE/CFFEX: 先撤后发
        cancelAll(0);  // 撤买单
        cancelAll(1);  // 撤卖单
    }

    // 4. 持仓检查 (max_position)
    int32_t potential = m_position;
    // ... 计算潜在持仓 ...

    // 5. 发新单
    uint32_t bidQ = static_cast<uint32_t>(mkt.getBid().sz());
    uint32_t askQ = static_cast<uint32_t>(mkt.getAsk().sz());
    if (bidQ > 0 && askQ > 0) {
        sendQuote(mkt.getBid().px(), bidQ, mkt.getAsk().px(), askQ);
    } else if (bidQ > 0) {
        sendLimitOrder(true, mkt.getBid().px(), bidQ);
    } else if (askQ > 0) {
        sendLimitOrder(false, mkt.getAsk().px(), askQ);
    }

    m_lastDesired = desired;
    return 1;
}
```

### 2.3 onOrderStatusChange / onFill 回调

对标 quantbox QuoteOrderManager::onOrderStatusChange/onFill：

```cpp
void OptionQuoteManager::onOrderStatusChange(uint32_t localid, bool isBuy,
    double totalQty, double leftQty, double price, bool isCanceled)
{
    auto& orders = isBuy ? m_bidOrders : m_askOrders;
    for (auto& o : orders) {
        if (o.localid == localid) {
            if (isCanceled || leftQty == 0) {
                o.active = false;
                o.cancelPending = false;
                m_numCancel++;
            }
            break;
        }
    }
    // 重建 m_orderMarketTracker
    rebuildOrderMarketTracker();
}

void OptionQuoteManager::onFill(uint32_t localid, bool isBuy,
    double fill_px, uint32_t fill_qty)
{
    // 更新持仓
    m_position += (isBuy ? 1 : -1) * static_cast<int32_t>(fill_qty);
    m_numFill++;
    // 标记订单成交量
    // ...
}
```

### 2.4 OptionTradingData 持有 OptionQuoteManager

```cpp
// OptionTradingData.h:
private:
    OptionQuoteManagerPtr m_quoteOM;  // 替代 executor lambdas
public:
    void setQuoteManager(OptionQuoteManagerPtr om) { m_quoteOM = om; }
    int32_t updateOrders(bool cancel_only) {
        if (m_quoteOM)
            return m_quoteOM->updateOrders(m_multiMarket, cancel_only);
        return 0;
    }
```

### 2.5 OTG::onAddOption 创建 OM

```cpp
// OTG::onAddOption:
OptionQuoteManager::Config omCfg;
omCfg.exchange = m_exchange;
omCfg.tick_size = od->getTickSize();
omCfg.enable_quote_api = (m_exchange == "SHFE" || m_exchange == "CZCE" || m_exchange == "INE");
auto om = std::make_shared<OptionQuoteManager>(od->getCode(), omCfg, m_ctx);
otd->setQuoteManager(om);
```

### 2.6 策略回调转发到 OM

```cpp
// UftOptionStrategy::on_order:
auto otd = _otg->getTradingData(stdCode);
if (otd && otd->getQuoteManager()) {
    otd->getQuoteManager()->onOrderStatusChange(localid, isBuy, totalQty, leftQty, price, isCanceled);
}

// UftOptionStrategy::on_trade:
auto otd = _otg->getTradingData(stdCode);
if (otd && otd->getQuoteManager()) {
    otd->getQuoteManager()->onFill(localid, isBuy, price, vol);
}
```

### 2.7 功能对齐清单

| quantbox 功能 | OptionQuoteManager | 实现 |
|---|---|---|
| m_orderMarketTracker | m_bidOrders/m_askOrders + rebuildTracker | ✅ |
| is_crossed 防自成交 | is_crossed(bidP, askP) | ✅ |
| cancelAll/cancelByPrice/cancelById | cancelAll/cancelById | ✅ |
| 交易所分支 (SHFE顶单 vs DCE报价API) | isQuoteApiExchange() | ✅ |
| onOrderStatusChange 回调 | onOrderStatusChange | ✅ |
| onFill 回调 | onFill + 持仓更新 | ✅ |
| max_position 检查 | potential position 计算 | ✅ |
| avoid_trade | avoid_trade 配置 | ✅ |
| getNumCancel/getNumReject/getNumFill | 计数器 | ✅ |
| updateQuoteOrders (desired→撤旧→发新) | 完整实现 | ✅ |
| time_in_force (TTL) | issueTimeMs + 定时撤 | ✅ |
| 今昨仓拆分 (issuePrev/Today/Open) | WT ActionPolicy 自动拆 | ✅ WT替代 |
| GetFlat 强平模式 | 后续 Phase 8 | ⚠️ 预留 |

### 2.8 验证

- 每个期权有独立 OM: `otd->getQuoteManager() != nullptr`
- is_crossed: bid >= ask 被跳过
- onOrderStatusChange: 撤单回报更新 m_bidOrders/m_askOrders
- onFill: 成交回报更新 m_position + m_numFill
- m_orderMarketTracker 从回报重建（非乐观更新）
- 交易所分支: SHFE 走 stra_quote 覆盖, DCE 先撤后发
- 回测无 segfault

**问题**：OptionRisk 代码完整(307行)但传 nullptr。

### 2.1 创建 OptionRisk

```cpp
// UftOptionStrategy::setupPricer:
_risk = std::make_shared<OptionRisk>(_grid);
_otg->setPositionRisk(_risk);   // OTG 持有
_grid->addListener(_risk.get()); // Risk 收到 onComputeValuesCompleted
_pricer = std::make_shared<CompositeOptionPricer>(copCfg, _grid.get(), _risk.get());
```

### 2.2 OptionRisk 自动创建 RiskData

- OptionRisk 是 GridListener → onAddOption → createOptionRiskData(od) 自动触发
- update() 聚合 Greeks → totalDelta/getExpiryGreeks

### 2.3 标的对冲注册

```cpp
// UftOptionStrategy::on_init:
_risk->registerHedgeInstrument(_underlyingCode, 0);  // 标的注册
```

### 2.4 验证

- OptionRisk::update() 被 onComputeValuesCompleted 调用
- totalDelta 返回非零（有持仓时）
- 回测无 segfault

---

## Phase 3: CTG 改走 OTG + combineMarkets 激活

**问题**：CTG::refresh 读 OptionValues（绕过 OTD）。combineMarkets 是 no-op。

### 3.1 CTG::refresh 改走 OTD

```cpp
// 当前 (L88): 读 od->values(0).ourMarket() 和 od->currentMarket()
// 改为: 通过 OTG 获取 OTD, 读 otd->multiMarket() 和 otd->getCurrentMarket()
void CTG::refresh() {
    for (const auto& od : m_otg->getOptionGrid()->getAllOptions()) {
        auto otd = od->getTradingData();
        if (!otd) continue;
        const auto& desired = otd->multiMarket();   // 从 OTD 读
        const auto& current = otd->getCurrentMarket();
        UPDATE_TYPE utype = check_markets(desired, current);
        // ... 收集 pendingQuotes ...
    }
}
```

### 3.2 combineMarkets 激活

```cpp
// 当前 (L280): no-op
// 改为: 合并 OptionValues.ourMarket → otd->multiMarket
void CTG::combineMarkets(const OptionData& od,
    std::vector<OptionTradingDataPtr>& outList) {
    auto otd = od.getTradingData();
    if (!otd) return;
    MultiMarket& our_mkt = otd->multiMarket();
    our_mkt.clear();
    if (m_ctx->enabled && otd->isActive() && od.values(0).isPriced())
        our_mkt = od.values(0).ourMarket();  // slot 0 (定价 desired)
    // slot 2 (scanner desired) 预留
    // ... discard 检查 ...
    outList.push_back(otd);
}
```

### 3.3 refresh 先 combineMarkets 再收集 pendingQuotes

```cpp
void CTG::refresh() {
    // 1. combineMarkets: 合并 desired → otd->multiMarket
    std::vector<OptionTradingDataPtr> otd_list;
    for (const auto& od : m_otg->getOptionGrid()->getAllOptions()) {
        combineMarkets(*od, otd_list);
    }
    // 2. check_markets diff → pendingQuotes
    for (const auto& otd : otd_list) {
        UPDATE_TYPE utype = check_markets(otd->multiMarket(), otd->getCurrentMarket());
        if (utype == UT_NONE) continue;
        // ... 收集 pendingQuotes (从 otd->multiMarket 读 bid/ask) ...
    }
}
```

### 3.4 drainPendingQuotes 更新 OTD currentMarket

```cpp
// 当前: 更新 od->setCurrentMarket (OptionData 层)
// 改为: 更新 otd->setCurrentMarket (OTD 层)
auto od = m_otg->getOptionGrid()->get(pq.code);
if (od && od->getTradingData()) {
    MultiMarket mkt;
    mkt.setBest(0, PriceSize(pq.bidP, pq.bidQ));
    mkt.setBest(1, PriceSize(pq.askP, pq.askQ));
    od->getTradingData()->setCurrentMarket(mkt);  // OTD 层
}
```

### 3.5 验证

- combineMarkets 返回非空 otd_list
- otd->multiMarket 有 desired 数据
- otd->getCurrentMarket 从回报更新（非乐观）
- pendingQuotes > 0
- 回测无 segfault

---

## Phase 4: 防自成交 + onFill 回调闭环

**问题**：无 STP。on_trade 不触发 onFill。

### 4.1 drainPendingQuotes 加 STP（最小版）

```cpp
// 在 m_quoteExec 调用前:
if (pq.bidP > 0 && pq.askP > 0 && pq.bidP >= pq.askP) {
    WTSLogger::log_by_cat("strategy", LL_WARN,
        "STP: skip crossed quote {} bid={} >= ask={}", pq.code, pq.bidP, pq.askP);
    continue;
}
```

### 4.2 on_trade → CompositeOptionPricer::onFill

```cpp
void UftOptionStrategy::on_trade(IUftStraCtx* ctx, uint32_t localid,
    const char* stdCode, bool isBuy, double vol, double price)
{
    _positions[stdCode] += (isBuy ? 1 : -1) * vol;
    
    // 触发 pricer onFill
    if (_pricer) {
        auto stub = std::make_shared<OrderStub>(stdCode, isBuy ? BUY : SELL, price, vol);
        _pricer->onFill(stub, price, static_cast<uint32_t>(vol));
    }
    
    // 更新 OptionRisk 持仓 (通过 OptionRiskData)
    if (_risk) {
        auto rd = _risk->get(stdCode);
        if (rd) {
            rd->addFill((isBuy ? 1 : -1) * static_cast<int32_t>(vol), price);
        }
    }
}
```

### 4.3 onFill 补充 deactivate

```cpp
// CompositeOptionPricer::onFill 补充 (对标 quantbox L2590):
// 成交后立即停报价 (保护机制)
auto od = m_grid->get(order->code);
if (od && od->getTradingData()) {
    od->values(0).ourMarket().clear();
    od->getTradingData()->setActive(false);
}
```

### 4.4 on_order 更新 OTD currentMarket

```cpp
void UftOptionStrategy::on_order(IUftStraCtx* ctx, uint32_t localid,
    const char* stdCode, bool isBuy, double totalQty, double leftQty,
    double price, bool isCanceled)
{
    auto od = _grid->get(stdCode);
    if (!od || !od->getTradingData()) return;
    if (isCanceled) {
        od->getTradingData()->setCurrentMarket(MultiMarket());  // 清空
    }
}
```

### 4.5 验证

- STP: bid >= ask 被跳过
- on_trade 后 m_instrument_lastbuy/lastsell 有值
- 成交后 ourMarket clear + setActive(false)
- m_bShouldComputeRiskShiftsVega = true
- 回测无 segfault

---

## Phase 5: 参数配置统一 + alpha 激活

**问题**：alpha weight=0。getTime()=0。硬编码 enableExpiry。

### 5.1 configbt.yaml params 扩展

```yaml
params:
    # alpha 权重
    wgt_vegaflow: 1.0
    wgt_frontfut_skew: 0.5
    wgt_deltaflow: 1.0
    wgt_atmsig: 0.5
    wgt_rollema: 0.5
    # EMA 窗口
    vegaflow_window: 300
    frontfut_skew_window: 60
    deltaflow_window: 10
    # 报价参数
    sticky_base: 0.5
    improve_retreat_ratio: 3.0
    trade_shock_ticks: 1
    # 到期日配置
    expiryConfig:
        "202608":
            enable: true
            delta_min: 0.05
            delta_max: 0.95
            sprd_fwd: 0.01
            sprd_atmvol: 0.1
            max_pos_opt: 50
            max_pos_stk: 50
            max_qsize: 5
            enable_auto_close: true
            close_pos_thresh: 0
```

### 5.2 init 读取参数

- 读 wgt_* → copCfg
- 读 expiryConfig → enableExpiry + setMaxPosQty + setSpreads
- 读 EMA 窗口 → copCfg

### 5.3 getTime 从 stra_get_time 获取

```cpp
// OptionTraderContext 增加 getTimeFn
_traderCtx->getTimeFn = [this]() {
    return _ctx ? static_cast<double>(_ctx->stra_get_time()) : 0;
};
```

### 5.4 currentDate 从 stra_get_date 获取

### 5.5 验证

- alpha 非 0（有 EMA 后）
- TPS 限流生效
- currentDate 从框架获取

---

## Phase 6: QuoteMode + rankOption6因子 + 标的报价

### 6.1 computeOurMarkets 使用 OTD QuoteMode

对标 quantbox L1315-1360：
- ON: 双边报
- AUTO: delta range 内双边报
- AUTO↔CLOSE: enable_auto_close 时自动切换
- CLOSE: 只报减仓侧（close_pos_thresh 阈值）
- OFF: 清空停报

### 6.2 rankOption 补全 6 因子 + drainPendingQuotes 排序

对标 quantbox L567-630：
1. UT_CANCEL > UT_NEW > UT_UPDATE
2. Crossing mid
3. Best bid/ask
4. Delta-based
5. Spread tightness
6. Days to expiry < 30

drainPendingQuotes 里按 rank 排序 pendingQuotes。

### 6.3 rankFuture

对标 quantbox L630-691。

### 6.4 UnderlyingTradingData 接线

- computeOurMarketsFuture 通过 UTD::ourMarket
- UTD::updateOrders → 标的报价

### 6.5 improve_retreat_ratio

- __apply_sticky_params 里用到（已有代码）
- 从 config 读取

### 6.6 验证

- QuoteMode CLOSE → 只报减仓侧
- rankOption 6因子排序
- computeOurMarketsFuture 有 UTD 数据

---

## Phase 7: Scanner + 运维功能

### 7.1 CTG 新增方法（对标 quantbox CTG）

| 方法 | quantbox 行 | 职责 |
|---|---|---|
| onOptionHit | L943 | Scanner 信号落地 → 标记更新 |
| onSetQMode | L832 | 运行时切换报价模式 |
| tradingStopMidDay | L225 | 盘中暂停 |
| onValuesReadyChanged | L748 | 到期数据就绪通知 |
| onPositionUpdated | L760 | 持仓变更广播 |
| onFillWithFees | L768 | 成交统计+lateFill+maxFillsPerSec |

### 7.2 Scanner 实现优先级

1. MMScanner（做市信号）
2. ButterflyScanner（跨式）
3. VolSpreadScanner（波动率价差）
4. 其他后续

### 7.3 验证

- onOptionHit 被调
- onSetQMode 切换 QuoteMode
- tradingStopMidDay 暂停

---

## Phase 8: 清理 + 回归验证

### 8.1 std::cout → WTSLogger (21处)
### 8.2 清理 debug static 计数器 (~15处)
### 8.3 stoul/stod try-catch
### 8.4 删除 OptionPricer.cpp stub
### 8.5 s_cache thread_local 修复
### 8.6 回归验证

编译+回测+全链路检查。

---

## 依赖关系

```
Phase 1 (OTG 中间层) ← 核心, 一切的基础
    ↓
Phase 2 (OptionQuoteManager) ← per-contract 订单管理, 依赖 OTD
    ↓
Phase 3 (OptionRisk) ← 依赖 OTG 持有
    ↓
Phase 4 (CTG 改走 OTG) ← 依赖 OTG 的 OTD+OM
    ↓
Phase 5 (STP + onFill) ← STP 在 OM 里, onFill 依赖 OM 回调
    ↓
Phase 6 (参数 + alpha) ← 独立, 可并行
    ↓
Phase 7 (QuoteMode + rank) ← 依赖 OTD + OM + UTD
    ↓
Phase 8 (Scanner + 运维) ← 依赖 Phase 7
    ↓
Phase 9 (清理)
```

## 风险评估

| Phase | 改动范围 | 风险 | 缓解 |
|---|---|---|---|
| 1 | 新建 OTG + OptionData 改 + UftOptionStrategy 改 + CTG 改 | 高 | 逐文件改, 逐次编译 |
| 2 | 新建 OptionQuoteManager + OTD 改 + 策略回调改 | 高 | 完整迁移 QuoteOrderManager 核心逻辑 |
| 3 | UftOptionStrategy 3行 + addListener | 低 | 独立验证 |
| 4 | CTG refresh + combineMarkets | 中 | 逐函数改 |
| 5 | on_trade/on_order + CTG + CompositeOptionPricer | 中 | STP 在 OM 里已有 |
| 6 | configbt.yaml + init | 低 | 配置变更 |
| 7 | computeOurMarkets + rankOption + UTD | 中 | 依赖 Phase 1+2+4 |
| 8 | Scanner + 运维 | 中 | 可分批 |
| 9 | 全局清理 | 低 | 最后做 |
