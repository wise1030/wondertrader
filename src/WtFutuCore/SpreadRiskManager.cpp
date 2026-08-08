/*!
 * \file SpreadRiskManager.cpp
 * \brief Spread Risk Manager Implementation
 */
#include "SpreadRiskManager.h"
#include <cmath>
#include <algorithm>
#include <chrono>

namespace futu {

SpreadRiskManager::SpreadRiskManager()
    : _portfolio_unrealized_pnl(0)
    , _portfolio_realized_pnl(0)
    , _portfolio_peak_pnl(0)
    , _current_drawdown(0)
    , _last_alert_time(0)
    , _current_date(0)
{
}

//==============================================================================
// Expiry Date Helper Methods
//==============================================================================

int32_t SpreadRiskManager::calculateDaysBetween(uint32_t date1, uint32_t date2)
{
    // YYYYMMDD -> 精确 Gregorian 天数 (Howard Hinnant days-from-civil, 全历法正确)
    auto toDays = [](uint32_t d) -> int32_t {
        int32_t y = static_cast<int32_t>(d / 10000);
        uint32_t m = (d % 10000) / 100;
        uint32_t day = d % 100;
        y -= (m <= 2) ? 1 : 0;
        int32_t era = (y >= 0 ? y : y - 399) / 400;
        uint32_t yoe = static_cast<uint32_t>(y - era * 400);
        uint32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + day - 1;
        uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + static_cast<int32_t>(doe) - 719468;
    };

    return toDays(date2) - toDays(date1);
}

int32_t SpreadRiskManager::getDaysToExpiry(const std::string& code) const
{
    if (!_expiry_callback || _current_date == 0)
        return -1;  // Unknown

    uint32_t expiry_date = _expiry_callback(code);
    if (expiry_date == 0)
        return -1;

    return calculateDaysBetween(_current_date, expiry_date);
}

void SpreadRiskManager::updatePairState(const std::string& pair_id, const SpreadState& state)
{
    _pair_states[pair_id] = state;
    _pair_risks[pair_id] = calculatePairRisk(pair_id);

    // Update portfolio-level metrics
    updateAlerts();
}

void SpreadRiskManager::updatePortfolioPnL(double unrealized_pnl, double realized_pnl)
{
    _portfolio_unrealized_pnl = unrealized_pnl;
    _portfolio_realized_pnl = realized_pnl;

    double total_pnl = unrealized_pnl + realized_pnl;
    if (total_pnl > _portfolio_peak_pnl)
    {
        _portfolio_peak_pnl = total_pnl;
    }

    _current_drawdown = std::max(0.0, _portfolio_peak_pnl - total_pnl);
}

SpreadRiskMetrics SpreadRiskManager::calculatePairRisk(const std::string& pair_id) const
{
    SpreadRiskMetrics risk;

    auto it = _pair_states.find(pair_id);
    if (it == _pair_states.end())
        return risk;

    const auto& state = it->second;

    // Calculate exposures (signed: long > 0, short < 0)
    risk.leg1_exposure = state.leg1_position * state.leg1_price;
    risk.leg2_exposure = state.leg2_position * state.leg2_price;
    // 净方向敞口 = 符号叠加 (空头 leg2_exposure 为负, 自然对冲多头).
    // 全对冲价差 (多1000/空1000) 净敞口≈0; 旧公式 leg1 - beta*leg2 得 2000, 虚高2倍.
    risk.net_exposure = risk.leg1_exposure + state.beta * risk.leg2_exposure;

    // Calculate VaR (simplified - assumes normal distribution)
    if (state.spread_std > 0 && state.hasPosition())
    {
        // 99% VaR = 2.33 * std * position
        double position_value = std::abs(state.spread_position) * state.spread_std;
        risk.var_99 = 2.33 * position_value;
    }

    // Maximum potential loss
    risk.max_loss = risk.var_99 * 1.5;  // Conservative estimate

    // Correlation risk
    risk.correlation_risk = (state.correlation < _config.min_correlation) ?
                            (_config.min_correlation - state.correlation) : 0;

    // Beta instability
    risk.beta_instability = (state.half_life > 0) ?
                            std::min(1.0, 100.0 / state.half_life) : 1.0;

    // Convergence risk
    if (state.hasPosition())
    {
        double zscore_risk = std::abs(state.zscore) / _config.max_divergence_zscore;
        risk.convergence_risk = std::min(zscore_risk, 1.0);
    }

    // Calculate days to expiry from contract info
    if (_expiry_callback && _current_date > 0)
    {
        // Get real expiry dates from base data
        int32_t days1 = getDaysToExpiry(state.leg1_code);
        int32_t days2 = getDaysToExpiry(state.leg2_code);

        risk.days_to_expiry_leg1 = (days1 >= 0) ? static_cast<uint32_t>(days1) : 0;
        risk.days_to_expiry_leg2 = (days2 >= 0) ? static_cast<uint32_t>(days2) : 0;
    }
    else
    {
        // Fallback: use placeholder values when callback not set
        risk.days_to_expiry_leg1 = 30;
        risk.days_to_expiry_leg2 = 60;
    }

    return risk;
}

PortfolioRiskSummary SpreadRiskManager::calculatePortfolioRisk() const
{
    PortfolioRiskSummary summary;

    summary.active_pairs = 0;
    summary.pairs_at_risk = 0;
    summary.min_correlation = 1.0;
    double correlation_sum = 0;
    double liquidity_sum = 0;

    for (const auto& kv : _pair_states)
    {
        const auto& state = kv.second;

        if (state.is_active)
        {
            summary.active_pairs++;
            summary.total_position += std::abs(state.spread_position);
            summary.total_exposure += std::abs(state.leg1_position * state.leg1_price) +
                                       std::abs(state.leg2_position * state.leg2_price);
            // 流动性: 当日成交量相对最低要求的比例, 截断到 [0,1]
            liquidity_sum += std::min(1.0, state.total_volume /
                              std::max(1.0, static_cast<double>(_config.min_daily_volume)));
        }

        // Track correlation
        if (state.correlation > 0)
        {
            correlation_sum += state.correlation;
            summary.min_correlation = std::min(summary.min_correlation, state.correlation);
        }

        // Check for correlation break
        if (state.correlation < _config.max_correlation_break)
        {
            summary.correlation_breaks++;
            summary.pairs_at_risk++;
        }

        // Check for high Z-Score
        if (std::abs(state.zscore) > _config.max_divergence_zscore * 0.8)
        {
            summary.pairs_at_risk++;
        }
    }

    // Calculate average correlation
    if (summary.active_pairs > 0)
    {
        summary.avg_correlation = correlation_sum / summary.active_pairs;
    }

    // Portfolio VaR
    summary.var_99 = calculatePortfolioVaR(0.99);

    // Current drawdown
    summary.max_drawdown = _current_drawdown;

    // Liquidity score: 活跃对成交量达标率均值
    summary.liquidity_score = liquidity_sum / std::max(1u, summary.active_pairs);

    // Check stop loss
    summary.has_stop_loss = (_current_drawdown > _config.portfolio_stop_loss);

    // Check for critical alerts
    for (const auto& alert : _active_alerts)
    {
        if (alert.level == RiskAlert::Level::CRITICAL ||
            alert.level == RiskAlert::Level::EMERGENCY)
        {
            summary.has_critical_alert = true;
            break;
        }
    }

    return summary;
}

double SpreadRiskManager::calculateVaR(const std::string& pair_id, double confidence) const
{
    auto risk_it = _pair_risks.find(pair_id);
    if (risk_it != _pair_risks.end())
    {
        return risk_it->second.var_99;
    }
    return 0;
}

double SpreadRiskManager::calculatePortfolioVaR(double confidence) const
{
    // Simplified portfolio VaR (assumes independent positions)
    // In reality, should account for correlations

    double total_var_sq = 0;

    for (const auto& kv : _pair_risks)
    {
        double var = kv.second.var_99;
        total_var_sq += var * var;
    }

    return std::sqrt(total_var_sq);
}

bool SpreadRiskManager::canOpenPosition(const std::string& pair_id, double size) const
{
    return checkPositionLimits(pair_id, size);
}

bool SpreadRiskManager::checkCorrelationBreak(const std::string& pair_id) const
{
    auto it = _pair_states.find(pair_id);
    if (it == _pair_states.end())
        return false;

    return it->second.correlation < _config.max_correlation_break;
}

bool SpreadRiskManager::checkConvergenceFailure(const std::string& pair_id) const
{
    auto it = _pair_states.find(pair_id);
    if (it == _pair_states.end())
        return false;

    const auto& state = it->second;

    if (!state.hasPosition())
        return false;

    // Check if divergence is too high for too long
    // 旧代码 positionDuration(0): uint64 下溢导致 long_duration 恒真
    bool high_divergence = std::abs(state.zscore) > _config.max_divergence_zscore;
    uint64_t now_us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count());
    bool long_duration = state.positionDuration(now_us) > _config.max_divergence_time;

    return high_divergence && long_duration;
}

std::vector<RiskAlert> SpreadRiskManager::generateAlerts() const
{
    return _active_alerts;
}

double SpreadRiskManager::getAllowedPositionSize(const std::string& pair_id) const
{
    auto state_it = _pair_states.find(pair_id);
    double current_pos = (state_it != _pair_states.end()) ?
                         std::abs(state_it->second.spread_position) : 0;

    // Calculate remaining capacity
    double remaining_single = _config.max_single_pair - current_pos;

    // Check total portfolio position
    double total_pos = 0;
    for (const auto& kv : _pair_states)
    {
        total_pos += std::abs(kv.second.spread_position);
    }
    double remaining_total = _config.max_total_position - total_pos + current_pos;

    return std::max(0.0, std::min(remaining_single, remaining_total));
}

double SpreadRiskManager::getCurrentPosition(const std::string& pair_id) const
{
    auto it = _pair_states.find(pair_id);
    if (it == _pair_states.end())
        return 0;
    return it->second.spread_position;
}

void SpreadRiskManager::updateAlerts()
{
    _active_alerts.clear();

    // Check portfolio-level risks
    if (_current_drawdown > _config.portfolio_stop_loss * 0.8)
    {
        RiskAlert alert;
        alert.level = (_current_drawdown > _config.portfolio_stop_loss) ?
                       RiskAlert::Level::EMERGENCY : RiskAlert::Level::CRITICAL;
        alert.type = RiskAlert::Type::STOP_LOSS;
        alert.message = "Portfolio drawdown approaching/exceeding stop loss";
        alert.value = _current_drawdown;
        alert.threshold = _config.portfolio_stop_loss;
        _active_alerts.push_back(alert);
    }

    // Check pair-level risks
    for (const auto& kv : _pair_states)
    {
        const auto& pair_id = kv.first;
        const auto& state = kv.second;

        // Correlation break
        if (state.correlation < _config.max_correlation_break)
        {
            RiskAlert alert;
            alert.pair_id = pair_id;
            alert.level = RiskAlert::Level::WARNING;
            alert.type = RiskAlert::Type::CORRELATION_BREAK;
            alert.message = "Correlation breakdown detected";
            alert.value = state.correlation;
            alert.threshold = _config.max_correlation_break;
            _active_alerts.push_back(alert);
        }

        // Divergence
        if (std::abs(state.zscore) > _config.max_divergence_zscore * 0.8)
        {
            RiskAlert alert;
            alert.pair_id = pair_id;
            alert.level = RiskAlert::Level::WARNING;
            alert.type = RiskAlert::Type::DIVERGENCE;
            alert.message = "Spread divergence high";
            alert.value = std::abs(state.zscore);
            alert.threshold = _config.max_divergence_zscore;
            _active_alerts.push_back(alert);
        }

        // Position limit
        if (std::abs(state.spread_position) > _config.max_single_pair * 0.9)
        {
            RiskAlert alert;
            alert.pair_id = pair_id;
            alert.level = RiskAlert::Level::WARNING;
            alert.type = RiskAlert::Type::POSITION_LIMIT;
            alert.message = "Approaching position limit";
            alert.value = std::abs(state.spread_position);
            alert.threshold = _config.max_single_pair;
            _active_alerts.push_back(alert);
        }
    }
}

bool SpreadRiskManager::checkPositionLimits(const std::string& pair_id, double size) const
{
    double new_pos = getCurrentPosition(pair_id) + size;

    // Check single pair limit
    if (std::abs(new_pos) > _config.max_single_pair)
        return false;

    // Check total portfolio limit
    double total_pos = 0;
    for (const auto& kv : _pair_states)
    {
        if (kv.first != pair_id)
        {
            total_pos += std::abs(kv.second.spread_position);
        }
    }
    total_pos += std::abs(new_pos);

    if (total_pos > _config.max_total_position)
        return false;

    return true;
}

void SpreadRiskManager::reset()
{
    _pair_states.clear();
    _pair_risks.clear();
    _portfolio_unrealized_pnl = 0;
    _portfolio_realized_pnl = 0;
    _portfolio_peak_pnl = 0;
    _current_drawdown = 0;
    _active_alerts.clear();
    _last_alert_time = 0;
}

} // namespace futu
