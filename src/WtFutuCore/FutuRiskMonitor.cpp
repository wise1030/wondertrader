/*!
 * \file FutuRiskMonitor.cpp
 * \brief Simplified Risk Monitoring Implementation
 * 
 * Uses atomic counters for lock-free rate tracking.
 * Integrates with EventNotifier for risk alert broadcasting.
 */
#include "FutuRiskMonitor.h"
#include "FutuPortfolio.h"
#include "UnifiedOrderTracker.h"
#include "../WTSTools/WTSLogger.h"
#include "../WtUftCore/EventNotifier.h"
#include <algorithm>

namespace futu {

FutuRiskMonitor::FutuRiskMonitor()
    : _event_notifier(nullptr)
    , _delta_snapshot_count(0)
    , _delta_snapshot_head(0)
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
            // 防止计数器下溢 — 只有当计数>0时才减
            int32_t cur = _orders_last_sec.load(std::memory_order_relaxed);
            if (cur > 0) {
                _orders_last_sec.fetch_sub(1, std::memory_order_relaxed);
            }
        }
        else
        {
            break;
        }
    }
    
    // Add new timestamp
    // try_push失败时仍然增加计数器
    // 原代码只在try_push成功时增加_orders_last_sec，但ring buffer满时订单仍然发生了，
    // 计数偏低会导致流控判断失误（以为订单频率低，实际已超限）。
    // try_push失败仅意味着旧时间戳无法被覆盖存储，不代表订单不存在。
    _order_times.try_push(now);
    _orders_last_sec.fetch_add(1, std::memory_order_relaxed);
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
            // 防止计数器下溢
            int32_t cur = _cancels_last_sec.load(std::memory_order_relaxed);
            if (cur > 0) {
                _cancels_last_sec.fetch_sub(1, std::memory_order_relaxed);
            }
        }
        else
        {
            break;
        }
    }
    
    // Add new timestamp
    // try_push失败时仍然增加计数器(与recordOrder同理)
    _cancel_times.try_push(now);
    _cancels_last_sec.fetch_add(1, std::memory_order_relaxed);
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
            // 防止计数器下溢
            int32_t cur = _trades_last_sec.load(std::memory_order_relaxed);
            if (cur > 0) {
                _trades_last_sec.fetch_sub(1, std::memory_order_relaxed);
            }
        }
        else
        {
            break;
        }
    }
    
    // Add new timestamp
    // try_push失败时仍然增加计数器(与recordOrder同理)
    _trade_times.try_push(now);
    _trades_last_sec.fetch_add(1, std::memory_order_relaxed);
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
    violations.reserve(6);  // P-5: 预分配，避免6次push_back的重分配
    
    if (!portfolio)
        return violations;
    
    const PortfolioParams& params = portfolio->getParams();
    uint64_t ts = _current_time.load(std::memory_order_relaxed);
    
    //==========================================================================
    // Delta 相关：软指标 + 硬限制
    // 软指标(<=delta_critical_mult)：用于 skew 偏移和对冲决策，仅日志
    // 硬限制(>delta_critical_mult)：产生 DELTA BREACH violation，触发风控动作
    // 利用率通过 SpreadOptimizer 的 computeDeltaAwareSkew 自动调整报价
    //==========================================================================
    double delta = portfolio->getTotalDelta();
    double absDelta = std::abs(delta);
    
    recordDeltaSnapshot(delta, ts);
    checkAndHandleDeltaRateBreach();
    
    if (params.portfolio_max_delta > 0)
    {
        double delta_utilization = absDelta / params.portfolio_max_delta;
        
        // 高利用率警告（>= 80%）
        if (delta_utilization >= _rate_limits.delta_warning_mult)
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
        
        // 软指标：超过 delta_critical_mult 倍时输出严重警告
        // max_delta 是软风控，仅用于调节 skew 和对冲决策，不触发硬风控动作
        // 真正的硬限制由 Exposure 和 Daily Loss 承担
        if (absDelta > params.portfolio_max_delta * _rate_limits.delta_critical_mult)
        {
            WTSLogger::error("[RISK] Delta critically high: {:.2f} > {:.2f} (portfolio_max_delta * {:.1f}, skew/hedge handling)",
                absDelta, params.portfolio_max_delta * _rate_limits.delta_critical_mult, _rate_limits.delta_critical_mult);
        }
    }
    
    //==========================================================================
    // 硬指标：Exposure（不得突破，严格风控）
    // 使用getTotalGrossExposure替代getTotalExposure
    // 跨品种多空不能简单对冲，毛暴露更准确反映实际风险
    //==========================================================================
    double exposure = portfolio->getTotalGrossExposure();
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
            if (v.utilization >= _rate_limits.position_breach_pause_threshold)
            {
                outCategory = RiskCategory::REVERSIBLE;
                return RiskAction::PAUSE_QUOTING;
            }
            if (v.current_value > 0)
                long_breach = true;
            else
                short_breach = true;
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
    
    if (breachCount >= _rate_limits.flatten_threshold)
    {
        outCategory = RiskCategory::REVERSIBLE;
        return RiskAction::FLATTEN_POSITION;
    }
    if (breachCount >= _rate_limits.pause_threshold)
    {
        outCategory = RiskCategory::REVERSIBLE;
        return RiskAction::PAUSE_QUOTING;
    }
    if (breachCount >= _rate_limits.widen_threshold)
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

bool FutuRiskMonitor::resumeTrading()
{
    // Only allow resume for reversible risks
    if (_halt_category == RiskCategory::IRREVERSIBLE)
    {
        WTSLogger::warn("[RISK] Cannot resume trading: halt is IRREVERSIBLE (requires manual intervention)");
        return false;
    }
    
    _trading_halted.store(false, std::memory_order_relaxed);
    _halt_timestamp = 0;
    broadcastAlert("TRADING_RESUMED", "Trading resumed after risk normalized");
    return true;
}

void FutuRiskMonitor::pauseQuoting()
{
    // 避免重复触发QUOTING_PAUSED（与resumeQuoting对称）
    bool expected = false;
    if (!_quoting_paused.compare_exchange_strong(expected, true,
            std::memory_order_relaxed, std::memory_order_relaxed)) {
        // 已经是paused状态，无需重复操作
        return;
    }
    _pause_timestamp = _current_time.load(std::memory_order_relaxed);
    broadcastAlert("QUOTING_PAUSED", "Quoting paused due to risk limits");
}

void FutuRiskMonitor::resumeQuoting()
{
    // 避免重复触发QUOTING_RESUMED
    // 之前没有检查当前状态，多个合约tick回调都会触发resumeQuoting
    // 导致同一秒内出现多次QUOTING_RESUMED日志
    bool expected = true;
    if (!_quoting_paused.compare_exchange_strong(expected, false, 
            std::memory_order_relaxed, std::memory_order_relaxed)) {
        // 已经是resumed状态，无需重复操作
        return;
    }
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
    
    // Check exposure utilization (use gross exposure)
    double exposure = portfolio->getTotalGrossExposure();
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
    // Preserve IRREVERSIBLE halt category across daily reset.
    // IRREVERSIBLE risks (e.g. daily loss) must not auto-recover on new day.
    // Only clearIrreversible() (called with human confirmation) can reset it.
    RiskCategory saved_halt_category = _halt_category;
    bool was_irreversible = (saved_halt_category == RiskCategory::IRREVERSIBLE);
    
    _trading_halted.store(was_irreversible, std::memory_order_relaxed);  // Stay halted if IRREVERSIBLE
    _long_blocked.store(false, std::memory_order_relaxed);
    _short_blocked.store(false, std::memory_order_relaxed);
    _quoting_paused.store(was_irreversible, std::memory_order_relaxed);  // Stay paused if IRREVERSIBLE
    // _halt_category is preserved (not reset to REVERSIBLE)
    _halt_timestamp = 0;
    _pause_timestamp = 0;
    _last_recovery_check = 0;
    
    _order_times.clear();
    _cancel_times.clear();
    _trade_times.clear();
    _orders_last_sec.store(0, std::memory_order_relaxed);
    _cancels_last_sec.store(0, std::memory_order_relaxed);
    _trades_last_sec.store(0, std::memory_order_relaxed);
    
    _delta_snapshots.fill(DeltaSnapshot());
    _delta_snapshot_count = 0;
    _delta_snapshot_head = 0;
    _delta_rate_breached.store(false, std::memory_order_relaxed);
    _delta_rate_breach_time = 0;
    
    if (was_irreversible)
    {
        WTSLogger::warn("[RISK] resetDaily: IRREVERSIBLE halt preserved, trading remains halted until clearIrreversible() is called");
    }
}

bool FutuRiskMonitor::clearIrreversible()
{
    if (_halt_category != RiskCategory::IRREVERSIBLE)
    {
        WTSLogger::debug("[RISK] clearIrreversible: not in IRREVERSIBLE state, nothing to clear");
        return false;
    }
    
    WTSLogger::info("[RISK] clearIrreversible: manually clearing IRREVERSIBLE halt (human confirmation)");
    _halt_category = RiskCategory::REVERSIBLE;
    _trading_halted.store(false, std::memory_order_relaxed);
    _quoting_paused.store(false, std::memory_order_relaxed);
    _halt_timestamp = 0;
    _pause_timestamp = 0;
    _was_loss_triggered = false;
    _halt_pnl_snapshot = 0;
    broadcastAlert("IRREVERSIBLE_CLEARED", "IRREVERSIBLE halt manually cleared, trading can resume");
    return true;
}

void FutuRiskMonitor::resetSession()
{
    resetDaily();
}

//==========================================================================
// Delta Rate Tracking Implementation
//==========================================================================

void FutuRiskMonitor::recordDeltaSnapshot(double currentDelta, uint64_t timestampMs)
{
    _delta_snapshots[_delta_snapshot_head] = DeltaSnapshot(currentDelta, timestampMs);
    _delta_snapshot_head = (_delta_snapshot_head + 1) % DELTA_SNAPSHOT_CAPACITY;
    if (_delta_snapshot_count < DELTA_SNAPSHOT_CAPACITY)
        _delta_snapshot_count++;
}

bool FutuRiskMonitor::checkDeltaRate() const
{
    return _delta_rate_breached.load(std::memory_order_relaxed);
}

double FutuRiskMonitor::getDeltaChangeRate() const
{
    // ========================================================================
    // 时间加权累积变化算法 (方案 A 修法, 2026-06-08)
    // 旧算法: |端点差|/(newestTime-oldestTime)
    //   ms 级 tick 间隔下分母 0.001-0.01s, 单笔 5 手成交算成 500-5000/s
    //   导致 _delta_rate_breached 标志被持续刷新, recovery 永久卡死
    // 新算法: window 内所有相邻 snapshot 的 |Δdelta| 累积 / 时间分母
    //   时间分母 = max(实际跨度, window配置的一半)
    //   反映"累积扰动强度", 单笔瞬时跳变被分摊, 不再卡死
    // ========================================================================
    if (_delta_snapshot_count < 2)
        return 0.0;
    
    uint64_t now = _current_time.load(std::memory_order_relaxed);
    uint32_t windowMs = _rate_limits.delta_rate_window_sec * 1000;
    if (windowMs == 0)
        return 0.0;
    uint64_t cutoff = (now > windowMs) ? (now - windowMs) : 0;
    
    // 收集 window 内 snapshot 按时间排序 (环形缓冲已按写入顺序排, 但物理 idx 不连续)
    struct TimedSnap { uint64_t t; double d; };
    TimedSnap window_snaps[DELTA_SNAPSHOT_CAPACITY];
    size_t n = 0;
    
    for (size_t i = 0; i < _delta_snapshot_count; ++i)
    {
        size_t idx;
        if (_delta_snapshot_count < DELTA_SNAPSHOT_CAPACITY)
            idx = i;
        else
            idx = (_delta_snapshot_head + i) % DELTA_SNAPSHOT_CAPACITY;
        
        const DeltaSnapshot& snap = _delta_snapshots[idx];
        if (snap.timestamp_ms < cutoff || snap.timestamp_ms == 0)
            continue;
        
        window_snaps[n++] = { snap.timestamp_ms, snap.delta };
    }
    
    if (n < 2)
        return 0.0;
    
    // 按时间升序排序 (n<=32, 插入排序足够)
    for (size_t i = 1; i < n; ++i)
    {
        TimedSnap key = window_snaps[i];
        size_t j = i;
        while (j > 0 && window_snaps[j-1].t > key.t)
        {
            window_snaps[j] = window_snaps[j-1];
            --j;
        }
        window_snaps[j] = key;
    }
    
    // 累积 |Δdelta|
    double cumulative_change = 0.0;
    for (size_t i = 1; i < n; ++i)
    {
        cumulative_change += std::abs(window_snaps[i].d - window_snaps[i-1].d);
    }
    
    // 时间分母: 取实际跨度 vs window 配置一半的较大值, 避免短期采样集中导致分母过小
    uint64_t actualSpanMs = window_snaps[n-1].t - window_snaps[0].t;
    uint64_t minDenomMs = windowMs / 2;
    uint64_t denomMs = (actualSpanMs > minDenomMs) ? actualSpanMs : minDenomMs;
    if (denomMs == 0)
        return 0.0;
    
    double denomSec = static_cast<double>(denomMs) / 1000.0;
    return cumulative_change / denomSec;
}

bool FutuRiskMonitor::checkAndHandleDeltaRateBreach()
{
    if (_rate_limits.max_delta_change_per_sec <= 0)
        return false;
    
    double rate = getDeltaChangeRate();
    bool breached = rate > _rate_limits.max_delta_change_per_sec;
    
    if (breached && !_delta_rate_breached.load(std::memory_order_relaxed))
    {
        _delta_rate_breached.store(true, std::memory_order_relaxed);
        _delta_rate_breach_time = _current_time.load(std::memory_order_relaxed);
        pauseQuoting();
        broadcastAlert("DELTA_RATE_BREACH",
            fmt::format("Delta change rate {:.2f}/s exceeds limit {:.2f}/s, quoting paused",
                rate, _rate_limits.max_delta_change_per_sec));
        WTSLogger::warn("[RISK] Delta rate breach: {:.2f}/s > {:.2f}/s, quoting paused",
            rate, _rate_limits.max_delta_change_per_sec);
        return true;
    }
    
    if (_delta_rate_breached.load(std::memory_order_relaxed))
    {
        uint64_t now = _current_time.load(std::memory_order_relaxed);
        uint64_t cooldownMs = _rate_limits.delta_rate_cooldown_ms;
        if (!breached && (now - _delta_rate_breach_time) >= cooldownMs)
        {
            _delta_rate_breached.store(false, std::memory_order_relaxed);
            if (_quoting_paused.load(std::memory_order_relaxed))
            {
                resumeQuoting();
                WTSLogger::info("[RISK] Delta rate recovered, quoting resumed");
            }
        }
    }
    
    return false;
}

//==========================================================================
// Closeout Management (收盘前平仓) - State Machine Implementation
//==========================================================================

bool FutuRiskMonitor::transitionCloseoutSub(CloseoutSub next_state, uint64_t timestamp)
{
    // same-state 静默短路 —— 调用方(StrategyCoordinator/UftFutuMmStrategy)在
    // closeout 窗口的 on_tick/on_calc 高频路径里反复调 markCloseoutFlattening,
    // 不应每次都报 warning("Invalid state transition: 2 -> 2")。同 state 视为
    // idempotent no-op 即可,真正的非法转移(如 IDLE → COMPLETED)仍走下面的告警。
    if (_closeout_state.state == next_state)
    {
        return true;  // idempotent
    }
    if (!_closeout_state.canTransitionTo(next_state))
    {
        WTSLogger::warn("[CLOSEOUT] Invalid state transition: {} -> {}",
            static_cast<int>(_closeout_state.state), static_cast<int>(next_state));
        return false;
    }
    
    _closeout_state.state = next_state;
    
    switch (next_state)
    {
        case CloseoutSub::TRIGGERED:
            _closeout_state.trigger_time = timestamp;
            break;
        case CloseoutSub::DRAINING:
            _closeout_state.flatten_start = timestamp;
            break;
        case CloseoutSub::COMPLETED:
            _closeout_state.complete_time = timestamp;
            break;
        case CloseoutSub::FAILED:
            _closeout_state.fail_time = timestamp;
            _closeout_state.retry_count++;
            break;
        case CloseoutSub::RETRYING:
            break;
        default:
            break;
    }
    
    return true;
}

void FutuRiskMonitor::markCloseoutTriggered(uint64_t timestamp)
{
    if (transitionCloseoutSub(CloseoutSub::TRIGGERED, timestamp))
    {
        pauseQuoting();
        broadcastAlert("CLOSEOUT_TRIGGERED", 
            fmt::format("Closeout state: TRIGGERED at {}", timestamp));
    }
}

void FutuRiskMonitor::markCloseoutDraining(uint64_t timestamp)
{
    if (transitionCloseoutSub(CloseoutSub::DRAINING, timestamp))
    {
        broadcastAlert("CLOSEOUT_DRAINING",
            fmt::format("Closeout state: DRAINING at {}", timestamp));
    }
}

void FutuRiskMonitor::markCloseoutCompleted(uint64_t timestamp)
{
    if (transitionCloseoutSub(CloseoutSub::COMPLETED, timestamp))
    {
        // 标记夜盘 closeout 已完成，防止 reset 后重触发
        if (_closeout_state.is_night_closeout)
            _closeout_state.night_closeout_done = true;
        broadcastAlert("CLOSEOUT_COMPLETED", 
            fmt::format("Closeout state: COMPLETED at {}", timestamp));
    }
}

void FutuRiskMonitor::markCloseoutFailed(uint64_t timestamp)
{
    if (transitionCloseoutSub(CloseoutSub::FAILED, timestamp))
    {
        if (_closeout_state.retry_count >= _closeout_state.max_retries)
        {
            broadcastAlert("CLOSEOUT_FAILED", 
                fmt::format("Closeout FAILED at {} (retries exhausted: {}/{}), manual intervention required",
                    timestamp, _closeout_state.retry_count, _closeout_state.max_retries));
        }
        else
        {
            broadcastAlert("CLOSEOUT_FAILED", 
                fmt::format("Closeout FAILED at {} (retry {}/{}), will retry in {}ms",
                    timestamp, _closeout_state.retry_count, _closeout_state.max_retries,
                    _closeout_state.retry_interval_ms));
        }
    }
}

bool FutuRiskMonitor::checkCloseoutRetry(uint64_t current_time_ms)
{
    if (_closeout_state.state != CloseoutSub::FAILED)
        return false;
    
    if (_closeout_state.retry_count >= _closeout_state.max_retries)
    {
        WTSLogger::error("[CLOSEOUT] Max retries ({}) exhausted, manual intervention required",
            _closeout_state.max_retries);
        return false;
    }
    
    if (current_time_ms - _closeout_state.fail_time < _closeout_state.retry_interval_ms)
        return false;
    
    if (transitionCloseoutSub(CloseoutSub::RETRYING, current_time_ms))
    {
        broadcastAlert("CLOSEOUT_RETRYING",
            fmt::format("Closeout retry {}/{} at {}", 
                _closeout_state.retry_count, _closeout_state.max_retries, current_time_ms));
        return true;
    }
    
    return false;
}

bool FutuRiskMonitor::checkCloseout(uint32_t currentTime, uint32_t closeTime)
{
    if (_closeout_state.state == CloseoutSub::COMPLETED ||
        _closeout_state.state == CloseoutSub::FAILED)
        return false;
    
    // ctx->stra_get_time() 返回 HHMM (4位), 不是 HHMMSS。
    // 兼容两种格式：>= 10000 视为 HHMMSS, 否则 HHMM。
    uint32_t currentHour, currentMin;
    if (currentTime >= 10000) {
        currentHour = currentTime / 10000;
        currentMin = (currentTime / 100) % 100;
    } else {
        currentHour = currentTime / 100;
        currentMin = currentTime % 100;
    }
    
    if (currentHour > 23 || currentMin > 59)
    {
        WTSLogger::warn("[RISK] Invalid current time format: {}", currentTime);
        return false;
    }
    
    uint32_t currentTotalMin = currentHour * 60 + currentMin;
    
    //==========================================================================
    // 双触发点平仓逻辑
    //
    // 有夜盘的品种有两个平仓触发点:
    //   1. 夜盘收盘前 night_minutes_before 分钟 (如 02:25)
    //   2. 全天收盘前 minutes_before 分钟 (如 15:10)
    //
    // 夜盘收盘时间格式 (HHMM):
    //   跨日品种: 230 (02:30), 100 (01:00) — 收盘在凌晨
    //   不跨日品种: 2300 (23:00), 2330 (23:30) — 收盘在当晚
    //   无夜盘: 0
    //
    // 全天收盘时间格式 (HHMMSS):
    //   150000 (15:00), 151500 (15:15)
    //==========================================================================
    
    // --- 触发点1: 夜盘收盘 ---
    // 只在夜盘时段 (21:00-05:59) 检查，避免白盘时段误触发
    // 跳过已完成的夜盘 closeout（防止 reset 后重触发）
    if (_closeout_config.night_close_time > 0 && _closeout_config.night_minutes_before > 0
        && (currentHour >= 21 || currentHour < 6)
        && !_closeout_state.night_closeout_done)
    {
        uint32_t nightClose = _closeout_config.night_close_time;
        uint32_t nightCloseHour = nightClose / 100;
        uint32_t nightCloseMin = nightClose % 100;
        
        // 夜盘收盘时间合法性校验，防止八进制误配
        // C++中以0开头的整数字面量被解析为八进制，如0230→八进制=十进制152(01:52)而非23:00。
        // 配置文件也可能误配为非法值。校验hour<=23, minute<=59。
        if (nightCloseHour > 23 || nightCloseMin > 59)
        {
            WTSLogger::warn("[RISK] Invalid night_close_time format: {} (hour={}, min={}), "
                           "possible octal misconfiguration. Expected HHMM format.",
                           nightClose, nightCloseHour, nightCloseMin);
            // Skip night session closeout check for invalid time
        }
        else
        {
        uint32_t nightCloseTotalMin = nightCloseHour * 60 + nightCloseMin;
        
        bool is_overnight = (nightCloseHour < 6);  // 收盘在凌晨 → 跨日品种
        
        if (is_overnight)
        {
            // 跨日品种: 统一时间轴映射
            // 21:00-23:59 → 保持原值 (1260-1439)
            // 00:00-05:59 → +1440    (0-359 → 1440-1799)
            // close=02:30 → 150+1440 = 1590
            int32_t closeAbs = static_cast<int32_t>(nightCloseTotalMin) + 1440;
            int32_t triggerAbs = closeAbs - static_cast<int32_t>(_closeout_config.night_minutes_before);
            
            int32_t currentAbs;
            if (currentTotalMin >= 1260) {
                currentAbs = static_cast<int32_t>(currentTotalMin);  // 21:00-23:59
            } else {
                currentAbs = static_cast<int32_t>(currentTotalMin) + 1440;  // 00:00-05:59
            }
            
            if (currentAbs >= triggerAbs && _closeout_state.state == CloseoutSub::IDLE)
            {
            // record this is a night closeout so COMPLETED handler
            // only resets state for night→day transition, not day closeout
            _closeout_state.is_night_closeout = true;
                markCloseoutTriggered(currentTime * 100);
                broadcastAlert("CLOSEOUT_TRIGGERED", 
                    fmt::format("Night closeout triggered at {}:{:02d}, night close {}:{:02d}+1d, {} minutes before",
                        currentHour, currentMin, nightCloseHour, nightCloseMin, _closeout_config.night_minutes_before));
                return true;
            }
        }
        else
        {
            // 不跨日品种: 夜盘收盘在当晚 (23:00, 23:30)
            int32_t triggerTotalMin = static_cast<int32_t>(nightCloseTotalMin) 
                                    - static_cast<int32_t>(_closeout_config.night_minutes_before);
            if (triggerTotalMin < 0) triggerTotalMin = 0;
            
            if (static_cast<int32_t>(currentTotalMin) >= triggerTotalMin 
                && _closeout_state.state == CloseoutSub::IDLE)
            {
            // record this is a night closeout
            _closeout_state.is_night_closeout = true;
                markCloseoutTriggered(currentTime * 100);
                broadcastAlert("CLOSEOUT_TRIGGERED", 
                    fmt::format("Night closeout triggered at {}:{:02d}, night close {}:{:02d}, {} minutes before",
                        currentHour, currentMin, nightCloseHour, nightCloseMin, _closeout_config.night_minutes_before));
                return true;
            }
        }
        } // end else (valid night_close_time)
    }
    
    // --- 触发点2: 全天收盘 (白盘) ---
    // 只在白盘时段 (06:00-15:59) 检查，避免夜盘 21:00+ 误触发
    // (原 currentHour<=20 把 20:59 也吃进来导致夜盘开盘前就平仓)
    if (_closeout_config.minutes_before > 0
        && currentHour >= 6 && currentHour <= 15)
    {
        uint32_t closeHour, closeMin;
        if (closeTime < 10000)
        {
            closeHour = closeTime / 100;
            closeMin = closeTime % 100;
        }
        else
        {
            closeHour = closeTime / 10000;
            closeMin = (closeTime / 100) % 100;
        }
        
        if (closeHour > 23 || closeMin > 59)
        {
            WTSLogger::warn("[RISK] Invalid close time format: {}, using default 15:15", closeTime);
            closeHour = 15;
            closeMin = 15;
        }
        
        uint32_t closeTotalMin = closeHour * 60 + closeMin;
        int32_t triggerTotalMin = static_cast<int32_t>(closeTotalMin) 
                                - static_cast<int32_t>(_closeout_config.minutes_before);
        if (triggerTotalMin < 0) triggerTotalMin = 0;
        
        if (static_cast<int32_t>(currentTotalMin) >= triggerTotalMin 
            && _closeout_state.state == CloseoutSub::IDLE)
        {
            // day closeout, not night; reset night flag for next session
            _closeout_state.is_night_closeout = false;
            _closeout_state.night_closeout_done = false;
            markCloseoutTriggered(currentTime * 100);
            broadcastAlert("CLOSEOUT_TRIGGERED", 
                fmt::format("Day closeout triggered at {}:{:02d}, close time {}:{:02d}, {} minutes before",
                    currentHour, currentMin, closeHour, closeMin, _closeout_config.minutes_before));
            return true;
        }
    }
    
    return _closeout_state.state != CloseoutSub::IDLE;
}

// resetCloseout改为通过状态机转换而非直接构造
// 直接构造新对象绕过canTransitionTo检查，可能导致非法状态转换
// force=true 用于 session_begin —— 新交易日是硬边界,
// 上一日 FLATTENING/TRIGGERED 等残留状态必须清掉(否则 resetCloseout 被状态机
// 拒绝,state 永久卡死,Delta 雪崩)。force=false(默认)保留状态机保护。
void FutuRiskMonitor::resetCloseout(bool force)
{
    if (force || _closeout_state.canTransitionTo(CloseoutSub::IDLE))
    {
        _closeout_state.state = CloseoutSub::IDLE;
        _closeout_state.trigger_time = 0;
        _closeout_state.flatten_start = 0;
        _closeout_state.complete_time = 0;
        _closeout_state.fail_time = 0;
        _closeout_state.retry_count = 0;
        _closeout_state.is_night_closeout = false;  // reset night flag
        // NOTE: night_closeout_done 不在此重置，只在新白盘 closeout 触发时重置
        // 这样 reset 后夜盘时段不会重触发夜盘 closeout
    }
    else
    {
        WTSLogger::warn("FutuRiskMonitor: resetCloseout blocked by state machine, "
                         "current state={} — cannot transition to IDLE",
            static_cast<int>(_closeout_state.state));
    }
}

FutuRiskMonitor::PreTradeResult FutuRiskMonitor::checkPreTradePosition(
    const std::string& code,
    const FutuPortfolio* portfolio,
    const UnifiedOrderTracker* tracker) const
{
    // v3 软风控：不再 BLOCK，返回 utilization 让 Quoter 做 qty 衰减
    PreTradeResult result{true, true, 0.0, 0.0, false, false};
    
    if (!portfolio) return result;
    
    const ContractState* cs = portfolio->getContract(code);
    if (!cs || cs->max_position <= 0) return result;
    
    double pending_buy = tracker ? tracker->getPendingBuyQty(code) : 0;
    double pending_sell = tracker ? tracker->getPendingSellQty(code) : 0;
    double projected_long = (cs->position > 0 ? cs->position : 0) + pending_buy;
    double projected_short = (cs->position < 0 ? std::abs(cs->position) : 0) + pending_sell;
    
    result.long_utilization  = projected_long  / cs->max_position;
    result.short_utilization = projected_short / cs->max_position;
    
    // v3: util >= 1.0 时只设 obligation 标志，不阻断；Quoter 负责
    //     (A) 加仓侧 qty 指数衰减 (util接近1时qty→0)
    //     (B) 减仓侧强制义务报价 (≥10手/≤10ticks)
    if (result.long_utilization >= 1.0) {
        result.force_ask_obligation = true;
        WTSLogger::warn("[RISK_V3] {} LONG cap reached: pos={:.0f} pending_buy={:.0f} proj_long={:.0f}/{:.0f} (util={:.2f}) → ASK obligation",
            code, cs->position, pending_buy, projected_long, cs->max_position, result.long_utilization);
    }
    if (result.short_utilization >= 1.0) {
        result.force_bid_obligation = true;
        WTSLogger::warn("[RISK_V3] {} SHORT cap reached: pos={:.0f} pending_sell={:.0f} proj_short={:.0f}/{:.0f} (util={:.2f}) → BID obligation",
            code, cs->position, pending_sell, projected_short, cs->max_position, result.short_utilization);
    }
    
    return result;
}

} // namespace futu
