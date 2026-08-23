/*!
 * \file SelfTradeCalibrator.h
 * \brief Self-Trade Calibration for Toxicity Detection
 *
 * Uses self-trade fills as ground truth for calibration:
 *   - Records fill prices and tracks subsequent price movements
 *   - Calculates realized adverse selection ratio
 *   - Provides calibration signals for synthetic transaction inference
 *
 * This component serves as the "Ground Truth" for the overall signal fusion,
 * since it uses actual trading results rather than inferred data.
 */
#pragma once

#include <string>
#include <cstdint>
#include <atomic>
#include <map>
#include "FutuConfig.h"
#include "../Includes/FasterDefs.h"
#include "../Share/RingBuffer.hpp"
#include "SpinLockGuard.h"

namespace futu
{

//==============================================================================
// Fill Record
//==============================================================================

/// Record of a self-trade fill for analysis
struct SelfFillRecord
{
    std::string code;   ///< Contract code
    uint64_t fill_time; ///< Fill timestamp
    double fill_price;  ///< Fill price
    double fill_qty;    ///< Fill quantity
    bool is_buy;        ///< True if we bought

    // Market state at fill
    double mid_at_fill;    ///< Mid price at fill
    double spread_at_fill; ///< Spread at fill

    // Post-fill analysis
    double price_move_after; ///< Price move after fill (in ticks)
    bool was_adverse;        ///< True if adverse selection occurred
    double toxicity_score;   ///< Individual toxicity score

    // Timing
    uint64_t analysis_time; ///< When this record was analyzed

    SelfFillRecord()
        : fill_time(0), fill_price(0), fill_qty(0), is_buy(false), mid_at_fill(0), spread_at_fill(0),
          price_move_after(0), was_adverse(false), toxicity_score(0), analysis_time(0)
    {}
};

//==============================================================================
// Calibration Result
//==============================================================================

/// Calibration result from self-trade analysis
struct CalibrationResult
{
    double direction_bias; ///< Direction bias [-1, 1], positive = buy bias
    double toxicity_level; ///< Overall toxicity level [0, 1]
    bool high_toxicity;    ///< High toxicity warning flag
    int recommended_side;  ///< Recommended side (1=buy, -1=sell, 0=neutral)

    // Statistics
    double sample_size; ///< Number of samples used
    double confidence;  ///< Confidence of calibration [0, 1]

    // Breakdown
    double buy_adverse_ratio;  ///< Adverse ratio for buys
    double sell_adverse_ratio; ///< Adverse ratio for sells

    CalibrationResult()
        : direction_bias(0), toxicity_level(0), high_toxicity(false), recommended_side(0), sample_size(0),
          confidence(0), buy_adverse_ratio(0), sell_adverse_ratio(0)
    {}
};

//==============================================================================
// Toxicity Metrics (Extended)
//==============================================================================

/// Extended toxicity metrics from self-trade calibration
struct SelfTradeToxicityMetrics
{
    double predictive_toxicity; ///< Pre-trade toxicity estimate
    double realized_toxicity;   ///< Post-trade realized toxicity
    double toxicity_score;      ///< Combined score
    bool is_toxic;              ///< Toxicity flag
    int toxic_side;             ///< = -recommended_side (V8-T5: 与 PredictiveToxicity "1=buy toxic" 语义不一致, 当前零消费者, R2 方向裁决时统一)

    // Detailed stats
    double avg_adverse_move; ///< Average adverse move (ticks)
    uint32_t total_fills;    ///< Total fills analyzed
    uint32_t adverse_fills;  ///< Number of adverse fills

    SelfTradeToxicityMetrics()
        : predictive_toxicity(0), realized_toxicity(0), toxicity_score(0), is_toxic(false), toxic_side(0),
          avg_adverse_move(0), total_fills(0), adverse_fills(0)
    {}
};

//==============================================================================
// Configuration
//==============================================================================

struct SelfTradeCalibratorConfig
{
    uint32_t toxicity_window_ms;
    double adverse_threshold;
    uint32_t min_samples;
    double move_threshold_ticks;
    double tick_size;

    double retreat_ticks;         ///< 成交后退后tick数 (default 2)
    uint32_t retreat_cooldown_ms; ///< 后退冷却时间ms (default 3000)

    SelfTradeCalibratorConfig()
        : toxicity_window_ms(5000), adverse_threshold(0.6), min_samples(5), move_threshold_ticks(1.0), tick_size(1.0),
          retreat_ticks(2.0), retreat_cooldown_ms(3000)
    {}

    static SelfTradeCalibratorConfig fromVariant(wtp::WTSVariant* v)
    {
        SelfTradeCalibratorConfig c;
        c.toxicity_window_ms = FutuConfig::readUInt32(v, "toxicityWindowMs", 5000);
        c.adverse_threshold = FutuConfig::readDouble(v, "adverseThreshold", 0.6);
        c.min_samples = FutuConfig::readUInt32(v, "minSamples", 5);
        c.move_threshold_ticks = FutuConfig::readDouble(v, "moveThresholdTicks", 1.0);
        c.retreat_ticks = FutuConfig::readDouble(v, "retreatTicks", 2.0);
        c.retreat_cooldown_ms = FutuConfig::readUInt32(v, "retreatCooldownMs", 3000);
        return c;
    }
};

//==============================================================================
// Fill Retreat Result (成交后退机制)
//==============================================================================

/// 成交后退机制结果：最近成交导致的 bid/ask 价格后退量
struct FillRetreat
{
    double bid_retreat_price; ///< bid 不得高于此价格 (0=无限制)
    double ask_retreat_price; ///< ask 不得低于此价格 (0=无限制)
    bool bid_retreat_active;  ///< bid 后退是否生效
    bool ask_retreat_active;  ///< ask 后退是否生效

    FillRetreat() : bid_retreat_price(0), ask_retreat_price(0), bid_retreat_active(false), ask_retreat_active(false) {}
};

//==============================================================================
// Self Trade Calibrator
//==============================================================================

class SelfTradeCalibrator
{
public:
    SelfTradeCalibrator();
    ~SelfTradeCalibrator() = default;

    //==========================================================================
    // Configuration
    //==========================================================================

    void setConfig(const SelfTradeCalibratorConfig& config) { _config = config; }
    const SelfTradeCalibratorConfig& getConfig() const { return _config; }

    void setTickSize(double tickSize) { _config.tick_size = tickSize; }

    //==========================================================================
    // Data Input
    //==========================================================================

    /// Record a fill for calibration
    void recordFill(const std::string& code,
                    double price,
                    double qty,
                    bool is_buy,
                    double mid_price,
                    double spread,
                    uint64_t timestamp);

    /// Update with new tick (to track price after fills)
    void onTick(const std::string& code, double mid_price, uint64_t timestamp);

    //==========================================================================
    // Analysis
    //==========================================================================

    /// Get calibration result for a contract
    CalibrationResult getCalibration(const std::string& code) const;

    /// Get toxicity metrics for a contract
    SelfTradeToxicityMetrics getToxicityMetrics(const std::string& code) const;

    /// Get quick toxicity score
    double getToxicityScore(const std::string& code) const;

    /// Check if high toxicity
    bool isHighToxicity(const std::string& code) const;

    //==========================================================================
    // Fill Retreat (成交后退机制)
    //==========================================================================

    /// 获取当前成交后退限制
    /// @param code 合约代码
    /// @param current_time 当前时间戳
    /// @return 后退结果，包含 bid/ask 价格限制
    /// 冷却期到期时顺带清除最近成交记录（方案要求: 冷却后清空 last_buy/sell_fill）
    FillRetreat getFillRetreat(const std::string& code, uint64_t current_time);

    //==========================================================================
    // Statistics
    //==========================================================================

    /// Get fill history for a contract (returns pointer to RingBuffer data)
    /// Note: Returns nullptr if contract not found
    const RingBuffer<SelfFillRecord, 128>* getFillHistory(const std::string& code) const;

    /// Get sample count for a contract
    uint32_t getSampleCount(const std::string& code) const;

    //==========================================================================
    // Reset
    //==========================================================================

    void reset();
    void resetContract(const std::string& code);

    /// Reset calibration for a contract (clear history, allow fresh start after toxicity cooloff)
    void resetCalibration(const std::string& code);

    /// Apply time-based decay - reduce weight of old fills
    void decayCalibration(const std::string& code, uint64_t current_time, uint64_t decay_window_ms = 30000);

private:
    // V8-R6 收官: 跨线程收编 —— recordFill/getCalibration (TdSpi: 成交/补挂路径) 与
    // onTick/decayCalibration/getFillRetreat (MdSpi: processQuoting/checkRisk) 双线程
    // 触碰 _contract_states (wt_hashmap operator[] 结构性插入) / _retreat_states
    // (std::map 同) / RingBuffer, 字段级 atomic 不覆盖容器结构。拆回调大锁
    // (FUTU_CB_LOCK_BIG=0) 的硬阻断项, 按 UnifiedOrderTracker 模式全方法收编。
    // 叶子锁: 临界区内不调用任何外部模块, 无锁序风险; getFillRetreat 为
    // Md+Td 双调用点, 必须同一把锁。
    mutable RecursiveSpinLock _lock;

    SelfTradeCalibratorConfig _config;

    // Fill history per contract (using RingBuffer for performance)
    struct ContractFillState
    {
        RingBuffer<SelfFillRecord, 128> fill_history; // capacity must be power of 2
        double mid_price;
        uint64_t timestamp;
        mutable CalibrationResult cached_result;
        mutable bool cache_dirty;

        ContractFillState()
            : mid_price(0), timestamp(0), cache_dirty(true)
        {}
    };
    wtp::wt_hashmap<std::string, ContractFillState> _contract_states;

    /// 成交后退状态（跨线程独立容器）:
    /// recordFill (TdSpi) 写, getFillRetreat (MdSpi) 读/清。
    /// 单字段用 atomic 保证无锁读写; (price,time) 成对读写存在极小撕裂窗口,
    /// 仅影响 retreat 软控制, 可接受。std::map 为节点容器, 支持不可移动的 atomic 值。
    struct RetreatState
    {
        std::atomic<double> buy_price{0.0};
        std::atomic<uint64_t> buy_time{0};
        std::atomic<double> sell_price{0.0};
        std::atomic<uint64_t> sell_time{0};
    };
    std::map<std::string, RetreatState> _retreat_states;

    //==========================================================================
    // Internal Methods
    //==========================================================================

    /// Analyze fill records for toxicity
    void analyzeFills(const std::string& code) const;

    /// Calculate realized toxicity for a single fill

    /// Check if fill was adverse selection
    bool checkAdverse(const SelfFillRecord& fill, double current_mid) const;

    /// Prune old fills from history
    void pruneHistory(const std::string& code, uint64_t current_time);
};

} // namespace futu
