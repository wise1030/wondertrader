/*!
 * \file UnifiedOrderTracker.h
 * \brief Unified Order Tracking for High-Frequency Market Making
 *
 * Single source of truth for order state, combining:
 *   - Order lifecycle management (from legacy SharedOrderTracker)
 *   - Self-trade detection (from legacy SelfTradePrevention)
 *   - Single source of truth for all order state
 *   - O(1) lookup by order ID
 *   - Efficient per-contract iteration
 *   - Self-trade detection between MM and arbitrage orders
 *
 * Performance optimizations:
 *   - Continuous memory layout (vector-based)
 *   - Lightweight per-contract indices (only order IDs)
 *   - Cache-friendly iteration
 *   - Inline access methods
 *
 * Thread Safety: NOT thread-safe. Must be called from single thread
 * or use external synchronization.
 */
#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <cmath>
#include <chrono>
#include "../Includes/FasterDefs.h"
#include "SpinLockGuard.h"
#include "../Includes/WTSMarcos.h"

NS_WTP_BEGIN
class IUftStraCtx;
NS_WTP_END

namespace futu
{

//==============================================================================
// Order Flags
//==============================================================================

/// Order flags for state tracking
enum class OrderFlags : uint8_t
{
    NONE = 0,
    PENDING_CANCEL = 1 << 0,
    IS_BID = 1 << 1, // Buy order
    IS_ACTIVE = 1 << 2,
    IS_MM_ORDER = 1 << 3, // Market making order (vs arbitrage)
    IS_ARB_ORDER = 1 << 4, // Arbitrage order
    IS_ZOMBIE = 1 << 5 // 撤单重试 K 次仍无 ack: 保留跟踪+计入 pending, 等升级处置 (B+)
};

inline OrderFlags operator|(OrderFlags a, OrderFlags b)
{
    return static_cast<OrderFlags>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

inline OrderFlags operator&(OrderFlags a, OrderFlags b)
{
    return static_cast<OrderFlags>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}

inline bool hasFlag(OrderFlags flags, OrderFlags flag)
{
    return (static_cast<uint8_t>(flags) & static_cast<uint8_t>(flag)) != 0;
}

//==============================================================================
// Cancel Reason
//==============================================================================

/// Cancellation reason - for analysis and statistics
enum class CancelReason : uint8_t
{
    NONE,
    PRICE_DEVIATION,     ///< Price deviated from mid beyond threshold
    STALE,               ///< Order age exceeded max_age_ms
    TIMEOUT,             ///< Order not filled within time limit
    STATE_CHANGE,        ///< Market state changed (volatility, etc.)
    INVENTORY_LIMIT,     ///< Inventory limit breached
    RISK_BREACH,         ///< Risk limit breach
    MARKET_STATE_CHANGE, ///< Market state changed to abnormal
    CLOSEOUT,            ///< Closeout before session end
    SELF_TRADE,          ///< Self-trade prevention triggered
    MANUAL,              ///< Manual cancellation
    REJECTED,            ///< Order rejected by broker/exchange (on_entrust failed)
    COUNT                ///< Number of cancel reasons (for stats array)
};

inline const char* cancelReasonToString(CancelReason reason)
{
    static const char* names[] = {"NONE",
                                  "PRICE_DEVIATION",
                                  "STALE",
                                  "TIMEOUT",
                                  "STATE_CHANGE",
                                  "INVENTORY_LIMIT",
                                  "RISK_BREACH",
                                  "MARKET_STATE_CHANGE",
                                  "CLOSEOUT",
                                  "SELF_TRADE",
                                  "MANUAL",
                                  "REJECTED"};
    size_t idx = static_cast<size_t>(reason);
    if (idx < sizeof(names) / sizeof(names[0]))
        return names[idx];
    return "UNKNOWN";
}

//==============================================================================
// Order Information
//==============================================================================

/// Fixed-size code buffer to avoid dynamic allocation
constexpr size_t MAX_CODE_LEN = 32;

/// Unified order information
struct UnifiedOrderInfo
{
    uint32_t order_id;
    uint32_t level_index; ///< Quote level (for MM orders)
    char code[MAX_CODE_LEN];
    double price;
    double qty;
    double filled_qty;   ///< 累计成交量 (部分成交跟踪)
    double original_qty; ///< 原始下单量 (track 时赋值, 不被 updateOrderQty 改写)
    double place_mid;    ///< Mid price at placement
    uint64_t place_time;
    uint64_t last_check;
    uint64_t last_inv_cancel_check;
    uint64_t cancel_time;        ///< 最近一次撤单发送/重试的时刻 (B+: mark 时写入, 重试时刷新)
    uint32_t cancel_retry_count; ///< 撤单重试次数 (>= cancel_max_retries 置 IS_ZOMBIE)
    OrderFlags flags;
    CancelReason cancel_reason;

    // Inline helpers
    inline bool isBid() const { return hasFlag(flags, OrderFlags::IS_BID); }
    inline bool isActive() const { return hasFlag(flags, OrderFlags::IS_ACTIVE); }
    inline bool isPendingCancel() const { return hasFlag(flags, OrderFlags::PENDING_CANCEL); }
    inline bool isMMOrder() const { return hasFlag(flags, OrderFlags::IS_MM_ORDER); }
    inline bool isArbOrder() const { return hasFlag(flags, OrderFlags::IS_ARB_ORDER); }
    inline bool isZombie() const { return hasFlag(flags, OrderFlags::IS_ZOMBIE); }

    inline void setPendingCancel(CancelReason reason, uint64_t now = 0)
    {
        flags = flags | OrderFlags::PENDING_CANCEL;
        cancel_reason = reason;
        cancel_time = now; // 0 = 未知, checkAutoCancel 首次观察时懒赋值兜底
        cancel_retry_count = 0;
    }

    inline void clearPendingCancel()
    {
        flags =
            static_cast<OrderFlags>(static_cast<uint8_t>(flags) & ~static_cast<uint8_t>(OrderFlags::PENDING_CANCEL));
        cancel_reason = CancelReason::NONE;
    }

    inline void setZombie() { flags = flags | OrderFlags::IS_ZOMBIE; }

    UnifiedOrderInfo()
        : order_id(0), level_index(0), price(0), qty(0), filled_qty(0), original_qty(0), place_mid(0), place_time(0),
          last_check(0), last_inv_cancel_check(0), cancel_time(0), cancel_retry_count(0), flags(OrderFlags::NONE),
          cancel_reason(CancelReason::NONE)
    {
        memset(code, 0, MAX_CODE_LEN);
    }
};

//==============================================================================
// Configuration
//==============================================================================

struct UnifiedTrackerConfig
{
    uint32_t max_orders;
    uint32_t max_age_ms;
    // B1(2026-08-24②): 原 price_deviation/sticky_threshold 删除 —— 前者消费者为死接口,
    //   后者与 QuoterConfig.sticky_threshold(顶单黏性)同名异义易混调。
    //   tracker 侧唯一用途 = STALE 订单延寿判定阈值(ticks), 独立成键。
    double stale_extension_ticks;
    uint32_t max_cancel_rate;
    uint32_t pending_cancel_timeout_ms; ///< B+: 撤单重试间隔 (默认 300ms, 超时重发而非遗忘)
    uint32_t cancel_max_retries;        ///< B+: 撤单最大重试次数, 达到后置 IS_ZOMBIE (默认 3)

    // Self-trade prevention
    bool stp_enabled;
    bool stp_allow_same_price;
    double stp_min_price_gap;

    UnifiedTrackerConfig()
        : max_orders(20), max_age_ms(10000), stale_extension_ticks(2.0), max_cancel_rate(10),
          pending_cancel_timeout_ms(300), cancel_max_retries(3), stp_enabled(true), stp_allow_same_price(false),
          stp_min_price_gap(1.0)
    {}
};

//==============================================================================
// Statistics
//==============================================================================

struct UnifiedTrackerStats
{
    uint32_t orders_placed;
    uint32_t orders_filled;
    uint32_t orders_canceled;
    uint32_t cancel_requests;
    uint32_t duplicate_cancels;

    // Per-reason cancel counts
    uint32_t price_deviation_cancels;
    uint32_t stale_cancels;
    uint32_t timeout_cancels;
    uint32_t state_change_cancels;
    uint32_t inventory_limit_cancels;
    uint32_t risk_breach_cancels;
    uint32_t market_state_cancels;
    uint32_t closeout_cancels;
    uint32_t self_trade_cancels;

    double avg_order_lifetime_ms;
    double fill_rate;
    double cancel_rate;
    uint64_t last_stats_log_time;

    // Self-trade stats
    uint32_t stp_checks;
    uint32_t stp_risks_detected;
    uint32_t stp_mm_cancels;

    UnifiedTrackerStats()
        : orders_placed(0), orders_filled(0), orders_canceled(0), cancel_requests(0), duplicate_cancels(0),
          price_deviation_cancels(0), stale_cancels(0), timeout_cancels(0), state_change_cancels(0),
          inventory_limit_cancels(0), risk_breach_cancels(0), market_state_cancels(0), closeout_cancels(0),
          self_trade_cancels(0), avg_order_lifetime_ms(0), fill_rate(0), cancel_rate(0), last_stats_log_time(0),
          stp_checks(0), stp_risks_detected(0), stp_mm_cancels(0)
    {}

    inline void recordCancel(CancelReason reason)
    {
        switch (reason) {
        case CancelReason::PRICE_DEVIATION:
            price_deviation_cancels++;
            break;
        case CancelReason::STALE:
            stale_cancels++;
            break;
        case CancelReason::TIMEOUT:
            timeout_cancels++;
            break;
        case CancelReason::STATE_CHANGE:
            state_change_cancels++;
            break;
        case CancelReason::INVENTORY_LIMIT:
            inventory_limit_cancels++;
            break;
        case CancelReason::RISK_BREACH:
            risk_breach_cancels++;
            break;
        case CancelReason::MARKET_STATE_CHANGE:
            market_state_cancels++;
            break;
        case CancelReason::CLOSEOUT:
            closeout_cancels++;
            break;
        case CancelReason::SELF_TRADE:
            self_trade_cancels++;
            break;
        default:
            break;
        }
    }
};

//==============================================================================
// Cancel Action
//==============================================================================

struct CancelAction
{
    uint32_t order_id;
    CancelReason reason;
    double deviation;

    CancelAction() : order_id(0), reason(CancelReason::NONE), deviation(0) {}
};

//==============================================================================
// Self-Trade Check Result
//==============================================================================

struct SelfTradeCheckResult
{
    bool has_risk;
    std::string risk_code;
    double conflict_price;
    double conflict_qty;
    std::vector<uint32_t> conflicting_order_ids;

    enum class Action : uint8_t
    {
        ALLOW,
        REJECT,
        CANCEL_MM_FIRST,
        ADJUST_PRICE
    } recommended_action;

    double adjusted_price;

    SelfTradeCheckResult()
        : has_risk(false), conflict_price(0), conflict_qty(0), recommended_action(Action::ALLOW), adjusted_price(0)
    {}
};

//==============================================================================
// Arbitrage Order Request (for self-trade check)
//==============================================================================

struct ArbitrageOrderRequest
{
    std::string code;
    bool is_buy;
    double price;
    double qty;
    bool is_market_order;

    ArbitrageOrderRequest() : price(0), qty(0), is_buy(true), is_market_order(false) {}
};

//==============================================================================
// Unified Order Tracker
//==============================================================================

class UnifiedOrderTracker
{
public:
    UnifiedOrderTracker() : _order_count(0), _total_cancels(0)
    {
        // P1优化: 预分配内存，减少运行时动态分配
        _orders.reserve(64);
        _free_slots.reserve(32);
        _cancel_timestamps.reserve(1000);
        _order_index.reserve(128);
        _order_place_times.reserve(128);
    }

    //==========================================================================
    // Configuration
    //==========================================================================

    void setConfig(const UnifiedTrackerConfig& cfg)
    {
        _cfg = cfg;
        // P1优化: 根据配置预分配内存
        if (_cfg.max_orders > _orders.capacity()) {
            _orders.reserve(_cfg.max_orders);
            _free_slots.reserve(_cfg.max_orders / 2);
            _order_index.reserve(_cfg.max_orders);
            _order_place_times.reserve(_cfg.max_orders);
        }
    }
    const UnifiedTrackerConfig& getConfig() const { return _cfg; }

    //==========================================================================
    // Order Management - MM Orders
    //==========================================================================

    /// Track a market making order
    uint32_t trackMMOrder(uint32_t orderId,
                          uint32_t levelIndex,
                          const std::string& code,
                          double price,
                          double qty,
                          double placeMid,
                          uint64_t placeTime,
                          bool isBid)
    {
        RecursiveSpinGuard _g(_lock);
        return trackOrderInternal(
            orderId, levelIndex, code, price, qty, placeMid, placeTime, isBid, true /*isMM*/, false /*isArb*/);
    }

    //==========================================================================
    // Order Management - Arbitrage Orders
    //==========================================================================

    /// Track an arbitrage order
    uint32_t trackArbOrder(uint32_t orderId,
                           const std::string& code,
                           double price,
                           double qty,
                           double placeMid,
                           uint64_t placeTime,
                           bool isBuy)
    {
        RecursiveSpinGuard _g(_lock);
        return trackOrderInternal(
            orderId, 0 /*levelIndex*/, code, price, qty, placeMid, placeTime, isBuy, false /*isMM*/, true /*isArb*/);
    }

    //==========================================================================
    // Order Management - Common
    //==========================================================================

    void updateOrderQty(uint32_t orderId, double remainingQty)
    {
        RecursiveSpinGuard _g(_lock);
        UnifiedOrderInfo* order = getOrderByOrderId(orderId);
        if (!order)
            return;
        if (remainingQty <= 0)
            untrackOrder(orderId);
        else {
            // F9/B+: 增量维护全源 pending (active 即计入, 含 pendingCancel — 保守口径,
            // 撤单飞行期仍视为在途, 防双份敞口)
            if (order->isActive())
                addPendingQty(order->code, order->isBid(), remainingQty - order->qty);
            order->qty = remainingQty;
        }
    }

    void untrackOrder(uint32_t orderId, uint64_t currentTime = 0);

    void markPendingCancel(uint32_t orderId, CancelReason reason, uint64_t now = 0)
    {
        RecursiveSpinGuard _g(_lock);
        UnifiedOrderInfo* order = getOrderByOrderId(orderId);
        if (order && !order->isPendingCancel()) {
            // B+: active→pendingCancel, 仍计入全源 pending (撤单未确认前敞口未消)
            order->setPendingCancel(reason, now);
        }
    }

    /// 原子撤单标记：一次锁内完成“存在 && !pending_cancel”校验、置 pending_cancel+撤单时刻。
    /// B+: pendingCancel 单仍计入全源 pending (保守口径), 不再扣减。
    /// 返回 true 表示本调用方成功取得撤单权，可以发送 stra_cancel；false 表示已由其他路径标记或订单不存在。
    bool tryMarkPendingCancel(uint32_t orderId, CancelReason reason, uint64_t now = 0)
    {
        RecursiveSpinGuard _g(_lock);
        UnifiedOrderInfo* order = getOrderByOrderId(orderId);
        if (!order || order->isPendingCancel())
            return false;

        order->setPendingCancel(reason, now);
        return true;
    }

    void clearPendingCancel(uint32_t orderId)
    {
        RecursiveSpinGuard _g(_lock);
        UnifiedOrderInfo* order = getOrderByOrderId(orderId);
        if (order && order->isPendingCancel()) {
            // B+: pending 未在 mark 时扣减, 此处无需加回
            order->clearPendingCancel();
        }
    }

    //==========================================================================
    // Query Methods - O(1)
    //==========================================================================

    inline bool isTracked(uint32_t orderId) const
    {
        RecursiveSpinGuard _g(_lock);
        return _order_index.find(orderId) != _order_index.end();
    }

    inline UnifiedOrderInfo* getOrderByOrderId(uint32_t orderId)
    {
        RecursiveSpinGuard _g(_lock);
        auto it = _order_index.find(orderId);
        if (it != _order_index.end())
            return &_orders[it->second];
        return nullptr;
    }

    inline const UnifiedOrderInfo* getOrderByOrderId(uint32_t orderId) const
    {
        RecursiveSpinGuard _g(_lock);
        auto it = _order_index.find(orderId);
        if (it != _order_index.end())
            return &_orders[it->second];
        return nullptr;
    }

    /// v7.6 阶段2: 锁内快照拷贝 — 供非持锁外部调用方替代裸指针
    /// (裸指针 API 仅限内部在递归锁内使用; 外部经此拷贝消除指针逃逸)
    bool getOrderInfoCopy(uint32_t orderId, UnifiedOrderInfo& out) const
    {
        RecursiveSpinGuard _g(_lock);
        const UnifiedOrderInfo* p = getOrderByOrderId(orderId);
        if (!p)
            return false;
        out = *p;
        return true;
    }

    inline UnifiedOrderInfo* getOrderByIndex(uint32_t index)
    {
        RecursiveSpinGuard _g(_lock);
        if (index < _orders.size())
            return &_orders[index];
        return nullptr;
    }

    // V8-R6/P3: getOrders() 引用逃生口已删除 —— 返回内部 vector 引用却即刻释放锁,
    // 全项目零调用(逐项 grep 复核), 属竞态地雷 API。
    inline uint32_t getOrderCount() const
    {
        RecursiveSpinGuard _g(_lock);
        return _order_count;
    }

    /// 订单集世代号: track/untrack 时递增。
    /// 调用方(如策略主线程)可缓存上次同步的世代号, 未变化时跳过
    /// MM 订单快照的全量深拷贝(updateMMOrders), 消除热路径无效拷贝.
    uint64_t getGeneration() const
    {
        RecursiveSpinGuard _g(_lock);
        return _generation;
    }

    //==========================================================================
    // Per-Contract Query
    //==========================================================================

    /// Get all active order IDs for a contract
    std::vector<uint32_t> getOrderIdsForContract(const std::string& code) const;

    /// Get active MM buy order IDs for a contract
    std::vector<uint32_t> getMMBuyOrderIds(const std::string& code) const;

    /// Get active MM sell order IDs for a contract
    std::vector<uint32_t> getMMSellOrderIds(const std::string& code) const;

    /// Get best MM buy price for a contract
    double getBestMMBuy(const std::string& code) const;

    /// Get best MM sell price for a contract
    double getBestMMSell(const std::string& code) const;

    /// Check if contract has active MM orders
    bool hasMMOrders(const std::string& code) const;

    /// Get total pending buy quantity for a contract (for position limit check)
    double getPendingBuyQty(const std::string& code) const;

    /// Get total pending sell quantity for a contract (for position limit check)
    double getPendingSellQty(const std::string& code) const;

    /// T4: 全源 pending (MM+arb 等所有 IS_ACTIVE 单; B+ 起含 PENDING_CANCEL --
    /// 撤单未确认前敞口未消的保守口径, 自成交/报价深度等实时性口径仍排除)。
    /// 旧口径只统计 MM 索引 -> arb 两腿/closeout 在途单对 skew/force/taker
    /// 的 util 投影不可见, 大幅 arb 建仓期间 util 系统性偏低。
    double getPendingBuyQtyAllSources(const std::string& code) const;
    double getPendingSellQtyAllSources(const std::string& code) const;

    //==========================================================================
    // Price Deviation Check
    //==========================================================================

    // B1(2026-08-24②): checkPriceDeviation/exceedsStickyThreshold 已删 —— 零调用死接口,
    //   报价黏性/价格偏离主判定在 FutuQuoter::checkStickyUpdate (sticky_threshold 专属 quoter)。
    //   tracker 侧仅保留 STALE 延寿判定, 使用独立键 stale_extension_ticks (原 sticky_threshold×2)。

    //==========================================================================
    // Auto-Cancel Checks
    // 注：STATE_CHANGE 和 PRICE_DEVIATION 已移除，由 FutuQuoter 粘性逻辑处理
    //==========================================================================

    const std::vector<CancelAction>& checkAutoCancel(const std::string& code,
                                                     uint64_t currentTime,
                                                     double currentMid,
                                                     double tickSize,
                                                     bool stateChanged);

    /// B+: zombie 升级列表 (本次 checkAutoCancel 新置 IS_ZOMBIE 的合约, 每合约去重)
    /// 由调用方 (coordinator) 处置: 告警 + 该合约 halt + stra_cancel_all(fullCode) 兜底。
    const std::vector<std::string>& getZombieEscalations() const { return _zombie_codes_buf; }

    /// B+ 修复(P2-3): 当前仍有存活 IS_ZOMBIE 单的合约集合 (每次 checkAutoCancel 刷新)。
    /// 调用方据此释放 zombie 已清零合约的 halt 闩锁 (retainZombieHalts)。
    const std::vector<std::string>& getAliveZombieContracts() const { return _zombie_alive_buf; }

    /// B+: 通道恢复锚点 - 清空 zombie 集合 (untrack 所有 IS_ZOMBIE 单, 清升级去重表)。
    /// 撤不掉的残留交引擎侧持仓对账 (登录 queryOrders/queryPosition 重建)。
    /// B+ 修复(P1-2): 返回被 untrack 的 zombie 订单 id -- 调用方需 (a) 引擎侧补发
    /// 全撤 (断连中失败的撤单重连后无人补发) (b) 广播 quoter.onOrder(id,canceled)
    /// 清孤儿槽 (terminal 回报永不到达时残留 id 会永久阻塞该层挂单门禁)。
    std::vector<uint32_t> clearZombies();

    //==========================================================================
    // Self-Trade Prevention
    //==========================================================================

    /// Check if an arbitrage order would cause self-trade
    SelfTradeCheckResult checkArbitrageOrder(const ArbitrageOrderRequest& request) const;

    /// Check for a specific contract and direction
    SelfTradeCheckResult
    checkSelfTrade(const std::string& code, bool is_buy, double price, bool is_market_order = false) const;

    /// Get MM orders that would conflict with an arbitrage order
    std::vector<uint32_t> getConflictingMMOrders(const std::string& code, bool arb_is_buy, double arb_price) const;

    //==========================================================================
    // Statistics
    //==========================================================================

    uint32_t getTotalTracked() const
    {
        RecursiveSpinGuard _g(_lock);
        return _order_count;
    }
    uint32_t getTotalCancellations() const
    {
        RecursiveSpinGuard _g(_lock);
        return _total_cancels;
    }
    const UnifiedTrackerStats& getStats() const
    {
        RecursiveSpinGuard _g(_lock);
        return _stats;
    }

    void recordFilled()
    {
        RecursiveSpinGuard _g(_lock);
        _stats.orders_filled++;
    }

    /// 记录一笔成交, 返回 true 表示该订单已完全成交(可安全 untrack)。
    /// 部分成交时订单保持跟踪: 残留活单仍需参与自成交检查/在途量统计/sticky 判断。
    /// 未跟踪的订单(如 OrderRouter 的单)返回 false, 调用方无需处理。
    bool recordOrderFill(uint32_t orderId, double qty)
    {
        RecursiveSpinGuard _g(_lock);
        auto it = _order_index.find(orderId);
        if (it == _order_index.end())
            return false;
        UnifiedOrderInfo& order = _orders[it->second];
        order.filled_qty += qty;
        // 用 original_qty (不被 updateOrderQty 改写) 判定完全成交。
        // 若用 order.qty: onOrder 部分成交分支会调 updateOrderQty 把 qty 改写为剩余量,
        // 导致 filled_qty >= remaining_qty 提前成立 → 多次部分成交的 MM 单被提前 untrack。
        return order.filled_qty >= order.original_qty - 1e-9;
    }
    void recordDuplicateCancel()
    {
        RecursiveSpinGuard _g(_lock);
        _stats.duplicate_cancels++;
    }

    bool shouldCancelDueToRate(uint64_t currentTime);

    void updateFillRateStats()
    {
        RecursiveSpinGuard _g(_lock);
        if (_stats.orders_placed > 0)
            _stats.fill_rate = static_cast<double>(_stats.orders_filled) / _stats.orders_placed;
    }

    //==========================================================================
    // Reset
    //==========================================================================

    void clear()
    {
        RecursiveSpinGuard _g(_lock);
        _orders.clear();
        _free_slots.clear();
        _order_index.clear();
        _order_place_times.clear();
        _cancel_timestamps.clear();
        _orders_by_code.clear();
        _mm_buy_by_code.clear();
        _mm_sell_by_code.clear();
        _pending_buy_all.clear();  // F9
        _pending_sell_all.clear(); // F9
        _best_buy_price.clear();
        _best_sell_price.clear();
        _order_count = 0;
        _total_cancels = 0;
        _stats = UnifiedTrackerStats();
    }

private:
    //==========================================================================
    // Internal Methods
    //==========================================================================

    uint32_t trackOrderInternal(uint32_t orderId,
                                uint32_t levelIndex,
                                const std::string& code,
                                double price,
                                double qty,
                                double placeMid,
                                uint64_t placeTime,
                                bool isBid,
                                bool isMM,
                                bool isArb);

    void updateBestPrices(const std::string& code, double price, bool is_buy);

    //==========================================================================
    // Data Members
    //==========================================================================

    UnifiedTrackerConfig _cfg;

    // v7.6 阶段2: 递归自旋锁 — 公开方法统一守卫 (MdSpi/TdSpi 双线程访问;
    //   递归: checkAutoCancel→untrackOrder 等嵌套调用)
    mutable RecursiveSpinLock _lock;

    // Main storage - continuous memory layout
    std::vector<UnifiedOrderInfo> _orders;
    std::vector<uint32_t> _free_slots;
    wtp::wt_hashmap<uint32_t, uint32_t> _order_index; // orderId -> vector index
    uint32_t _order_count;
    uint64_t _generation = 0; ///< 订单集世代号(track/untrack 递增)
    uint32_t _total_cancels;

    // checkAutoCancel 复用缓冲 (每 tick 调用, 避免重复堆分配; 主线程单线程访问)
    std::vector<CancelAction> _actions_buf;
    std::vector<size_t> _active_indices_buf;
    std::vector<uint32_t> _stale_buf;

    // Place times for lifetime calculation
    wtp::wt_hashmap<uint32_t, uint64_t> _order_place_times;

    // Cancel timestamps for rate limiting
    std::vector<uint64_t> _cancel_timestamps;

    // Per-contract indices (lightweight - only order IDs)
    wtp::wt_hashmap<std::string, std::vector<uint32_t>> _orders_by_code;

    // F9: 全源 pending 增量计数 (track/untrack/updateQty 状态变迁时维护),
    //   getPending*QtyAllSources 从每 tick 遍历+hash 降为 O(1) 查询。
    //   B+ 不变量: 含全部 IS_ACTIVE 单 (含 PENDING_CANCEL — 撤单未确认前敞口未消,
    //   风控保守口径, 防双份敞口; 自成交/报价深度等实时性口径仍排除 PENDING_CANCEL)。
    wtp::wt_hashmap<std::string, double> _pending_buy_all;
    wtp::wt_hashmap<std::string, double> _pending_sell_all;

    // B+: zombie 升级去重表 (zombie 清零的合约在 checkAutoCancel 中重置 -- 重新
    //   武装升级, 修复"同合约第二个 zombie 无升级/无兜底"; clearZombies 时全清)
    std::vector<std::string> _zombie_codes_buf;
    std::vector<std::string> _zombie_alive_buf; // B+ 修复(P2-3): 存活 zombie 合约集合
    wtp::wt_hashmap<std::string, bool> _zombie_escalated;

    inline void addPendingQty(const char* code, bool isBid, double delta)
    {
        if (delta == 0.0)
            return;
        auto& m = isBid ? _pending_buy_all : _pending_sell_all;
        double& v = m[code];
        v += delta;
        if (v < 0.0 && v > -1e-9)
            v = 0.0; // 浮点残差收敛
    }

    // Separate indices for MM buy/sell orders (for self-trade detection)
    wtp::wt_hashmap<std::string, std::vector<uint32_t>> _mm_buy_by_code;
    wtp::wt_hashmap<std::string, std::vector<uint32_t>> _mm_sell_by_code;

    // Best price cache
    wtp::wt_hashmap<std::string, double> _best_buy_price;
    wtp::wt_hashmap<std::string, double> _best_sell_price;

    // Statistics
    UnifiedTrackerStats _stats;
};

} // namespace futu
