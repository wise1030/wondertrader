# WtOptionCore 增强设计计划 (修订版)

## 背景

基于 quantbox vs WtOptionCore 对比分析，结合 WT 框架已有的 TraderAdapter + ActionPolicyMgr 机制，设计 6 个增强模块。

## 关键架构认知

WT 框架已内置 quantbox PositionOffsetManager 的核心功能：
- **TraderAdapter::PosItem**: long/short × today/prev × vol/avail 持仓结构
- **ActionPolicyMgr**: buy/sell 自动按策略规则拆分为 open/closeToday/closeYestoday
- **on_position 回调**: broker权威持仓推送到策略
- **多策略共享**: TraderAdapter sink模式，所有策略共享同一PosItem
- **stra_enter_long/short**: 开仓, **stra_exit_long/short(isToday)**: 平今/平昨

WT 缺失部分（本计划补齐）：
1. 策略侧**可平量查询**（closeable today/prev）
2. **挂单冻结**可见性
3. **持仓不一致检测**（安全护栏）
4. **stra_quote 的 offset 感知**（quote API 不经 ActionPolicy）
5. **可组合风控过滤器链**
6. **成交价偏离监控**
7. **拒绝重试**

---

## Phase 1: RiskFilterChain - 可组合订单过滤器链

**新文件**: `RiskFilterChain.h/.cpp`

### 设计

```cpp
// 过滤器结果
enum class FilterResult { APPROVED, REJECTED, MODIFIED };

// 过滤器上下文（传给每个过滤器）
struct FilterContext {
    std::string code;
    bool isBuy;
    double price;
    uint32_t qty;
    int32_t currentPosition;      // 当前净持仓
    int32_t potentialPosition;    // 挂单后潜在持仓
    int32_t numCancels;           // 撤单次数
    int32_t numNewOrders;         // 新单次数
    int32_t numFills;             // 成交次数
};

// 过滤器接口
class IRiskFilter {
public:
    virtual ~IRiskFilter() = default;
    virtual FilterResult process(FilterContext& ctx) = 0;
    virtual const char* name() const = 0;
};

// 过滤器链
class RiskFilterChain {
    std::vector<std::unique_ptr<IRiskFilter>> m_filters;
public:
    void add(std::unique_ptr<IRiskFilter> f);
    FilterResult execute(FilterContext& ctx);  // 顺序执行，REJECT短路
};
```

### 5个内置过滤器

1. **MaxOrderSizeFilter** - 胖手指（拒绝或截断到最大值）
2. **MinSellPriceFilter** - 最低卖价保护
3. **MaxPositionFilter** - 3模式：REJECT / ALLOW_OVERFLOW / MODIFY_TO_MAX
4. **MaxCancelFilter** - 软/硬限制，软限制时允许减仓单通过(`abs(final) < abs(current)`)
5. **MaxNewOrdersFilter** - hard_flat / reject 双模式

### 集成点

`OptionQuoteManager::updateOrders()` 在 `sendQuote()` 之前调用过滤器链。

---

## Phase 2: PositionOffsetMgr - 策略侧可平量跟踪

**新文件**: `PositionOffsetMgr.h/.cpp`

### 设计

不同于 quantbox 的 PositionOffsetManager（订阅 TD 层消息），WT 框架的 TraderAdapter 已经维护了 PosItem。本模块作为**策略侧缓存层**，从 `on_position` 回调提取 today/prev 可平量，并跟踪挂单冻结。

```cpp
class PositionOffsetMgr {
public:
    // 从 on_position 回调更新（broker权威数据）
    void onPositionUpdate(bool isLong, double prevol, double preavail,
                           double newvol, double newavail);

    // 挂单/撤单时更新冻结量
    void onOrderSent(bool isBuy, uint32_t qty, bool isCloseToday);
    void onOrderCancelled(bool isBuy, uint32_t qty, bool isCloseToday);
    void onFill(bool isBuy, uint32_t fillQty, bool isCloseToday);

    // 查询可平量
    int32_t getCloseableToday(bool isBuy) const;   // 可平今
    int32_t getCloseablePrev(bool isBuy) const;    // 可平昨
    int32_t getCloseableTotal(bool isBuy) const;    // 总可平

    // 查询持仓
    int32_t getLongToday() const;
    int32_t getLongPrev() const;
    int32_t getShortToday() const;
    int32_t getShortPrev() const;

    // 订单拆分建议: 返回 (isOpen, isCloseToday, closeTodayQty, closePrevQty, openQty)
    struct OrderBreakdown {
        uint32_t closeTodayQty = 0;
        uint32_t closePrevQty = 0;
        uint32_t openQty = 0;
    };
    OrderBreakdown getOrderBreakdown(bool isBuy, uint32_t qty) const;

    // 持仓不一致检测
    struct DiscrepancyInfo {
        bool hasDiscrepancy = false;
        int32_t brokerPosition = 0;
        int32_t localPosition = 0;
        int32_t diff = 0;
    };
    DiscrepancyInfo checkDiscrepancy() const;

private:
    // Broker权威数据（从on_position更新）
    int32_t m_brokerLongVol = 0;     // long prevol + newvol
    int32_t m_brokerLongAvail = 0;   // long preavail + newavail
    int32_t m_brokerShortVol = 0;
    int32_t m_brokerShortAvail = 0;
    int32_t m_brokerLongTodayAvail = 0;
    int32_t m_brokerLongPrevAvail = 0;
    int32_t m_brokerShortTodayAvail = 0;
    int32_t m_brokerShortPrevAvail = 0;

    // 挂单冻结（策略侧跟踪）
    int32_t m_frozenLongToday = 0;   // 挂平多今冻结
    int32_t m_frozenLongPrev = 0;     // 挂平多昨冻结
    int32_t m_frozenShortToday = 0;
    int32_t m_frozenShortPrev = 0;

    // 内部fill累计（用于不一致检测）
    int32_t m_localLongVol = 0;
    int32_t m_localShortVol = 0;
};
```

### 集成方式

1. `HftOptionStrategy::on_position()` 调用 `PositionOffsetMgr::onPositionUpdate()`
2. `HftOptionStrategy::on_trade()` 调用 `PositionOffsetMgr::onFill()`
3. `OptionQuoteManager` 持有 `PositionOffsetMgr` 引用，发单前查询可平量
4. 当 `getCloseableTotal(isBuy) < qty` 时，改用 `stra_exit_long/short(isToday)` 精确平仓，而非 `stra_quote`

### 与WT框架的协作

- **stra_quote**: 用于双边做市报价（WT内部处理offset）
- **stra_enter_long/short**: 当需要主动开仓时
- **stra_exit_long/short(isToday=true/false)**: 当需要精确平今/平昨时
- **stra_buy/sell**: 当不需要区分开平仓时（ActionPolicyMgr自动拆分）

---

## Phase 3: PositionGuard - 持仓不一致检测

**新文件**: `PositionGuard.h/.cpp`

### 设计

```cpp
class PositionGuard {
public:
    struct Config {
        int32_t tolerance = 0;         // 允许的偏差（手数）
        double alertCooldownSec = 10;   // 告警冷却时间
        bool disableOnBreach = true;   // 偏差超限时禁用交易
    };

    PositionGuard(const Config& cfg);

    // 内部fill累计
    void onFill(bool isBuy, uint32_t qty);

    // 外部broker持仓（从on_position）
    void onBrokerPosition(bool isLong, double vol);

    // 检查
    bool isOK() const;                 // 无不一致
    int32_t getDiff() const;           // 内部 - 外部
    void reconcile();                  // 解决不一致（重置内部为broker值）

    // 严重不一致回调
    using DiscrepancyCallback = std::function<void(int32_t diff)>;
    void setDiscrepancyCallback(DiscrepancyCallback cb);

private:
    Config m_cfg;
    int32_t m_internalPos = 0;         // fill累计
    int32_t m_brokerPos = 0;            // on_position
    bool m_disabled = false;
    double m_lastAlertTime = 0;
    DiscrepancyCallback m_callback;
};
```

### 集成点

`HftOptionStrategy` 为每个合约创建 PositionGuard：
- `on_trade()` -> `guard.onFill(isBuy, qty)`
- `on_position()` -> `guard.onBrokerPosition(isLong, vol)`
- `guard.isOK() == false` -> 暂停该合约交易 + 告警

---

## Phase 4: FillPriceChecker - 成交价偏离监控

**新文件**: `FillPriceChecker.h/.cpp`

### 设计

```cpp
class FillPriceChecker {
public:
    struct Config {
        double warningThreshold = 0.0025;  // 0.25%
        double panicThreshold = 0.005;      // 0.5%
    };

    using WarningCallback = std::function<void(const std::string&, double fillPx, double issuePx, double pct)>;
    using PanicCallback = std::function<void(const std::string&, double fillPx, double issuePx, double pct)>;

    FillPriceChecker(const Config& cfg);

    // 记录发单价
    void onOrderSent(const std::string& code, uint32_t localid, double price);

    // 检查成交价
    void onFill(const std::string& code, uint32_t localid, double fillPx);

    void setWarningCallback(WarningCallback cb);
    void setPanicCallback(PanicCallback cb);

private:
    Config m_cfg;
    std::unordered_map<uint32_t, double> m_issuePrices;  // localid -> issue price
    WarningCallback m_warnCb;
    PanicCallback m_panicCb;
};
```

### 集成点

`OptionQuoteManager` 持有 FillPriceChecker：
- `sendQuote()` 时记录 issue price
- `onFill()` 时检查偏离

---

## Phase 5: RiskLimitsEx - 扩展预交易风控

**新文件**: `RiskLimitsEx.h/.cpp`

### 设计

扩展现有 `RiskLimits` 结构体，新增预交易检查：

```cpp
struct RiskLimitsEx {
    // 原有（继承RiskLimits语义）
    double maxDelta = 1000;
    double maxGamma = 100;
    double maxVega = 10000;
    double maxPositionPerOption = 100;
    double maxTotalPosition = 1000;
    double maxLossPerDay = 100000;
    double panicThreshold = 0.05;

    // 新增：预交易检查
    uint32_t maxOrderSize = 100;              // 单笔最大手数
    double maxOrderValue = 1000000;           // 单笔最大金额
    double clearlyErroneousPercent = 0.05;   // 价格偏离参考价百分比
    uint32_t maxBurstOrdersPerSec = 50;       // 突发报单频率
    int32_t maxShortCallPerSymbol = 0;        // 期权空头call限制(0=不限)
    int32_t maxShortPutPerSymbol = 0;          // 期权空头put限制(0=不限)
    double minSellPrice = 0;                   // 最低卖价

    // 检查结果
    enum class CheckResult { PASS, REJECT, WARN };
    struct CheckReport {
        CheckResult result;
        std::string reason;
        double value;
        double limit;
    };

    // 预交易检查（短路链）
    CheckReport checkPreTrade(const std::string& code, bool isBuy,
                                double price, uint32_t qty,
                                int32_t currentPosition,
                                double refPrice) const;

    // 事后复查
    CheckReport checkPostTrade(int32_t totalPosition, double totalExposure) const;

    // Greeks限制检查
    CheckReport checkGreeks(double delta, double gamma, double vega, double pnl) const;
};
```

### 集成点

`HftOptionStrategy::checkRiskLimits()` 使用 RiskLimitsEx 替代 RiskLimits。
CTG 执行前调用 `checkPreTrade()`。

---

## Phase 6: OrderRejectRetry - 拒绝重试

**修改文件**: `OptionQuoteManager.h/.cpp`

### 设计

在 OptionQuoteManager 中新增：

```cpp
// Config 新增
struct Config {
    // ... 现有字段 ...
    uint32_t reject_retry_delay_ms = 400;   // 拒绝后重试延迟
    uint32_t reject_max_retries = 3;        // 最大重试次数
};

// 新增私有成员
uint32_t m_rejectRetryCount = 0;
double m_lastRejectTime = 0;

// onOrderStatusChange 中检测拒绝
if (isCanceled && leftQty == totalQty) {
    // 全量拒绝/撤单未成交
    if (m_rejectRetryCount < m_cfg.reject_max_retries) {
        m_rejectRetryCount++;
        m_lastRejectTime = m_getTime ? m_getTime() : 0;
        // 延迟重试由timer触发 updateOrders(m_lastDesired)
    }
}
```

### 集成方式

- `OptionQuoteManager::onOrderStatusChange()` 检测全量拒绝
- 延迟 `reject_retry_delay_ms` 后重新调用 `updateOrders(m_lastDesired)`
- `HftOptionStrategy::on_timer()` 中检查各 OQM 是否需要重试

---

## 实现优先级

| 优先级 | Phase | 模块 | 原因 |
|--------|-------|------|------|
| P0 | 1 | RiskFilterChain | 可组合风控基础 |
| P0 | 2 | PositionOffsetMgr | 策略侧可平量跟踪（利用WT on_position） |
| P0 | 3 | PositionGuard | 安全护栏 |
| P1 | 4 | FillPriceChecker | 成交质量监控 |
| P1 | 5 | RiskLimitsEx | 风控完整性 |
| P2 | 6 | OrderRejectRetry | 健壮性提升 |

## 文件清单

### 新增文件
- `RiskFilterChain.h/.cpp` (Phase 1)
- `PositionOffsetMgr.h/.cpp` (Phase 2)
- `PositionGuard.h/.cpp` (Phase 3)
- `FillPriceChecker.h/.cpp` (Phase 4)
- `RiskLimitsEx.h/.cpp` (Phase 5)
- `tests/test_risk_filters.cpp` (单元测试)

### 修改文件
- `OptionQuoteManager.h/.cpp` - 集成过滤器链、PositionOffsetMgr、FillPriceChecker、拒绝重试
- `HftOptionStrategy.h/.cpp` - 创建各模块实例，连接回调
- `CMakeLists.txt` - 添加新源文件
- `README.md` - 文档更新
