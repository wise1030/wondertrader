/*!
 * \file FutuQuoter.h
 * \brief Multi-level bilateral quoting engine for futures market making
 * 
 * Manages multiple price levels of bids/asks per contract, with:
 *   - Configurable number of levels and per-level quantity
 *   - Auto-adjustment of widths based on portfolio risk (via FutuPortfolio)
 *   - Efficient O(1) cancel-and-replace for quote refreshes
 *   - Unified order tracking with AutoCancelPolicy via UnifiedOrderTracker
 * 
 * Designed for inline synchronous use within on_tick callback.
 * 
 * Performance optimizations:
 *   - Uses UnifiedOrderTracker for zero-copy order state access
 *   - Pre-allocated level vectors
 *   - Inline price computation
 */
#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <cmath>
#include "../Includes/ExecuteDefs.h"
#include "../Includes/WTSMarcos.h"
#include "../Includes/FasterDefs.h"
#include "BilateralQuoteStats.h"

NS_WTP_BEGIN
class IUftStraCtx;
class WTSSessionInfo;
NS_WTP_END

namespace futu {

class FutuPortfolio;
class UnifiedOrderTracker;

/// A single price level in the quote ladder (lightweight, no order state)
struct QuoteLevel
{
    double      price;          ///< Current quote price
    double      qty;            ///< Current quote quantity
    uint32_t    order_id;       ///< Active order ID (0 = no active order)
    uint8_t     level_index;    ///< Level index for O(1) lookup
    bool        is_bid;         ///< true=bid, false=ask
    
    QuoteLevel() : price(0), qty(0), order_id(0), level_index(0), is_bid(true) {}
};

/// Configuration for multi-level quoting on a single contract
struct QuoterConfig
{
    std::string code;           ///< Contract code
    uint32_t    num_levels;     ///< Number of price levels per side (e.g., 3 = 3 bids + 3 asks)
    double      base_spread;    ///< Base spread in ticks (per side, from mid)
    double      level_step;     ///< Additional tick step between levels
    double      base_qty;       ///< Base quantity per level
    double      qty_decay;      ///< Quantity multiplier for outer levels (e.g. 0.5 = halve each level)
    double      tick_size;      ///< Minimum price increment
    
    // Sticky 策略参数
    double      sticky_threshold; ///< Price stickiness threshold in ticks
    double      improve_retreat_ratio; ///< Ratio for asymmetric sticky: improve price容忍更大, retreat更敏感 (default: 2.0)
    
    // 价格验证参数
    double      max_price_deviation; ///< Max allowed price deviation from mid (in ticks), 0 = no limit
    
    // 做市报价价格保护参数
    // 防止报价穿过市场最优价格：bid不能超过最优买价+protect_ticks，ask不能低于最优卖价-protect_ticks
    bool        price_protection;    ///< 是否启用价格保护 (default: true)
    double      protect_ticks;       ///< 价格保护tick数 (default: 1.0)
    
    // 双边报价配置
    bool        use_bilateral_quote;  ///< 是否使用双边报价接口 stra_quote() (default: false)
    double      min_valid_qty;        ///< 有效挂单最小数量，用于统计 (default: 1.0)
    
    // 做市义务配置
    double      max_obligation_spread; ///< 做市义务最大报价宽度（ticks），用于单边受限时和双边报价统计 (default: 100.0)
    
    // v3 软风控参数（use_bilateral_quote=false 路径专用，bilateral 路径不受影响）
    double      qty_decay_factor;          ///< qty 指数衰减因子 (default: 2.0)，bidQty *= exp(-factor * long_util)
    double      obligation_min_qty;        ///< 软 obligation 报价最小手数 (default: 10)
    double      obligation_max_spread_ticks; ///< 软 obligation 最大报价宽度 ticks (default: 10)
    bool        obligation_only_l0;        ///< 软 obligation 是否仅 L0 (default: true)
    
    QuoterConfig()
        : num_levels(1), base_spread(2.0), level_step(1.0)
        , base_qty(5.0), qty_decay(0.7), tick_size(1.0)
        , sticky_threshold(1.0), improve_retreat_ratio(2.0), max_price_deviation(20.0)
        , price_protection(true), protect_ticks(1.0)
        , use_bilateral_quote(false), min_valid_qty(1.0), max_obligation_spread(10.0)
        , qty_decay_factor(2.0), obligation_min_qty(10.0)
        , obligation_max_spread_ticks(10.0), obligation_only_l0(true) {}
};

/// Multi-level quoter for a single contract
class FutuQuoter
{
public:
    FutuQuoter();
    ~FutuQuoter() {}

    /// Initialize with config
    void init(const QuoterConfig& cfg);
    
    /// Set shared order tracker (must be called before refreshQuotes)
    void setOrderTracker(UnifiedOrderTracker* tracker) { _tracker = tracker; }
    UnifiedOrderTracker* getOrderTracker() const { return _tracker; }

    /// Get configuration
    const QuoterConfig& config() const { return _cfg; }
    void updateQuotingParams(double base_spread, double base_qty, double qty_decay, double level_step) {
        _cfg.base_spread = base_spread;
        _cfg.base_qty = base_qty;
        _cfg.qty_decay = qty_decay;
        _cfg.level_step = level_step;
    }
    void updateStickyParams(double sticky_threshold, double improve_retreat_ratio) {
        _cfg.sticky_threshold = sticky_threshold;
        _cfg.improve_retreat_ratio = improve_retreat_ratio;
    }
    void updateProtectionParams(bool price_protection, double protect_ticks, double max_obligation_spread) {
        _cfg.price_protection = price_protection;
        _cfg.protect_ticks = protect_ticks;
        _cfg.max_obligation_spread = max_obligation_spread;
    }
    void updateMaxPriceDeviation(double max_price_deviation) {
        _cfg.max_price_deviation = max_price_deviation;
    }

    //==========================================================================
    // Quote management (called within on_tick, all synchronous)
    //==========================================================================

    /// Refresh all quote levels based on current market data
    /// @param ctx     Strategy context for placing/cancelling orders
    /// @param mid     Current mid-price or fair value
    /// @param skew    Price skew from portfolio risk in TICKS (shifts all levels)
    /// @param allow_bid   Whether bidding is allowed
    /// @param allow_ask   Whether asking is allowed
    /// @param now     Current timestamp (ms) for order tracking
    /// @param upper_limit  Upper price limit (涨停价), 0 = no limit
    /// @param lower_limit  Lower price limit (跌停价), 0 = no limit
    /// @param best_bid    Market best bid price (for price protection), 0 = no protection
    /// @param best_ask    Market best ask price (for price protection), 0 = no protection
    /// @param long_util   v3: 多头利用率 proj_long/max_position (default 0 = 兼容旧调用)
    /// @param short_util  v3: 空头利用率 proj_short/max_position (default 0 = 兼容旧调用)
    /// @param force_ask_obligation  v3: 多头打满，强制 ask 软义务报价 (default false)
    /// @param force_bid_obligation  v3: 空头打满，强制 bid 软义务报价 (default false)
    /// @return       Number of new orders placed (for rate limiting)
    uint32_t refreshQuotes(wtp::IUftStraCtx* ctx, double mid, double l0_bid_price, double l0_ask_price,
                           double spread_mult = 1.0, bool allow_bid = true, 
                           bool allow_ask = true, uint64_t now = 0,
                           double upper_limit = 0, double lower_limit = 0,
                           double best_bid = 0, double best_ask = 0,
                           double long_util = 0.0, double short_util = 0.0,
                           bool force_ask_obligation = false, bool force_bid_obligation = false);

    /// Cancel all outstanding quotes
    void cancelAll(wtp::IUftStraCtx* ctx);

    /// Handle order update — clear level order ID, trigger stats update
    /// @param localid       本地订单号
    /// @param isCanceled    是否撤销
    /// @param leftQty       剩余数量
    /// @param uTime_HHMM    当前时间 HHMM 格式（用于 BilateralStats 时间累计；0 = 不更新统计）
    /// @param sec_in_min    分钟内秒数 [0, 59]
    void onOrder(uint32_t localid, bool isCanceled, double leftQty,
                 uint32_t uTime_HHMM = 0, uint32_t sec_in_min = 0);

    /// Handle fill — clear the filled level's order ID, trigger stats update
    /// （成交导致挂单 qty 减少，可能跌出 min_valid_qty 累计深度阈值，必须更新统计）
    /// @param uTime_HHMM    当前时间 HHMM 格式（用于 BilateralStats 时间累计；0 = 不更新统计）
    /// @param sec_in_min    分钟内秒数 [0, 59]
    void onTrade(uint32_t localid, double vol, double price,
                 uint32_t uTime_HHMM = 0, uint32_t sec_in_min = 0);

    //==========================================================================
    // Accessors
    //==========================================================================

    /// Get all active bid levels
    const std::vector<QuoteLevel>& getBidLevels() const { return _bid_levels; }
    
    /// Get all active ask levels
    const std::vector<QuoteLevel>& getAskLevels() const { return _ask_levels; }
    
    /// Get modifiable bid levels (for direct updates)
    std::vector<QuoteLevel>& getBidLevelsMut() { return _bid_levels; }
    std::vector<QuoteLevel>& getAskLevelsMut() { return _ask_levels; }

    /// Total bid quantity outstanding
    double totalBidQty() const;

    /// Total ask quantity outstanding
    double totalAskQty() const;

    /// Check if a given order ID belongs to this quoter - O(1) via tracker
    bool isMyOrder(uint32_t localid) const;

    /// Get the quote level for a given order ID
    QuoteLevel* getLevelByOrder(uint32_t localid);
    
    //==========================================================================
    // 双边报价统计接口（R3 v2 - Per-Quoter 值成员）
    //==========================================================================

    /// 配置 BilateralStats（每 quoter 独立实例）
    /// @param sessInfo  必须传入有效 sessionInfo;nullptr 会让本 quoter 的统计 DISABLED
    /// @return true=注入成功, false=禁用统计
    bool initBilateralStats(WTSSessionInfo* sessInfo)
    {
        BilateralStatsConfig bcfg;
        bcfg.min_valid_qty = _cfg.min_valid_qty;
        bcfg.max_obligation_spread = _cfg.max_obligation_spread;
        _bilateral_stats.setConfig(bcfg);
        return _bilateral_stats.setSessionInfo(sessInfo, _cfg.code.c_str());
    }

    /// 获取统计模块（const,只读)
    const BilateralQuoteStats& getBilateralStats() const { return _bilateral_stats; }
    BilateralQuoteStats& getBilateralStats() { return _bilateral_stats; }

    /// 获取是否使用双边报价接口
    bool useBilateralQuote() const { return _cfg.use_bilateral_quote; }

    /// 获取有效报价快照（用于统计）
    /// 累计加权语义（R3 v2）：
    ///   - 从最优档累加到 min_valid_qty，最后档按 qty 截取
    ///   - 加权价 = Σ(qty_i × price_i) / min_valid_qty
    ///   - 整侧累计 < min_valid_qty 时 has_valid_xxx=false（深度不足该侧不 valid）
    ValidQuoteSnapshot getValidQuoteSnapshot() const;

private:
    /// Compute price for a given level
    /// @param mid        Current mid-price
    /// @param skew       Price skew in TICKS (will be multiplied by tick_size)
    /// @param spread_mult Spread multiplier
    /// @param level      Quote level index
    __attribute__((always_inline)) 
    inline double computeBidPrice(double mid, double skew, double spread_mult, uint32_t level) const
    {
        double offset = (_cfg.base_spread + level * _cfg.level_step) * spread_mult * _cfg.tick_size;
        // skew is in ticks, convert to price
        double skew_price = skew * _cfg.tick_size;
        double raw = mid + skew_price - offset;
        return floor(raw / _cfg.tick_size) * _cfg.tick_size;
    }

    __attribute__((always_inline))
    inline double computeAskPrice(double mid, double skew, double spread_mult, uint32_t level) const
    {
        double offset = (_cfg.base_spread + level * _cfg.level_step) * spread_mult * _cfg.tick_size;
        // skew is in ticks, convert to price
        double skew_price = skew * _cfg.tick_size;
        double raw = mid + skew_price + offset;
        return ceil(raw / _cfg.tick_size) * _cfg.tick_size;
    }

    /// Compute quantity for a given level
    __attribute__((always_inline))
    inline double computeQty(uint32_t level) const
    {
        if (level < _level_qtys.size())
            return _level_qtys[level];
        double qty = _cfg.base_qty * pow(_cfg.qty_decay, level);
        return std::max(1.0, std::round(qty));
    }

    /// Validate computed price before placing order
    /// @param price   Computed price to validate
    /// @param mid     Current mid-price
    /// @param upper_limit Upper price limit (0 = no check)
    /// @param lower_limit Lower price limit (0 = no check)
    /// @return true if price is valid, false if invalid
    __attribute__((always_inline))
    inline bool validatePrice(double price, double mid, 
                              double upper_limit, double lower_limit) const
    {
        // Check for NaN or Inf
        if (std::isnan(price) || std::isinf(price))
            return false;
        
        // Check for non-positive price
        if (price <= 0)
            return false;
        
        // Check against limits
        if (upper_limit > 0 && price > upper_limit)
            return false;
        if (lower_limit > 0 && price < lower_limit)
            return false;
        
        // Check for extreme deviation from mid (potential calculation error)
        if (_cfg.max_price_deviation > 0 && mid > 0 && _cfg.tick_size > 0)
        {
            double deviation_ticks = std::abs(price - mid) / _cfg.tick_size;
            if (deviation_ticks > _cfg.max_price_deviation)
                return false;
        }
        
        return true;
    }

    /// 检查是否需要更新订单（不对称粘性阈值）
    /// @param newPrice     新计算的价格
    /// @param currentPrice 当前订单价格
    /// @param is_bid       是否为 bid 方向
    /// @return true 表示需要更新订单
    bool checkStickyUpdate(double newPrice, double currentPrice, bool is_bid) const
    {
        // BID方向：向上(价格提高=改善)容忍更大，向下(价格降低=撤退)更敏感
        // ASK方向：向下(价格降低=改善)容忍更大，向上(价格提高=撤退)更敏感
        double upper_ratio = is_bid ? _cfg.improve_retreat_ratio : 1.0;
        double lower_ratio = is_bid ? 1.0 : _cfg.improve_retreat_ratio;
        double threshold = _cfg.sticky_threshold * _cfg.tick_size;
        double upper_bound = currentPrice + upper_ratio * threshold;
        double lower_bound = currentPrice - lower_ratio * threshold;
        return (newPrice > upper_bound || newPrice < lower_bound);
    }

private:
    QuoterConfig _cfg;
    std::vector<QuoteLevel> _bid_levels;
    std::vector<QuoteLevel> _ask_levels;
    std::vector<double> _level_qtys;
    
    UnifiedOrderTracker* _tracker;
    wtp::IUftStraCtx* _ctx = nullptr;
    bool _allow_bid = true;
    bool _allow_ask = true;
    
    BilateralQuoteStats _bilateral_stats;   ///< Per-Quoter 值成员（R3 v2）
    
    // 存储level索引+方向，避免bid/ask共用索引时查找冲突
    struct OrderLevelInfo {
        uint8_t level;    ///< Level index
        bool is_bid;      ///< true=bid, false=ask
    };
    wtp::wt_hashmap<uint32_t, OrderLevelInfo> _order_id_to_level;
};

} // namespace futu