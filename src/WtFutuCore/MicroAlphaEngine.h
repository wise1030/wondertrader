/*!
 * \file MicroAlphaEngine.h
 * \brief Legacy Alpha Types (Backward Compatibility)
 * 
 * This file provides type definitions for backward compatibility.
 * The actual alpha computation has been replaced by SignalAggregator.
 * 
 * Types defined here:
 *   - AlphaResult: Alpha signal result
 *   - TradeImbalanceResult: Trade imbalance result
 */
#pragma once

#include <cstdint>

namespace futu {

/// Legacy alpha result structure
struct AlphaResult
{
    double alpha;               ///< Alpha signal [-1, 1]
    double confidence;          ///< Confidence [0, 1]
    double ofi_component;       ///< OFI contribution
    double trade_component;     ///< Trade flow contribution
    bool is_strong_signal;      ///< Strong signal flag
    uint64_t timestamp;         ///< Timestamp
    
    AlphaResult()
        : alpha(0), confidence(0), ofi_component(0), trade_component(0)
        , is_strong_signal(false), timestamp(0) {}
};

/// Legacy trade imbalance result structure
struct TradeImbalanceResult
{
    double net_flow;            ///< Net trade flow
    double imbalance_ratio;     ///< Imbalance ratio [-1, 1]
    double large_trade_ratio;   ///< Large trade ratio [0, 1]
    uint64_t timestamp;         ///< Timestamp
    
    TradeImbalanceResult()
        : net_flow(0), imbalance_ratio(0), large_trade_ratio(0), timestamp(0) {}
};

} // namespace futu
