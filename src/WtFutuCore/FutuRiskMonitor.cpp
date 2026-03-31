/*!
 * \file FutuRiskMonitor.cpp
 * \brief Simplified Risk Monitoring Implementation
 * 
 * Uses atomic counters for lock-free rate tracking.
 * Integrates with EventNotifier for risk alert broadcasting.
 */
#include "FutuRiskMonitor.h"
#include "FutuPortfolio.h"
#include "../WTSTools/WTSLogger.h"
#include "../WtUftCore/EventNotifier.h"
#include <algorithm>

namespace futu {

FutuRiskMonitor::FutuRiskMonitor()
    : _event_notifier(nullptr)
{
}

void FutuRiskMonitor::recordOrder()
{
    uint64_t now = _current_time.load(std::memory_order_relaxed);
    _order_times.push(now);
    
    // Count timestamps within last 1000ms
    uint64_t cutoff = (now > 1000) ? (now - 1000) : 0;
    uint32_t count = 0;
    for (size_t i = 0; i < _order_times.size(); ++i)
    {
        if (_order_times[i] >= cutoff)
            ++count;
    }
    _orders_last_sec.store(count, std::memory_order_relaxed);
}

void FutuRiskMonitor::recordCancel()
{
    uint64_t now = _current_time.load(std::memory_order_relaxed);
    _cancel_times.push(now);
    
    // Count timestamps within last 1000ms
    uint64_t cutoff = (now > 1000) ? (now - 1000) : 0;
    uint32_t count = 0;
    for (size_t i = 0; i < _cancel_times.size(); ++i)
    {
        if (_cancel_times[i] >= cutoff)
            ++count;
    }
    _cancels_last_sec.store(count, std::memory_order_relaxed);
}

void FutuRiskMonitor::recordTrade()
{
    uint64_t now = _current_time.load(std::memory_order_relaxed);
    _trade_times.push(now);
    
    // Count timestamps within last 1000ms
    uint64_t cutoff = (now > 1000) ? (now - 1000) : 0;
    uint32_t count = 0;
    for (size_t i = 0; i < _trade_times.size(); ++i)
    {
        if (_trade_times[i] >= cutoff)
            ++count;
    }
    _trades_last_sec.store(count, std::memory_order_relaxed);
}

void FutuRiskMonitor::broadcastAlert(const std::string& alertType, const std::string& message)
{
    // Log the alert
    WTSLogger::warn("[RISK] {}: {}", alertType, message);
    
    // Broadcast via EventNotifier if available
    if (_event_notifier)
    {
        _event_notifier->notify(alertType.c_str(), message.c_str());
    }
}

std::vector<RiskViolation> FutuRiskMonitor::checkRiskLimits(const FutuPortfolio* portfolio)
{
    std::vector<RiskViolation> violations;
    
    if (!portfolio)
        return violations;
    
    const PortfolioParams& params = portfolio->getParams();
    uint64_t ts = _current_time.load(std::memory_order_relaxed);
    
    // Check delta limit
    double delta = portfolio->getTotalDelta();
    double absDelta = std::abs(delta);
    if (absDelta > params.delta_limit)
    {
        RiskViolation v;
        v.type = RiskLimitType::DELTA;
        v.current_value = absDelta;
        v.limit_value = params.delta_limit;
        v.utilization = absDelta / params.delta_limit;
        v.severity = v.utilization > 1.0 ? RiskSeverity::BREACH : RiskSeverity::WARNING;
        v.timestamp = ts;
        v.message = fmt::format("Delta limit exceeded: {:.2f} > {:.2f}", absDelta, params.delta_limit);
        violations.push_back(v);
        
        if (v.severity == RiskSeverity::BREACH)
            broadcastAlert("DELTA_BREACH", v.message);
    }
    
    // Check max delta
    if (absDelta > params.max_delta)
    {
        RiskViolation v;
        v.type = RiskLimitType::DELTA;
        v.current_value = absDelta;
        v.limit_value = params.max_delta;
        v.utilization = absDelta / params.max_delta;
        v.severity = RiskSeverity::CRITICAL;
        v.timestamp = ts;
        v.message = fmt::format("Maximum delta breached: {:.2f} > {:.2f}", absDelta, params.max_delta);
        violations.push_back(v);
        
        broadcastAlert("DELTA_CRITICAL", v.message);
    }
    
    // Check exposure
    double exposure = portfolio->getTotalExposure();
    if (exposure > params.max_exposure)
    {
        RiskViolation v;
        v.type = RiskLimitType::EXPOSURE;
        v.current_value = exposure;
        v.limit_value = params.max_exposure;
        v.utilization = exposure / params.max_exposure;
        v.severity = v.utilization > 1.0 ? RiskSeverity::BREACH : RiskSeverity::WARNING;
        v.timestamp = ts;
        v.message = fmt::format("Exposure limit exceeded: {:.2f} > {:.2f}", exposure, params.max_exposure);
        violations.push_back(v);
        
        if (v.severity == RiskSeverity::BREACH)
            broadcastAlert("EXPOSURE_BREACH", v.message);
    }
    
    // Check daily loss
    double pnl = portfolio->getTotalPnL();
    if (pnl < -params.max_loss)
    {
        RiskViolation v;
        v.type = RiskLimitType::DAILY_LOSS;
        v.current_value = pnl;
        v.limit_value = -params.max_loss;
        v.utilization = std::abs(pnl) / params.max_loss;
        v.severity = RiskSeverity::CRITICAL;
        v.timestamp = ts;
        v.message = fmt::format("Daily loss limit breached: {:.2f} < -{:.2f}", pnl, params.max_loss);
        violations.push_back(v);
        
        broadcastAlert("LOSS_CRITICAL", v.message);
    }
    
    // Check rate limits (using atomic values)
    uint32_t orders = _orders_last_sec.load(std::memory_order_relaxed);
    uint32_t cancels = _cancels_last_sec.load(std::memory_order_relaxed);
    uint32_t trades = _trades_last_sec.load(std::memory_order_relaxed);
    
    if (orders > _rate_limits.max_orders_per_sec)
    {
        RiskViolation v;
        v.type = RiskLimitType::ORDER_RATE;
        v.current_value = orders;
        v.limit_value = _rate_limits.max_orders_per_sec;
        v.utilization = (double)orders / _rate_limits.max_orders_per_sec;
        v.severity = RiskSeverity::WARNING;
        v.timestamp = ts;
        v.message = fmt::format("Order rate limit exceeded: {} > {} per sec", orders, _rate_limits.max_orders_per_sec);
        violations.push_back(v);
    }
    
    if (cancels > _rate_limits.max_cancels_per_sec)
    {
        RiskViolation v;
        v.type = RiskLimitType::CANCEL_RATE;
        v.current_value = cancels;
        v.limit_value = _rate_limits.max_cancels_per_sec;
        v.utilization = (double)cancels / _rate_limits.max_cancels_per_sec;
        v.severity = RiskSeverity::WARNING;
        v.timestamp = ts;
        v.message = fmt::format("Cancel rate limit exceeded: {} > {} per sec", cancels, _rate_limits.max_cancels_per_sec);
        violations.push_back(v);
    }
    
    if (trades > _rate_limits.max_trades_per_sec)
    {
        RiskViolation v;
        v.type = RiskLimitType::TRADE_RATE;
        v.current_value = trades;
        v.limit_value = _rate_limits.max_trades_per_sec;
        v.utilization = (double)trades / _rate_limits.max_trades_per_sec;
        v.severity = RiskSeverity::WARNING;
        v.timestamp = ts;
        v.message = fmt::format("Trade rate limit exceeded: {} > {} per sec", trades, _rate_limits.max_trades_per_sec);
        violations.push_back(v);
    }
    
    // Check for single contract POSITION limit breaches (持仓手数限制)
    const ContractState* pos_breached = portfolio->getPositionBreachedContract();
    if (pos_breached)
    {
        RiskViolation v;
        v.type = RiskLimitType::POSITION_NET;
        v.code = pos_breached->code;
        v.current_value = pos_breached->position;
        v.limit_value = pos_breached->max_position;
        v.utilization = pos_breached->max_position > 0 ? 
            std::abs(pos_breached->position) / pos_breached->max_position : 1.0;
        v.severity = RiskSeverity::BREACH;
        v.timestamp = ts;
        v.message = fmt::format("Contract {} POSITION limit breached: {} (max {})", 
            pos_breached->code, pos_breached->position, pos_breached->max_position);
        violations.push_back(v);
        
        broadcastAlert("POSITION_BREACH", v.message);
    }
    
    // Check for single contract DELTA limit breaches (Delta Cash 限制)
    const ContractState* delta_breached = portfolio->getDeltaBreachedContract();
    if (delta_breached)
    {
        RiskViolation v;
        v.type = RiskLimitType::DELTA;  // 使用 DELTA 类型
        v.code = delta_breached->code;
        v.current_value = delta_breached->delta();
        v.limit_value = delta_breached->max_delta;
        v.utilization = delta_breached->max_delta > 0 ?
            std::abs(delta_breached->delta()) / delta_breached->max_delta : 1.0;
        v.severity = RiskSeverity::BREACH;
        v.timestamp = ts;
        v.message = fmt::format("Contract {} DELTA limit breached: {:.2f} (max {:.2f})", 
            delta_breached->code, delta_breached->delta(), delta_breached->max_delta);
        violations.push_back(v);
        
        broadcastAlert("CONTRACT_DELTA_BREACH", v.message);
    }
    
    return violations;
}

bool FutuRiskMonitor::checkRateLimits() const
{
    return _orders_last_sec.load(std::memory_order_relaxed) < _rate_limits.max_orders_per_sec &&
           _cancels_last_sec.load(std::memory_order_relaxed) < _rate_limits.max_cancels_per_sec &&
           _trades_last_sec.load(std::memory_order_relaxed) < _rate_limits.max_trades_per_sec;
}

RiskAction FutuRiskMonitor::determineAction(const std::vector<RiskViolation>& violations) const
{
    RiskCategory category;
    return determineActionWithCategory(violations, category);
}

RiskAction FutuRiskMonitor::determineActionWithCategory(const std::vector<RiskViolation>& violations,
                                                         RiskCategory& outCategory) const
{
    outCategory = RiskCategory::REVERSIBLE;  // Default: reversible
    
    if (violations.empty())
        return RiskAction::NONE;
    
    // 1. Check for irreversible risks (daily loss) - requires manual intervention
    for (const auto& v : violations)
    {
        if (v.type == RiskLimitType::DAILY_LOSS && v.severity == RiskSeverity::CRITICAL)
        {
            outCategory = RiskCategory::IRREVERSIBLE;
            return RiskAction::HALT_TRADING;
        }
    }
    
    // 2. Check for critical reversible risks (max delta, max exposure)
    for (const auto& v : violations)
    {
        if (v.severity == RiskSeverity::CRITICAL)
        {
            // These are reversible - trading can resume after risk normalizes
            outCategory = RiskCategory::REVERSIBLE;
            return RiskAction::HALT_TRADING;
        }
    }
    
    // 3. Check for position direction blocks
    bool long_breach = false;
    bool short_breach = false;
    bool contract_breach = false;
    
    for (const auto& v : violations)
    {
        if (v.severity < RiskSeverity::BREACH)
            continue;
            
        if (v.type == RiskLimitType::POSITION_LONG)
            long_breach = true;
        else if (v.type == RiskLimitType::POSITION_SHORT)
            short_breach = true;
        else if (v.type == RiskLimitType::DELTA || v.type == RiskLimitType::EXPOSURE)
        {
            // Positive delta/exposure breach means too much long
            // Negative delta breach means too much short
            if (v.current_value > 0)
                long_breach = true;
            else
                short_breach = true;
        }
        else if (v.type == RiskLimitType::POSITION_NET)
        {
            // Single contract position breach - handled specifically, no global block
            contract_breach = true;
        }
    }
    
    // If both directions blocked, pause quoting
    if (long_breach && short_breach)
    {
        outCategory = RiskCategory::REVERSIBLE;
        return RiskAction::PAUSE_QUOTING;
    }
    
    // Block specific direction
    if (long_breach)
    {
        outCategory = RiskCategory::REVERSIBLE;
        return RiskAction::BLOCK_SIDE_LONG;
    }
    if (short_breach)
    {
        outCategory = RiskCategory::REVERSIBLE;
        return RiskAction::BLOCK_SIDE_SHORT;
    }
    
    // Check specific contract breach
    if (contract_breach)
    {
        outCategory = RiskCategory::REVERSIBLE;
        return RiskAction::BLOCK_CONTRACT_OPENING;
    }
    
    // 4. Count remaining breaches for tiered response
    int breachCount = 0;
    for (const auto& v : violations)
    {
        if (v.severity == RiskSeverity::BREACH)
            breachCount++;
    }
    
    if (breachCount >= 3)
    {
        outCategory = RiskCategory::REVERSIBLE;
        return RiskAction::FLATTEN_POSITION;
    }
    if (breachCount >= 2)
    {
        outCategory = RiskCategory::REVERSIBLE;
        return RiskAction::PAUSE_QUOTING;
    }
    if (breachCount >= 1)
    {
        outCategory = RiskCategory::REVERSIBLE;
        return RiskAction::WIDEN_SPREAD;
    }
    
    // Only warnings
    outCategory = RiskCategory::REVERSIBLE;
    return RiskAction::WARN;
}

void FutuRiskMonitor::haltTrading(RiskCategory category)
{
    _trading_halted.store(true, std::memory_order_relaxed);
    _halt_category = category;
    _halt_timestamp = _current_time.load(std::memory_order_relaxed);
    
    const char* category_str = (category == RiskCategory::IRREVERSIBLE) ? "IRREVERSIBLE" : "REVERSIBLE";
    broadcastAlert("TRADING_HALTED", 
        fmt::format("Trading halted ({}) due to risk limits", category_str));
}

void FutuRiskMonitor::resumeTrading()
{
    // Only allow resume for reversible risks
    if (_halt_category == RiskCategory::IRREVERSIBLE)
    {
        WTSLogger::warn("[RISK] Cannot resume trading: halt is IRREVERSIBLE (requires manual intervention)");
        return;
    }
    
    _trading_halted.store(false, std::memory_order_relaxed);
    _halt_timestamp = 0;
    broadcastAlert("TRADING_RESUMED", "Trading resumed after risk normalized");
}

void FutuRiskMonitor::pauseQuoting()
{
    _quoting_paused.store(true, std::memory_order_relaxed);
    _pause_timestamp = _current_time.load(std::memory_order_relaxed);
    broadcastAlert("QUOTING_PAUSED", "Quoting paused due to risk limits");
}

void FutuRiskMonitor::resumeQuoting()
{
    _quoting_paused.store(false, std::memory_order_relaxed);
    _pause_timestamp = 0;
    broadcastAlert("QUOTING_RESUMED", "Quoting resumed after risk normalized");
}

bool FutuRiskMonitor::canRecover(const FutuPortfolio* portfolio) const
{
    if (!portfolio)
        return false;
    
    // Irreversible risks cannot auto-recover
    if (_halt_category == RiskCategory::IRREVERSIBLE)
        return false;
    
    uint64_t now = _current_time.load(std::memory_order_relaxed);
    
    // Check cooldown period
    if (_trading_halted.load(std::memory_order_relaxed))
    {
        if (now - _halt_timestamp < _recovery_config.cooldown_ms)
            return false;
    }
    
    if (_quoting_paused.load(std::memory_order_relaxed))
    {
        if (now - _pause_timestamp < _recovery_config.cooldown_ms)
            return false;
    }
    
    // Check if risk utilization is below recovery threshold
    const PortfolioParams& params = portfolio->getParams();
    
    // Check delta utilization
    double delta_util = portfolio->getDeltaUtilization();
    if (delta_util > _recovery_config.recovery_threshold)
        return false;
    
    // Check exposure utilization
    double exposure = portfolio->getTotalExposure();
    if (params.max_exposure > 0)
    {
        double exposure_util = exposure / params.max_exposure;
        if (exposure_util > _recovery_config.recovery_threshold)
            return false;
    }
    
    // Check if any contract limit is breached
    if (portfolio->isAnyContractLimitBreached())
        return false;
    
    return true;
}

bool FutuRiskMonitor::checkAndRecover(const FutuPortfolio* portfolio)
{
    uint64_t now = _current_time.load(std::memory_order_relaxed);
    
    // Throttle recovery checks
    if (now - _last_recovery_check < _recovery_config.check_interval_ms)
        return false;
    
    _last_recovery_check = now;
    
    // Check if we can recover
    if (!canRecover(portfolio))
        return false;
    
    // Perform recovery
    bool recovered = false;
    
    // Resume trading if halted
    if (_trading_halted.load(std::memory_order_relaxed) && 
        _halt_category == RiskCategory::REVERSIBLE)
    {
        resumeTrading();
        recovered = true;
    }
    
    // Resume quoting if paused
    if (_quoting_paused.load(std::memory_order_relaxed))
    {
        resumeQuoting();
        recovered = true;
    }
    
    // Unblock directions if position normalized
    if (_long_blocked.load(std::memory_order_relaxed) ||
        _short_blocked.load(std::memory_order_relaxed))
    {
        // Check if positions have normalized
        const ContractState* breached = portfolio ? portfolio->getBreachedContract() : nullptr;
        if (!breached)
        {
            unblockLong();
            unblockShort();
            recovered = true;
        }
    }
    
    return recovered;
}

void FutuRiskMonitor::resetDaily()
{
    _trading_halted.store(false, std::memory_order_relaxed);
    _long_blocked.store(false, std::memory_order_relaxed);
    _short_blocked.store(false, std::memory_order_relaxed);
    _quoting_paused.store(false, std::memory_order_relaxed);
    _halt_category = RiskCategory::REVERSIBLE;
    _halt_timestamp = 0;
    _pause_timestamp = 0;
    _last_recovery_check = 0;
    
    _order_times.clear();
    _cancel_times.clear();
    _trade_times.clear();
    _orders_last_sec.store(0, std::memory_order_relaxed);
    _cancels_last_sec.store(0, std::memory_order_relaxed);
    _trades_last_sec.store(0, std::memory_order_relaxed);
}

void FutuRiskMonitor::resetSession()
{
    resetDaily();
}

//==========================================================================
// Closeout Management (收盘前平仓)
//==========================================================================

bool FutuRiskMonitor::checkCloseout(uint32_t currentTime, uint32_t closeTime)
{
    // 如果未启用收盘前平仓
    if (_closeout_config.minutes_before == 0)
        return false;
    
    // 如果已完成，不再触发
    if (_closeout_completed.load(std::memory_order_relaxed))
        return false;
    
    // 计算平仓触发时间
    // closeTime 格式为 HHMMSS，需要转换为 HHMM 进行减法
    uint32_t closeHourMin = closeTime / 100;  // HHMM
    uint32_t closeoutTime = closeHourMin - _closeout_config.minutes_before;
    
    // 当前时间 (HHMM)
    uint32_t currentHourMin = currentTime / 100;
    
    // 检查是否到达触发时间
    if (currentHourMin >= closeoutTime && !_closeout_triggered.load(std::memory_order_relaxed))
    {
        _closeout_triggered.store(true, std::memory_order_relaxed);
        pauseQuoting();  // 停止报价
        
        broadcastAlert("CLOSEOUT_TRIGGERED", 
            fmt::format("Closeout triggered at {}, close time {}", currentHourMin, closeHourMin));
        
        return true;
    }
    
    return _closeout_triggered.load(std::memory_order_relaxed);
}

void FutuRiskMonitor::resetCloseout()
{
    _closeout_triggered.store(false, std::memory_order_relaxed);
    _closeout_completed.store(false, std::memory_order_relaxed);
}

} // namespace futu
