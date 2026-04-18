/*!
 * \file PredictiveToxicity.h
 * \brief Predictive Toxicity Detection (VPIN, OFI, Alpha)
 * 
 * Handles pre-trade toxicity signals:
 *   - VPIN (Volume-synchronized Probability of Informed Trading)
 *   - OFI (Order Flow Imbalance)
 *   - Alpha signals from MicroAlphaEngine
 * 
 * This is the "before the fact" toxicity - predicting adverse selection
 * before it happens based on market microstructure signals.
 * 
 * Performance: Updated on every tick (high frequency)
 */
#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <cmath>
#include <unordered_map>
#include "../Includes/WTSMarcos.h"
#include "MicroAlphaEngine.h"

NS_WTP_BEGIN
class WTSTickData;
NS_WTP_END

namespace futu {

/// Predictive toxicity configuration
struct PredictiveToxicityConfig
{
    double      vpin_threshold;         ///< VPIN threshold for toxicity
    uint32_t    vpin_window;            ///< Number of buckets for VPIN
    double      vpin_bucket_size;       ///< Target volume per bucket (0=auto)
    double      alpha_threshold;        ///< Alpha signal threshold
    double      ofi_weight;             ///< Weight for OFI component
    double      trade_weight;           ///< Weight for trade imbalance component
    
    PredictiveToxicityConfig()
        : vpin_threshold(0.85)
        , vpin_window(50)
        , vpin_bucket_size(1000)
        , alpha_threshold(0.7)
        , ofi_weight(0.5)
        , trade_weight(0.5) {}
};

/// Predictive toxicity result
struct PredictiveToxicityResult
{
    double      vpin;                   ///< Current VPIN value
    double      ofi_toxicity;           ///< OFI-based toxicity
    double      trade_toxicity;         ///< Trade imbalance toxicity
    double      alpha_toxicity;         ///< Combined alpha toxicity
    double      combined_score;         ///< Weighted combined score
    bool        is_toxic;               ///< Exceeds threshold
    int         toxic_side;             ///< 1=buy toxic, -1=sell toxic, 0=none
    double      extreme_signal;         ///< Maximum extreme signal (> 0.9)
    
    PredictiveToxicityResult()
        : vpin(0), ofi_toxicity(0), trade_toxicity(0), alpha_toxicity(0)
        , combined_score(0), is_toxic(false), toxic_side(0), extreme_signal(0) {}
};

/// Predictive Toxicity Detector
class PredictiveToxicity
{
public:
    PredictiveToxicity();
    ~PredictiveToxicity() = default;
    
    //==========================================================================
    // Configuration
    //==========================================================================
    
    void setConfig(const PredictiveToxicityConfig& cfg) { _cfg = cfg; }
    const PredictiveToxicityConfig& getConfig() const { return _cfg; }
    
    void setBucketSize(double bucket_size);
    
    //==========================================================================
    // Data Input
    //==========================================================================
    
    /// Update with alpha and trade imbalance signals
    void updateAlpha(const AlphaResult& alpha, const TradeImbalanceResult& tradeImb);
    
    /// Update VPIN with explicit trade data
    void onTrade(double price, double qty, bool isBuy, uint64_t timestamp);
    
    /// Update VPIN from tick snapshot (infers direction)
    void onTickVolume(const char* stdCode, const wtp::WTSTickData* tick);
    
    //==========================================================================
    // Analysis
    //==========================================================================
    
    /// Compute predictive toxicity
    PredictiveToxicityResult analyze() const;
    
    /// Quick toxicity score
    double getToxicityScore() const;
    
    /// Get current VPIN
    double getVPIN() const { return _vpin; }
    
    //==========================================================================
    // State
    //==========================================================================
    
    bool hasData() const { return _has_alpha_data; }
    
    void reset();
    
private:
    PredictiveToxicityConfig _cfg;
    
    // Alpha data
    AlphaResult _latest_alpha;
    TradeImbalanceResult _latest_trade_imb;
    bool _has_alpha_data = false;
    
    // VPIN state
    struct VolumeBucket {
        double buy_volume{0};
        double sell_volume{0};
        double total_volume{0};
        uint64_t start_time{0};
        uint64_t end_time{0};
    };
    
    struct LastTickInfo {
        double bid_px{0};
        double ask_px{0};
        double total_volume{0};
    };
    
    double _bucket_size{0};
    std::vector<VolumeBucket> _buckets;
    VolumeBucket _current_bucket;
    double _vpin{0};
    std::vector<double> _order_imbalances;
    std::unordered_map<std::string, LastTickInfo> _last_ticks;
    
    // Cached result
    mutable PredictiveToxicityResult _cached_result;
    mutable bool _cache_dirty = true;
    
    void updateCache() const;
};

} // namespace futu
