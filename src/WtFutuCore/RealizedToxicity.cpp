/*!
 * \file RealizedToxicity.cpp
 * \brief Realized Toxicity Detection Implementation
 */

#include "RealizedToxicity.h"
#include "MarketDataContext.h"
#include <cmath>

namespace futu {

RealizedToxicity::RealizedToxicity()
    : _has_calibration_data(false)
    , _has_book_data(false)
    , _cache_dirty(true)
{
}

//------------------------------------------------------------------------------
// Data Input
//------------------------------------------------------------------------------

void RealizedToxicity::onCalibration(const CalibrationResult& calibration)
{
    _latest_calibration = calibration;
    _has_calibration_data = true;
    _cache_dirty = true;
}

void RealizedToxicity::onBookAnalysis(double imbalance_score)
{
    _latest_book.imbalance_score = imbalance_score;
    _has_book_data = true;
    _cache_dirty = true;
}

//------------------------------------------------------------------------------
// Cache Update
//------------------------------------------------------------------------------

void RealizedToxicity::updateCache() const
{
    if (!_cache_dirty) return;
    
    _cached_result = RealizedToxicityResult();
    
    if (_has_calibration_data)
    {
        _cached_result.adverse_ratio = _latest_calibration.toxicity_level;
        _cached_result.total_fills = static_cast<uint32_t>(_latest_calibration.sample_size);
        _cached_result.adverse_fills = static_cast<uint32_t>(
            _latest_calibration.sample_size * _latest_calibration.toxicity_level
        );
        _cached_result.avg_adverse_move = _latest_calibration.toxicity_level * 2.0;
        _cached_result.direction_bias = _latest_calibration.direction_bias;
        
        // Confidence based on sample size
        if (_latest_calibration.sample_size >= _cfg.min_samples)
        {
            double sample_confidence = std::min(1.0, _latest_calibration.sample_size / 10.0);
            _cached_result.confidence = sample_confidence * _latest_calibration.confidence;
        }
        
        // Decayed score: apply weight and time decay
        if (_cached_result.confidence > 0)
        {
            _cached_result.decayed_score = 
                _cached_result.adverse_ratio * _cfg.weight * _cached_result.confidence;
        }
    }
    
    _cache_dirty = false;
}

//------------------------------------------------------------------------------
// Analysis
//------------------------------------------------------------------------------

RealizedToxicityResult RealizedToxicity::analyze() const
{
    updateCache();
    return _cached_result;
}

double RealizedToxicity::getToxicityScore() const
{
    updateCache();
    return _cached_result.decayed_score;
}

//------------------------------------------------------------------------------
// Reset
//------------------------------------------------------------------------------

void RealizedToxicity::reset()
{
    _has_calibration_data = false;
    _has_book_data = false;
    _cache_dirty = true;
    _cached_result = RealizedToxicityResult();
}

} // namespace futu
