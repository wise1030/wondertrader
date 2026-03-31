/*!
 * \file FutuQuoter.h
 * \brief Multi-level bilateral quoting engine for futures market making
 * 
 * Manages multiple price levels of bids/asks per contract, with:
 *   - Configurable number of levels and per-level quantity
 *   - Auto-adjustment of widths based on portfolio risk (via FutuPortfolio)
 *   - Efficient O(1) cancel-and-replace for quote refreshes
 *   - Order tracking per level for targeted modifications
 * 
 * Designed for inline synchronous use within on_tick callback.
 * 
 * Performance optimizations:
 *   - O(1) order lookup via hash map
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

NS_WTP_BEGIN
class IUftStraCtx;
NS_WTP_END

namespace futu {

class FutuPortfolio;

/// A single price level in the quote ladder
struct QuoteLevel
{
    double      price;          ///< Quote price
    double      qty;            ///< Quote quantity
    uint32_t    order_id;       ///< Active order ID (0 = no active order)
    bool        is_bid;         ///< true=bid, false=ask

    QuoteLevel() : price(0), qty(0), order_id(0), is_bid(true) {}
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
                                  ///< 当新计算价格与当前挂单价格偏离不超过此阈值时，保留原订单不撤单重报
    
    QuoterConfig()
        : num_levels(3), base_spread(2.0), level_step(1.0)
        , base_qty(1.0), qty_decay(0.7), tick_size(1.0)
        , sticky_threshold(1.0) {}  // 默认1个tick的粘性阈值
};

/// Multi-level quoter for a single contract
class FutuQuoter
{
public:
    FutuQuoter();
    ~FutuQuoter() {}

    /// Initialize with config
    void init(const QuoterConfig& cfg);

    /// Get configuration
    const QuoterConfig& config() const { return _cfg; }

    //==========================================================================
    // Quote management (called within on_tick, all synchronous)
    //==========================================================================

    /// Refresh all quote levels based on current market data
    /// @param ctx     Strategy context for placing/cancelling orders
    /// @param mid     Current mid-price or fair value
    /// @param skew    Price skew from portfolio risk (shifts all levels)
    /// @param spread_mult  Spread multiplier from portfolio risk (widens levels)
    /// @param allow_bid   Whether bidding is allowed (e.g. not if at max long)
    /// @param allow_ask   Whether asking is allowed (e.g. not if at max short)
    /// @return       Number of new orders placed (for rate limiting)
    uint32_t refreshQuotes(wtp::IUftStraCtx* ctx, double mid, double skew = 0,
                           double spread_mult = 1.0, bool allow_bid = true, bool allow_ask = true);

    /// Cancel all outstanding quotes
    void cancelAll(wtp::IUftStraCtx* ctx);

    /// Handle order update — update tracked order IDs (O(1) lookup)
    void onOrder(uint32_t localid, bool isCanceled, double leftQty);

    /// Handle fill — clear the filled level's order ID (O(1) lookup)
    void onTrade(uint32_t localid, double vol, double price);

    //==========================================================================
    // Accessors
    //==========================================================================

    /// Get all active bid levels
    const std::vector<QuoteLevel>& getBidLevels() const { return _bid_levels; }
    
    /// Get all active ask levels
    const std::vector<QuoteLevel>& getAskLevels() const { return _ask_levels; }

    /// Total bid quantity outstanding
    double totalBidQty() const;

    /// Total ask quantity outstanding
    double totalAskQty() const;

    /// Check if a given order ID belongs to this quoter - O(1) lookup
    inline bool isMyOrder(uint32_t localid) const
    {
        return _order_to_level.find(localid) != _order_to_level.end();
    }

    /// Get the quote level for a given order ID - O(1) lookup, returns nullptr if not found
    inline QuoteLevel* getLevelByOrder(uint32_t localid)
    {
        auto it = _order_to_level.find(localid);
        if (it != _order_to_level.end())
            return it->second;
        return nullptr;
    }

private:
    /// Compute price for a given level
    __attribute__((always_inline)) 
    inline double computeBidPrice(double mid, double skew, double spread_mult, uint32_t level) const
    {
        double offset = (_cfg.base_spread + level * _cfg.level_step) * spread_mult * _cfg.tick_size;
        double raw = mid + skew - offset;
        return floor(raw / _cfg.tick_size) * _cfg.tick_size;
    }

    __attribute__((always_inline))
    inline double computeAskPrice(double mid, double skew, double spread_mult, uint32_t level) const
    {
        double offset = (_cfg.base_spread + level * _cfg.level_step) * spread_mult * _cfg.tick_size;
        double raw = mid + skew + offset;
        return ceil(raw / _cfg.tick_size) * _cfg.tick_size;
    }

    /// Compute quantity for a given level
    /// Returns integer quantity (minimum 1)
    __attribute__((always_inline))
    inline double computeQty(uint32_t level) const
    {
        double qty = _cfg.base_qty * pow(_cfg.qty_decay, level);
        // Ensure minimum 1 lot and round to integer
        return std::max(1.0, std::round(qty));
    }

    /// Check if a price has changed enough to warrant re-quoting
    __attribute__((always_inline))
    inline bool priceChanged(double oldPrice, double newPrice) const
    {
        return std::abs(oldPrice - newPrice) >= _cfg.tick_size * 0.5;
    }

    /// Register an order in the lookup map
    inline void registerOrder(uint32_t localid, QuoteLevel* level)
    {
        if (localid != 0 && level != nullptr)
            _order_to_level[localid] = level;
    }

    /// Unregister an order from the lookup map
    inline void unregisterOrder(uint32_t localid)
    {
        _order_to_level.erase(localid);
    }

private:
    QuoterConfig _cfg;
    std::vector<QuoteLevel> _bid_levels;
    std::vector<QuoteLevel> _ask_levels;

    /// O(1) order lookup map: order_id -> QuoteLevel*
    wtp::wt_hashmap<uint32_t, QuoteLevel*> _order_to_level;
};

} // namespace futu
