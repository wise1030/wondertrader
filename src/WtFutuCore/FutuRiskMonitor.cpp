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
    
    // Clean up expired ones
    uint64_t cutoff = (now > 1000) ? (now - 1000) : 0;
    while (auto item = _order_times.try_peek())
    {
        if (*item < cutoff)
        {
            _order_times.try_pop();
            _orders_last_sec.fetch_sub(1, std::memory_order_relaxed);
        }
        else
        {
            break;
        }
    }
    
    // Add new timestamp
    if (_order_times.try_push(now))
    {
        _orders_last_sec.fetch_add(1, std::memory_order_relaxed);
    }
}

void FutuRiskMonitor::recordCancel()
{
    uint64_t now = _current_time.load(std::memory_order_relaxed);
    
    // Clean up expired ones
    uint64_t cutoff = (now > 1000) ? (now - 1000) : 0;
    while (auto item = _cancel_times.try_peek())
    {
        if (*item < cutoff)
        {
            _cancel_times.try_pop();
            _cancels_last_sec.fetch_sub(1, std::memory_order_relaxed);
        }
        else
        {
            break;
        }
    }
    
    // Add new timestamp
    if (_cancel_times.try_push(now))
    {
        _cancels_last_sec.fetch_add(1, std::memory_order_relaxed);
    }
}

void FutuRiskMonitor::recordTrade()
{
    uint64_t now = _current_time.load(std::memory_order_relaxed);
    
    // Clean up expired ones
    uint64_t cutoff = (now > 1000) ? (now - 1000) : 0;
    while (auto item = _trade_times.try_peek())
    {
        if (*item < cutoff)
        {
            _trade_times.try_pop();
            _trades_last_sec.fetch_sub(1, std::memory_order_relaxed);
        }
        else
        {
            break;
        }
    }
    
    // Add new timestamp
    if (_trade_times.try_push(now))
    {
        _trades_last_sec.fetch_add(1, std::memory_order_relaxed);
    }
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
    
    //==========================================================================
    // 软指标：Delta 相关（用于 skew 偏移和对冲决策，不触发风控动作）
    // 利用率通过 SpreadOptimizer 的 computeDeltaAwareSkew 自动调整报价
    //==========================================================================
    double delta = portfolio->getTotalDelta();
    double absDelta = std::abs(delta);
    
    // Delta utilization warning (软指标，仅日志)
    if (params.portfolio_max_delta > 0)
    {
        double delta_utilization = absDelta / params.portfolio_max_delta;
        
        // 高利用率警告（>= 80%）
        if (delta_utilization >= 0.8)
        {
            WTSLogger::warn("[RISK] Delta utilization high: {:.1f}% ({:.2f} / {:.2f}) - skew will adjust quotes",
                delta_utilization * 100, absDelta, params.portfolio_max_delta);
        }
        
        // 超限警告（>= 100%），但不作为违规
        if (delta_utilization >= 1.0)
        {
            WTSLogger::warn("[RISK] Delta limit exceeded: {:.2f} > {:.2f} (soft limit, skew handling)",
                absDelta, params.portfolio_max_delta);
        }
        
        // portfolio_max_delta 是绝对上限，超过时输出严重警告但仍不 block
        // 让 skew 和对冲机制尝试回归
        if (absDelta > params.portfolio_max_delta * 1.5)  // 严重超限
        {
            WTSLogger::error("[RISK] Delta critically high: {:.2f} > {:.2f} (portfolio_max_delta * 1.5, urgent skew/hedge needed)",
                absDelta, params.portfolio_max_delta * 1.5);
        }
    }
    
    //==========================================================================
    // 硬指标：Exposure（不得突破，严格风控）
    //==========================================================================
    double exposure = portfolio->getTotalExposure();
    if (exposure > params.max_exposure)
    {
        RiskViolation v;
        v.type = RiskLimitType::EXPOSURE;
        // Store signed delta for direction detection (exposure itself is always positive)
        v.current_value = delta;  // Use delta's sign to determine breach direction
        v.limit_value = params.max_exposure;
        v.utilization = exposure / params.max_exposure;
        v.severity = v.utilization > 1.0 ? RiskSeverity::BREACH : RiskSeverity::WARNING;
        v.timestamp = ts;
        v.message = fmt::format("Exposure limit exceeded: {:.2f} > {:.2f} (delta={:.2f})", 
            exposure, params.max_exposure, delta);
        violations.push_back(v);
        
        if (v.severity == RiskSeverity::BREACH)
            broadcastAlert("EXPOSURE_BREACH", v.message);
    }
    
    //==========================================================================
    // 硬指标：Daily Loss（不可逆，需人工干预）
    //==========================================================================
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
    
    // 注意：单合约 Delta 是软指标，不产生 violation
    // Delta 超限时通过 skew 偏移和日志警告处理，不进行风控 block
    
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
    // Note: We need to distinguish long vs short breaches to block the correct direction
    // - positive position/long delta -> too much long -> block long (no more buying)
    // - negative position/short delta -> too much short -> block short (no more selling)
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
            // Single contract position breach - distinguish direction by position sign
            // v.current_value contains the signed position (positive=long, negative=short)
            if (v.current_value > 0)
                long_breach = true;   // 多头超限，block 开多
            else
                short_breach = true;  // 空头超限，block 开空
            contract_breach = true;   // Also mark for potential contract-specific handling
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

void FutuRiskMonitor::haltTrading(RiskCategory category, double pnl_snapshot)
{
    _trading_halted.store(true, std::memory_order_relaxed);
    _halt_category = category;
    _halt_timestamp = _current_time.load(std::memory_order_relaxed);
    _halt_pnl_snapshot = pnl_snapshot;
    _was_loss_triggered = (category == RiskCategory::IRREVERSIBLE) && (pnl_snapshot < 0);
    _recovery_count = 0;  // Reset recovery count on new halt
    
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
    
    // Check max recovery count
    if (_recovery_count >= _recovery_config.max_recovery_count)
    {
        WTSLogger::warn("[RISK] Max recovery count ({}) reached, manual intervention required", 
            _recovery_config.max_recovery_count);
        return false;
    }
    
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
    
    // Check delta utilization (软指标)
    double delta_util = portfolio->getPortfolioDeltaUtilization();
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
    
    // Enhanced: Check PnL recovery if halt was triggered by loss
    if (_was_loss_triggered)
    {
        double current_pnl = portfolio->getTotalPnL();
        double loss_at_halt = -_halt_pnl_snapshot;  // Negative value
        double current_loss = -current_pnl;
        
        // Must recover at least pnl_recovery_ratio of the loss
        if (current_loss > loss_at_halt * (1.0 - _recovery_config.pnl_recovery_ratio))
        {
            WTSLogger::debug("[RISK] PnL not recovered enough: loss_at_halt={:.2f}, current_loss={:.2f}",
                loss_at_halt, current_loss);
            return false;
        }
    }
    
    // P1-3.3: Check max loss threshold for recovery
    // If the loss at halt exceeds max_loss_for_recovery, block auto-recovery
    if (_recovery_config.max_loss_for_recovery != 0)
    {
        double halted_loss = std::abs(_halt_pnl_snapshot);
        if (halted_loss > std::abs(_recovery_config.max_loss_for_recovery))
        {
            WTSLogger::warn("[RISK] Loss at halt ({:.2f}) exceeds max for recovery ({:.2f}), manual intervention required",
                halted_loss, std::abs(_recovery_config.max_loss_for_recovery));
            return false;
        }
    }
    
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
        _recovery_count++;
        WTSLogger::info("[RISK] Auto-recovery #{}/{}: Trading resumed", 
            _recovery_count, _recovery_config.max_recovery_count);
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
        const ContractState* breached = portfolio ? portfolio->getPositionBreachedContract() : nullptr;
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
// Closeout Management (收盘前平仓) - State Machine Implementation
//==========================================================================

bool FutuRiskMonitor::transitionCloseoutState(CloseoutState next_state, uint64_t timestamp)
{
    if (!_closeout_state.canTransitionTo(next_state))
    {
        WTSLogger::warn("[CLOSEOUT] Invalid state transition: {} -> {}",
            static_cast<int>(_closeout_state.state), static_cast<int>(next_state));
        return false;
    }
    
    _closeout_state.state = next_state;
    
    switch (next_state)
    {
        case CloseoutState::TRIGGERED:
            _closeout_state.trigger_time = timestamp;
            break;
        case CloseoutState::FLATTENING:
            _closeout_state.flatten_start = timestamp;
            break;
        case CloseoutState::COMPLETED:
            _closeout_state.complete_time = timestamp;
            break;
        default:
            break;
    }
    
    return true;
}

void FutuRiskMonitor::markCloseoutTriggered(uint64_t timestamp)
{
    if (transitionCloseoutState(CloseoutState::TRIGGERED, timestamp))
    {
        pauseQuoting();  // 停止报价
        broadcastAlert("CLOSEOUT_TRIGGERED", 
            fmt::format("Closeout state: TRIGGERED at {}", timestamp));
    }
}

void FutuRiskMonitor::markCloseoutFlattening(uint64_t timestamp)
{
    if (transitionCloseoutState(CloseoutState::FLATTENING, timestamp))
    {
        broadcastAlert("CLOSEOUT_FLATTENING", 
            fmt::format("Closeout state: FLATTENING at {}", timestamp));
    }
}

void FutuRiskMonitor::markCloseoutCompleted(uint64_t timestamp)
{
    if (transitionCloseoutState(CloseoutState::COMPLETED, timestamp))
    {
        broadcastAlert("CLOSEOUT_COMPLETED", 
            fmt::format("Closeout state: COMPLETED at {}", timestamp));
    }
}

bool FutuRiskMonitor::checkCloseout(uint32_t currentTime, uint32_t closeTime)
{
    // 如果未启用收盘前平仓
    if (_closeout_config.minutes_before == 0)
        return false;
    
    // 如果已完成，不再触发
    if (_closeout_state.state == CloseoutState::COMPLETED)
        return false;
    
    // 计算收盘时间 (转换为分钟数)
    uint32_t closeHour, closeMin;
    if (closeTime < 10000)
    {
        // HHMM format (e.g., 1515)
        closeHour = closeTime / 100;
        closeMin = closeTime % 100;
    }
    else
    {
        // HHMMSS format (e.g., 150000), extract HHMM
        closeHour = closeTime / 10000;
        closeMin = (closeTime / 100) % 100;
    }
    
    // Validate format
    if (closeHour > 23 || closeMin > 59)
    {
        WTSLogger::warn("[RISK] Invalid close time format: {}, using default 15:15", closeTime);
        closeHour = 15;
        closeMin = 15;
    }
    
    // 计算收盘总分钟数
    uint32_t closeTotalMin = closeHour * 60 + closeMin;
    
    // 计算触发时间 (总分钟数)
    uint32_t triggerTotalMin = closeTotalMin - _closeout_config.minutes_before;
    
    // 当前时间 (HHMMSS -> 总分钟数)
    uint32_t currentHour = currentTime / 10000;
    uint32_t currentMin = (currentTime / 100) % 100;
    uint32_t currentTotalMin = currentHour * 60 + currentMin;
    
    // 检查是否到达触发时间 (only from IDLE state)
    if (currentTotalMin >= triggerTotalMin && _closeout_state.state == CloseoutState::IDLE)
    {
        markCloseoutTriggered(currentTime * 100);  // Convert to timestamp format
        
        broadcastAlert("CLOSEOUT_TRIGGERED", 
            fmt::format("Closeout triggered at {}:{:02d}, close time {}:{:02d}, {} minutes before",
                currentHour, currentMin, closeHour, closeMin, _closeout_config.minutes_before));
        
        return true;
    }
    
    return _closeout_state.state != CloseoutState::IDLE;
}

void FutuRiskMonitor::resetCloseout()
{
    _closeout_state = CloseoutStateInfo();  // Reset to IDLE state
}

} // namespace futu
