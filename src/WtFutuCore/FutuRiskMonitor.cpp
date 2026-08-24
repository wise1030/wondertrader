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
#include "SessionPhaseManager.h"
#include "../WTSTools/WTSLogger.h"
#include "../WtUftCore/EventNotifier.h"
#include <algorithm>

namespace futu
{

FutuRiskMonitor::FutuRiskMonitor() : _event_notifier(nullptr), _delta_snapshot_count(0), _delta_snapshot_head(0) {}

void FutuRiskMonitor::recordOrder()
{
    uint64_t now = _current_time.load(std::memory_order_relaxed);

    // P2-1: 去掉 atomic 双轨计数,直接用 ring buffer size() 读取
    // 旧代码 atomic +1 无条件、-1 有条件(try_pop 成功),ring buffer 满(256)时
    // try_push 失败仍 +1,导致计数单调虚高。改用 size() 精确反映 1 秒窗口内事件数。
    // V8-P0-2: SPSC ring 存在多生产者 (MdSpi/arb/TdSpi), 自旋锁串行化
    SpinLockGuard _g(_rate_lock);

    uint64_t cutoff = (now > 1000) ? (now - 1000) : 0;
    while (auto item = _order_times.try_peek()) {
        if (*item < cutoff)
            _order_times.try_pop();
        else
            break;
    }
    _order_times.try_push(now);
}

void FutuRiskMonitor::recordOrders(uint32_t n)
{
    if (n == 0)
        return;
    uint64_t now = _current_time.load(std::memory_order_relaxed);

    // V8-P0-2: MM 批量计数 (refreshQuotes 返回值), 单次临界区
    SpinLockGuard _g(_rate_lock);

    uint64_t cutoff = (now > 1000) ? (now - 1000) : 0;
    while (auto item = _order_times.try_peek()) {
        if (*item < cutoff)
            _order_times.try_pop();
        else
            break;
    }
    for (uint32_t i = 0; i < n; i++)
        _order_times.try_push(now);
}

void FutuRiskMonitor::recordCancel()
{
    uint64_t now = _current_time.load(std::memory_order_relaxed);

    // V8-P0-2: 同上, 多生产者串行化
    SpinLockGuard _g(_rate_lock);

    uint64_t cutoff = (now > 1000) ? (now - 1000) : 0;
    while (auto item = _cancel_times.try_peek()) {
        if (*item < cutoff)
            _cancel_times.try_pop();
        else
            break;
    }
    _cancel_times.try_push(now);
}

void FutuRiskMonitor::recordTrade()
{
    uint64_t now = _current_time.load(std::memory_order_relaxed);

    // V8-P0-2: 同上, 多生产者串行化
    SpinLockGuard _g(_rate_lock);

    uint64_t cutoff = (now > 1000) ? (now - 1000) : 0;
    while (auto item = _trade_times.try_peek()) {
        if (*item < cutoff)
            _trade_times.try_pop();
        else
            break;
    }
    _trade_times.try_push(now);
}

void FutuRiskMonitor::pruneRateWindows(uint64_t now)
{
    // V8-P0-2: 与 record* 同锁串行化 (此前 MdSpi prune 与 arb 线程 push 并发)
    SpinLockGuard _g(_rate_lock);

    uint64_t cutoff = (now > 1000) ? (now - 1000) : 0;
    while (auto item = _order_times.try_peek()) {
        if (*item < cutoff)
            _order_times.try_pop();
        else
            break;
    }
    while (auto item = _cancel_times.try_peek()) {
        if (*item < cutoff)
            _cancel_times.try_pop();
        else
            break;
    }
    while (auto item = _trade_times.try_peek()) {
        if (*item < cutoff)
            _trade_times.try_pop();
        else
            break;
    }
}

void FutuRiskMonitor::broadcastCostBasisStale(const std::string& code)
{
    uint64_t now = _current_time.load(std::memory_order_relaxed);
    uint64_t last = _last_cost_stale_alert_ms.load(std::memory_order_relaxed);
    if (now < last || now - last < 5000)
        return;
    _last_cost_stale_alert_ms.store(now, std::memory_order_relaxed);
    broadcastAlert("COST_BASIS_STALE",
                   fmt::format("Contract {} cost basis is stale; daily-loss irreversible halt is downgraded", code));
}

void FutuRiskMonitor::broadcastAlert(const std::string& alertType, const std::string& message)
{
    // Log the alert
    WTSLogger::warn("[RISK] {}: {}", alertType, message);

    // Broadcast via EventNotifier if available
    if (_event_notifier) {
        _event_notifier->notify(alertType.c_str(), message.c_str());
    }
}

std::vector<RiskViolation> FutuRiskMonitor::checkRiskLimits(const FutuPortfolio* portfolio)
{
    std::vector<RiskViolation> violations;
    checkRiskLimits(portfolio, violations);
    return violations;
}

void FutuRiskMonitor::checkRiskLimits(const FutuPortfolio* portfolio, std::vector<RiskViolation>& violations)
{
    violations.clear();
    violations.reserve(6); // P-5: 预分配，避免6次push_back的重分配
    if (!portfolio)
        return;

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

    if (params.portfolio_max_delta > 0) {
        double delta_utilization = absDelta / params.portfolio_max_delta;

        // 热路径告警限频：持续超限时每 tick 刷屏（线上曾单日 100MB+），
        // 同一类 delta 告警按时间节流，最多每 1s 输出一次。
        uint64_t last_delta_warn = _last_delta_warn_ms.load(std::memory_order_relaxed);
        bool log_delta = (ts < last_delta_warn || ts - last_delta_warn >= WARN_THROTTLE_MS);
        if (log_delta)
            _last_delta_warn_ms.store(ts, std::memory_order_relaxed);

        // 高利用率警告（>= 80%）
        if (delta_utilization >= _rate_limits.delta_warning_mult && log_delta) {
            WTSLogger::warn("[STRATEGY] Delta utilization high: {:.1f}% ({:.2f} / {:.2f}) - skew will adjust quotes",
                            delta_utilization * 100,
                            absDelta,
                            params.portfolio_max_delta);
        }

        // 超限警告（>= 100%），但不作为违规
        if (delta_utilization >= 1.0 && log_delta) {
            WTSLogger::warn("[STRATEGY] Delta limit exceeded: {:.2f} > {:.2f} (soft limit, skew handling)",
                            absDelta,
                            params.portfolio_max_delta);
        }

        // 软指标：超过 delta_critical_mult 倍时输出严重警告
        // max_delta 是软风控，仅用于调节 skew 和对冲决策，不触发硬风控动作
        // 真正的硬限制由 Exposure 和 Daily Loss 承担
        if (absDelta > params.portfolio_max_delta * _rate_limits.delta_critical_mult && log_delta) {
            WTSLogger::warn(
                "[STRATEGY] Delta critically high: {:.2f} > {:.2f} (portfolio_max_delta * {:.1f}, skew/hedge handling)",
                absDelta,
                params.portfolio_max_delta * _rate_limits.delta_critical_mult,
                _rate_limits.delta_critical_mult);
        }
    }

    //==========================================================================
    // 硬指标：Exposure（不得突破，严格风控）
    // 使用getTotalGrossExposure替代getTotalExposure
    // 跨品种多空不能简单对冲，毛暴露更准确反映实际风险
    //==========================================================================
    double exposure = portfolio->getTotalGrossExposure();
    // max_exposure > 0 防护: 配 0 表示禁用, 否则 exposure>0 恒真 → 每 tick 误报
    if (params.max_exposure > 0 && exposure > params.max_exposure) {
        RiskViolation v;
        v.type = RiskLimitType::EXPOSURE;
        // Store signed delta for direction detection (exposure itself is always positive)
        v.current_value = delta; // Use delta's sign to determine breach direction
        v.limit_value = params.max_exposure;
        v.utilization = exposure / params.max_exposure;
        v.severity = v.utilization > 1.0 ? RiskSeverity::BREACH : RiskSeverity::WARNING;
        v.timestamp = ts;
        v.message = fmt::format(
            "Exposure limit exceeded: {:.2f} > {:.2f} (delta={:.2f})", exposure, params.max_exposure, delta);
        violations.push_back(v);

        if (v.severity == RiskSeverity::BREACH)
            broadcastAlert("EXPOSURE_BREACH", v.message);
    }

    //==========================================================================
    // 硬指标：Daily Loss（不可逆，需人工干预）
    //==========================================================================
    double pnl = portfolio->getTotalPnL();
    // max_loss > 0 防护: 配 0 表示禁用, 否则任何浮亏都触发 IRREVERSIBLE halt
    if (params.max_loss > 0 && pnl < -params.max_loss) {
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
    // P2-1: 直接用 ring buffer size() 替代 atomic 双轨计数
    // 读侧先剔除过期样本: 滑窗只在事件到达时推进会导致停止报单后误报持续
    pruneRateWindows(ts);
    uint32_t orders = static_cast<uint32_t>(_order_times.size());
    uint32_t cancels = static_cast<uint32_t>(_cancel_times.size());
    uint32_t trades = static_cast<uint32_t>(_trade_times.size());

    if (orders > _rate_limits.max_orders_per_sec) {
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

    if (cancels > _rate_limits.max_cancels_per_sec) {
        RiskViolation v;
        v.type = RiskLimitType::CANCEL_RATE;
        v.current_value = cancels;
        v.limit_value = _rate_limits.max_cancels_per_sec;
        v.utilization = (double)cancels / _rate_limits.max_cancels_per_sec;
        v.severity = RiskSeverity::WARNING;
        v.timestamp = ts;
        v.message =
            fmt::format("Cancel rate limit exceeded: {} > {} per sec", cancels, _rate_limits.max_cancels_per_sec);
        violations.push_back(v);
    }

    if (trades > _rate_limits.max_trades_per_sec) {
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
    ContractState pos_breached_buf;
    const ContractState* pos_breached =
        portfolio->getPositionBreachedSnapshot(pos_breached_buf) ? &pos_breached_buf : nullptr;
    if (pos_breached) {
        RiskViolation v;
        v.type = RiskLimitType::POSITION_NET;
        v.code = pos_breached->code;
        v.current_value = pos_breached->position;
        v.limit_value = pos_breached->max_position;
        v.utilization =
            pos_breached->max_position > 0 ? std::abs(pos_breached->position) / pos_breached->max_position : 1.0;
        v.severity = RiskSeverity::BREACH;
        v.timestamp = ts;
        v.message = fmt::format("Contract {} POSITION limit breached: {} (max {})",
                                pos_breached->code,
                                pos_breached->position,
                                pos_breached->max_position);
        violations.push_back(v);

        // 热路径告警限频：POSITION_BREACH 在持仓持续超限时每 tick 广播/写盘，
        // 按时间节流至最多每 1s 一次。
        uint64_t last_pos_warn = _last_pos_breach_warn_ms.load(std::memory_order_relaxed);
        if (ts < last_pos_warn || ts - last_pos_warn >= WARN_THROTTLE_MS) {
            _last_pos_breach_warn_ms.store(ts, std::memory_order_relaxed);
            broadcastAlert("POSITION_BREACH", v.message);
        }
    }

    // 注意：单合约 Delta 是软指标，不产生 violation
    // Delta 超限时通过 skew 偏移和日志警告处理，不进行风控 block
}

bool FutuRiskMonitor::checkRateLimits()
{
    pruneRateWindows(_current_time.load(std::memory_order_relaxed));
    return _order_times.size() < _rate_limits.max_orders_per_sec &&
           _cancel_times.size() < _rate_limits.max_cancels_per_sec &&
           _trade_times.size() < _rate_limits.max_trades_per_sec;
}

// R2.2 checkSoftLimits 已删 (2026-08-24② A1): 零调用死方法。
//   WIDEN_SPREAD 软响应唯一活路径 = RiskCoordinator::checkRisk 的
//   quote_chain->riskWiden().tickSoft(cur_util, l1, l2, halted) (每 tick 无状态重算,
//   阈值同源 RateLimits.position_warning_l1/l2)。

RiskAction FutuRiskMonitor::determineAction(const std::vector<RiskViolation>& violations) const
{
    RiskCategory category;
    return determineActionWithCategory(violations, category);
}

RiskAction FutuRiskMonitor::determineActionWithCategory(const std::vector<RiskViolation>& violations,
                                                        RiskCategory& outCategory,
                                                        bool cost_basis_stale) const
{
    outCategory = RiskCategory::REVERSIBLE; // Default: reversible

    if (violations.empty())
        return RiskAction::NONE;

    // 1. Check for irreversible risks (daily loss) - requires manual intervention
    for (const auto& v : violations) {
        if (v.type == RiskLimitType::DAILY_LOSS && v.severity == RiskSeverity::CRITICAL) {
            if (cost_basis_stale) {
                // 成本基不可信时，日亏不得驱动不可逆决策。
                outCategory = RiskCategory::REVERSIBLE;
                WTSLogger::warn("[RISK] DAILY_LOSS critical but cost basis stale -> downgraded to REVERSIBLE HALT");
                return RiskAction::HALT_TRADING;
            }
            outCategory = RiskCategory::IRREVERSIBLE;
            return RiskAction::HALT_TRADING;
        }
    }

    // 2. Check for critical reversible risks (max delta, max exposure)
    for (const auto& v : violations) {
        if (v.severity == RiskSeverity::CRITICAL) {
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

    for (const auto& v : violations) {
        if (v.severity < RiskSeverity::BREACH)
            continue;

        if (v.type == RiskLimitType::DELTA || v.type == RiskLimitType::EXPOSURE) {
            // Positive delta/exposure breach means too much long
            // Negative delta breach means too much short
            if (v.current_value > 0)
                long_breach = true;
            else
                short_breach = true;
        } else if (v.type == RiskLimitType::POSITION_NET) {
            // v7.1 连续控制重设计: 单合约仓位 breach 不再触发 BLOCK_SIDE/PAUSE 硬动作。
            // 仓位由连续控制层处理 (skew穿越权限 + obligation减仓 + taker紧急减仓),
            // 报价永在线(做市义务), 无状态=无恢复=无死锁。
            // alert 已在 checkRiskLimits 中上报 (POSITION_BREACH)。
            continue;
        }
    }

    // v7.3: PAUSE_QUOTING 与 FLATTEN_POSITION 分支已删除 (控制链断环清理)。
    //   原 PAUSE 要求 long_breach && short_breach, 但每笔 violation 的 delta 只有
    //   一个符号且 EXPOSURE 每 tick 最多一条 -> 数学上不可达;
    //   原 FLATTEN 要求 breachCount>=2, 但唯一能产 BREACH 的 EXPOSURE 每 tick
    //   最多一条 -> 恒 <=1 不可达。控制链实为"两头化":
    //   软连续控制(skew/force/takerReduce, 仓位永在线) + 硬 HALT(日亏/CRITICAL)。
    //   EXPOSURE breach 走下方单向 BLOCK_SIDE。

    // R2.3 遗留: breachCount 仅供下方 WIDEN 升级路径使用。
    int breachCount = 0;
    for (const auto& v : violations) {
        // POSITION_NET 不计入: 仓位 breach 由连续控制处理, 不参与升级
        if (v.severity == RiskSeverity::BREACH && v.type != RiskLimitType::POSITION_NET)
            breachCount++;
    }

    // Block specific direction
    if (long_breach) {
        outCategory = RiskCategory::REVERSIBLE;
        return RiskAction::BLOCK_SIDE_LONG;
    }
    if (short_breach) {
        outCategory = RiskCategory::REVERSIBLE;
        return RiskAction::BLOCK_SIDE_SHORT;
    }

    // 4. Count remaining breaches for tiered response (WARNING-only 升级路径)
    // 这一段处理 WARNING severity (BREACH 已在上面返回)
    if (breachCount >= _rate_limits.widen_threshold) {
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
    // V8-R6/WS-A: halt 域多字段复合写入收编 (写者含 arb 线程 handleRiskAlert)
    RecursiveSpinGuard _g(_halt_domain_lock);
    _halt_category.store(category, std::memory_order_release);
    _halt_timestamp = _current_time.load(std::memory_order_relaxed);
    _halt_pnl_snapshot = pnl_snapshot;
    _was_loss_triggered = (category == RiskCategory::IRREVERSIBLE) && (pnl_snapshot < 0);
    // 不在此处重置 _recovery_count — 旧代码每次 halt 清零,
    // 使 max_recovery_count(每 session 上限)形同虚设. 计数由 resetDaily 重置.

    const char* category_str = (category == RiskCategory::IRREVERSIBLE) ? "IRREVERSIBLE" : "REVERSIBLE";
    broadcastAlert("TRADING_HALTED", fmt::format("Trading halted ({}) due to risk limits", category_str));
}

bool FutuRiskMonitor::resumeTrading()
{
    RecursiveSpinGuard _g(_halt_domain_lock); // V8-R6/WS-A
    // Only allow resume for reversible risks
    if (_halt_category.load(std::memory_order_acquire) == RiskCategory::IRREVERSIBLE) {
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
    if (!_quoting_paused.compare_exchange_strong(
            expected, true, std::memory_order_relaxed, std::memory_order_relaxed)) {
        // 已经是paused状态，无需重复操作
        return;
    }
    RecursiveSpinGuard _g(_halt_domain_lock); // V8-R6/WS-A
    _pause_timestamp = _current_time.load(std::memory_order_relaxed);
    broadcastAlert("QUOTING_PAUSED", "Quoting paused due to risk limits");
}

void FutuRiskMonitor::resumeQuoting()
{
    // 避免重复触发QUOTING_RESUMED
    // 之前没有检查当前状态，多个合约tick回调都会触发resumeQuoting
    // 导致同一秒内出现多次QUOTING_RESUMED日志
    bool expected = true;
    if (!_quoting_paused.compare_exchange_strong(
            expected, false, std::memory_order_relaxed, std::memory_order_relaxed)) {
        // 已经是resumed状态，无需重复操作
        return;
    }
    RecursiveSpinGuard _g(_halt_domain_lock); // V8-R6/WS-A
    _pause_timestamp = 0;
    broadcastAlert("QUOTING_RESUMED", "Quoting resumed after risk normalized");
}

bool FutuRiskMonitor::canRecover(const FutuPortfolio* portfolio) const
{
    if (!portfolio)
        return false;

    RecursiveSpinGuard _g(_halt_domain_lock); // V8-R6/WS-A (const: lock 为 mutable)

    // Irreversible risks cannot auto-recover
    if (_halt_category.load(std::memory_order_acquire) == RiskCategory::IRREVERSIBLE)
        return false;

    // Check max recovery count
    if (_recovery_count >= _recovery_config.max_recovery_count) {
        WTSLogger::warn("[RISK] Max recovery count ({}) reached, manual intervention required",
                        _recovery_config.max_recovery_count);
        return false;
    }

    uint64_t now = _current_time.load(std::memory_order_relaxed);

    // Check cooldown period
    if (_trading_halted.load(std::memory_order_relaxed)) {
        if (now - _halt_timestamp < _recovery_config.cooldown_ms)
            return false;
    }

    if (_quoting_paused.load(std::memory_order_relaxed)) {
        if (now - _pause_timestamp < _recovery_config.cooldown_ms)
            return false;
    }

    // Check if risk utilization is below recovery threshold
    const PortfolioParams& params = portfolio->getParams();

    // 不检查 delta_utilization: delta 是软指标, 职责是调节 skew
    // (见 checkRiskLimits 注释 "不触发硬风控动作"), 不应作为恢复硬闸门 —
    // 否则 "HALT→无法交易→delta 降不下来→永不恢复" 死锁
    // (2026-08-17 ao 实盘: delta_util=1.7 挡住全天 2232 次恢复检查)。

    // Check exposure utilization (use gross exposure)
    double exposure = portfolio->getTotalGrossExposure();
    if (params.max_exposure > 0) {
        double exposure_util = exposure / params.max_exposure;
        if (exposure_util > _recovery_config.recovery_threshold)
            return false;
    }

    // Check if any contract position is still above pause threshold.
    // Recovery requires pos < max_position * pause_threshold (e.g. 1.2*50=60),
    // NOT pos <= max_position (50). The stricter requirement creates a dead-lock:
    // PAUSE blocks quoting -> no fills -> pos can't decrease -> never recovers.
    // With the relaxed threshold, MM resumes at 60 but checkPreTradePosition's
    // v3 soft limit (obligation mode at util>=1.0) drives natural reduction.
    for (const auto& c : portfolio->getAllContractsSnapshot()) {
        if (c.max_position > 0 && std::abs(c.position) > c.max_position * _rate_limits.position_breach_pause_threshold)
            return false;
    }

    // Enhanced: Check PnL recovery if halt was triggered by loss
    if (_was_loss_triggered) {
        double current_pnl = portfolio->getTotalPnL();
        double loss_at_halt = -_halt_pnl_snapshot; // Negative value
        double current_loss = -current_pnl;

        // Must recover at least pnl_recovery_ratio of the loss
        if (current_loss > loss_at_halt * (1.0 - _recovery_config.pnl_recovery_ratio)) {
            WTSLogger::debug("[RISK] PnL not recovered enough: loss_at_halt={:.2f}, current_loss={:.2f}",
                             loss_at_halt,
                             current_loss);
            return false;
        }
    }

    // P1-3.3: Check max loss threshold for recovery
    // If the loss at halt exceeds max_loss_for_recovery, block auto-recovery
    if (_recovery_config.max_loss_for_recovery != 0) {
        double halted_loss = std::abs(_halt_pnl_snapshot);
        if (halted_loss > std::abs(_recovery_config.max_loss_for_recovery)) {
            WTSLogger::warn(
                "[RISK] Loss at halt ({:.2f}) exceeds max for recovery ({:.2f}), manual intervention required",
                halted_loss,
                std::abs(_recovery_config.max_loss_for_recovery));
            return false;
        }
    }

    return true;
}

bool FutuRiskMonitor::checkAndRecover(const FutuPortfolio* portfolio)
{
    RecursiveSpinGuard _g(_halt_domain_lock); // V8-R6/WS-A (递归锁: 内部 resumeTrading/canRecover 重入)

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
        _halt_category.load(std::memory_order_acquire) == RiskCategory::REVERSIBLE) {
        resumeTrading();
        _recovery_count++;
        WTSLogger::info(
            "[RISK] Auto-recovery #{}/{}: Trading resumed", _recovery_count, _recovery_config.max_recovery_count);
        recovered = true;
    }

    // Resume quoting if paused
    if (_quoting_paused.load(std::memory_order_relaxed)) {
        resumeQuoting();
        recovered = true;
    }

    // Unblock directions if position normalized
    if (_long_blocked.load(std::memory_order_relaxed) || _short_blocked.load(std::memory_order_relaxed)) {
        // Check if positions have normalized
        ContractState breached_buf;
        const ContractState* breached =
            (portfolio && portfolio->getPositionBreachedSnapshot(breached_buf)) ? &breached_buf : nullptr;
        if (!breached) {
            unblockLong();
            unblockShort();
            recovered = true;
        }
    }

    return recovered;
}

void FutuRiskMonitor::resetDaily()
{
    RecursiveSpinGuard _g(_halt_domain_lock); // V8-R6/WS-A
    // Preserve IRREVERSIBLE halt category across daily reset.
    // IRREVERSIBLE risks (e.g. daily loss) must not auto-recover on new day.
    // Only clearIrreversible() (called with human confirmation) can reset it.
    // v7.1: auto_clear_irreversible_on_reset=true 时日界自动清除
    //       (回测场景, 模拟隔夜人工复核; 生产保持 false)
    RiskCategory saved_halt_category = _halt_category.load(std::memory_order_acquire);
    bool was_irreversible = (saved_halt_category == RiskCategory::IRREVERSIBLE);
    if (was_irreversible && _recovery_config.auto_clear_irreversible_on_reset) {
        WTSLogger::warn("[RISK] resetDaily: auto-clearing IRREVERSIBLE halt at day boundary "
                        "(autoClearIrreversibleOnReset=true, simulated overnight manual review)");
        was_irreversible = false;
        _halt_category.store(RiskCategory::REVERSIBLE, std::memory_order_release);
        _was_loss_triggered = false;
        _halt_pnl_snapshot = 0;
        broadcastAlert("IRREVERSIBLE_CLEARED", "IRREVERSIBLE halt auto-cleared at day boundary (config-gated)");
    }

    _trading_halted.store(was_irreversible, std::memory_order_relaxed); // Stay halted if IRREVERSIBLE
    _long_blocked.store(false, std::memory_order_relaxed);
    _short_blocked.store(false, std::memory_order_relaxed);
    _quoting_paused.store(was_irreversible, std::memory_order_relaxed); // Stay paused if IRREVERSIBLE
    // _halt_category is preserved (not reset to REVERSIBLE)
    _halt_timestamp = 0;
    _pause_timestamp = 0;
    _last_recovery_check = 0;

    _order_times.clear();
    _cancel_times.clear();
    _trade_times.clear();
    // P2-1: atomic 双轨计数已删除,清零靠 ring buffer clear()

    _delta_snapshots.fill(DeltaSnapshot());
    _delta_snapshot_count = 0;
    _delta_snapshot_head = 0;
    _delta_rate_breached.store(false, std::memory_order_relaxed);
    _delta_rate_breach_time = 0;

    // 新 session 重置自动恢复计数(每 session 最多 max_recovery_count 次)
    _recovery_count = 0;

    if (was_irreversible) {
        WTSLogger::warn("[RISK] resetDaily: IRREVERSIBLE halt preserved, trading remains halted until "
                        "clearIrreversible() is called");
    }
}

bool FutuRiskMonitor::clearIrreversible()
{
    RecursiveSpinGuard _g(_halt_domain_lock); // V8-R6/WS-A
    if (_halt_category.load(std::memory_order_acquire) != RiskCategory::IRREVERSIBLE) {
        WTSLogger::debug("[RISK] clearIrreversible: not in IRREVERSIBLE state, nothing to clear");
        return false;
    }

    WTSLogger::info("[RISK] clearIrreversible: manually clearing IRREVERSIBLE halt (human confirmation)");
    _halt_category.store(RiskCategory::REVERSIBLE, std::memory_order_release);
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
    struct TimedSnap
    {
        uint64_t t;
        double d;
    };
    TimedSnap window_snaps[DELTA_SNAPSHOT_CAPACITY];
    size_t n = 0;

    for (size_t i = 0; i < _delta_snapshot_count; ++i) {
        size_t idx;
        if (_delta_snapshot_count < DELTA_SNAPSHOT_CAPACITY)
            idx = i;
        else
            idx = (_delta_snapshot_head + i) % DELTA_SNAPSHOT_CAPACITY;

        const DeltaSnapshot& snap = _delta_snapshots[idx];
        if (snap.timestamp_ms < cutoff || snap.timestamp_ms == 0)
            continue;

        window_snaps[n++] = {snap.timestamp_ms, snap.delta};
    }

    if (n < 2)
        return 0.0;

    // 按时间升序排序 (n<=32, 插入排序足够)
    for (size_t i = 1; i < n; ++i) {
        TimedSnap key = window_snaps[i];
        size_t j = i;
        while (j > 0 && window_snaps[j - 1].t > key.t) {
            window_snaps[j] = window_snaps[j - 1];
            --j;
        }
        window_snaps[j] = key;
    }

    // 累积 |Δdelta|
    double cumulative_change = 0.0;
    for (size_t i = 1; i < n; ++i) {
        cumulative_change += std::abs(window_snaps[i].d - window_snaps[i - 1].d);
    }

    // 时间分母: 取实际跨度 vs window 配置一半的较大值, 避免短期采样集中导致分母过小
    uint64_t actualSpanMs = window_snaps[n - 1].t - window_snaps[0].t;
    uint64_t minDenomMs = windowMs / 2;
    uint64_t denomMs = (actualSpanMs > minDenomMs) ? actualSpanMs : minDenomMs;
    if (denomMs == 0)
        return 0.0;

    double denomSec = static_cast<double>(denomMs) / 1000.0;
    return cumulative_change / denomSec;
}

bool FutuRiskMonitor::checkAndHandleDeltaRateBreach()
{
    // B3 fix: detection-only — does NOT directly pause/resume quoting.
    // Coordinator::checkRisk manages TradingState transitions based on checkDeltaRate().
    // This avoids dual recovery paths (RiskMonitor _quoting_paused vs Coordinator RISK_HALTED).
    if (_rate_limits.max_delta_change_per_sec <= 0)
        return false;

    double rate = getDeltaChangeRate();
    bool breached = rate > _rate_limits.max_delta_change_per_sec;

    if (breached && !_delta_rate_breached.load(std::memory_order_relaxed)) {
        _delta_rate_breached.store(true, std::memory_order_relaxed);
        _delta_rate_breach_time = _current_time.load(std::memory_order_relaxed);
        broadcastAlert("DELTA_RATE_BREACH",
                       fmt::format("Delta change rate {:.2f}/s exceeds limit {:.2f}/s",
                                   rate,
                                   _rate_limits.max_delta_change_per_sec));
        WTSLogger::warn("[RISK] Delta rate breach: {:.2f}/s > {:.2f}/s", rate, _rate_limits.max_delta_change_per_sec);
        return true;
    }

    if (_delta_rate_breached.load(std::memory_order_relaxed)) {
        uint64_t now = _current_time.load(std::memory_order_relaxed);
        uint64_t cooldownMs = _rate_limits.delta_rate_cooldown_ms;
        if (!breached && (now - _delta_rate_breach_time) >= cooldownMs) {
            _delta_rate_breached.store(false, std::memory_order_relaxed);
            WTSLogger::info("[RISK] Delta rate recovered after cooldown");
        }
    }

    return false;
}

//==========================================================================
// Closeout Management (收盘前平仓) - State Machine Implementation
//==========================================================================

bool FutuRiskMonitor::transitionCloseoutSub(CloseoutSub next_state, uint64_t timestamp)
{
    RecursiveSpinGuard _g(_halt_domain_lock); // V8-R6/WS-A
    // same-state 静默短路 —— 调用方(StrategyCoordinator/UftFutuMmStrategy)在
    // closeout 窗口的 on_tick/on_calc 高频路径里反复调 markCloseoutFlattening,
    // 不应每次都报 warning("Invalid state transition: 2 -> 2")。同 state 视为
    // idempotent no-op 即可,真正的非法转移(如 IDLE → COMPLETED)仍走下面的告警。
    if (_closeout_state.state == next_state) {
        return true; // idempotent
    }
    if (!_closeout_state.canTransitionTo(next_state)) {
        WTSLogger::warn("[CLOSEOUT] Invalid state transition: {} -> {}",
                        static_cast<int>(_closeout_state.state),
                        static_cast<int>(next_state));
        return false;
    }

    _closeout_state.state = next_state;

    switch (next_state) {
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
    RecursiveSpinGuard _g(_halt_domain_lock); // V8-R6/WS-A (递归: transition 内部重入)
    if (transitionCloseoutSub(CloseoutSub::TRIGGERED, timestamp)) {
        pauseQuoting();
        broadcastAlert("CLOSEOUT_TRIGGERED", fmt::format("Closeout state: TRIGGERED at {}", timestamp));
    }
}

void FutuRiskMonitor::markCloseoutDraining(uint64_t timestamp)
{
    RecursiveSpinGuard _g(_halt_domain_lock); // V8-R6/WS-A
    if (transitionCloseoutSub(CloseoutSub::DRAINING, timestamp)) {
        broadcastAlert("CLOSEOUT_DRAINING", fmt::format("Closeout state: DRAINING at {}", timestamp));
    }
}

void FutuRiskMonitor::markCloseoutCompleted(uint64_t timestamp)
{
    RecursiveSpinGuard _g(_halt_domain_lock); // V8-R6/WS-A
    if (transitionCloseoutSub(CloseoutSub::COMPLETED, timestamp)) {
        // 标记夜盘 closeout 已完成，防止 reset 后重触发
        if (_closeout_state.is_night_closeout)
            _closeout_state.night_closeout_done = true;
        broadcastAlert("CLOSEOUT_COMPLETED", fmt::format("Closeout state: COMPLETED at {}", timestamp));
    }
}

void FutuRiskMonitor::markCloseoutFailed(uint64_t timestamp)
{
    RecursiveSpinGuard _g(_halt_domain_lock); // V8-R6/WS-A
    if (transitionCloseoutSub(CloseoutSub::FAILED, timestamp)) {
        if (_closeout_state.retry_count >= _closeout_state.max_retries) {
            broadcastAlert("CLOSEOUT_FAILED",
                           fmt::format("Closeout FAILED at {} (retries exhausted: {}/{}), manual intervention required",
                                       timestamp,
                                       _closeout_state.retry_count,
                                       _closeout_state.max_retries));
        } else {
            broadcastAlert("CLOSEOUT_FAILED",
                           fmt::format("Closeout FAILED at {} (retry {}/{}), will retry in {}ms",
                                       timestamp,
                                       _closeout_state.retry_count,
                                       _closeout_state.max_retries,
                                       _closeout_state.retry_interval_ms));
        }
    }
}

bool FutuRiskMonitor::checkCloseoutRetry(uint64_t current_time_ms)
{
    RecursiveSpinGuard _g(_halt_domain_lock); // V8-R6/WS-A (递归: transition 内部重入)
    if (_closeout_state.state != CloseoutSub::FAILED)
        return false;

    if (_closeout_state.retry_count >= _closeout_state.max_retries) {
        WTSLogger::error("[CLOSEOUT] Max retries ({}) exhausted, manual intervention required",
                         _closeout_state.max_retries);
        return false;
    }

    if (current_time_ms - _closeout_state.fail_time < _closeout_state.retry_interval_ms)
        return false;

    if (transitionCloseoutSub(CloseoutSub::RETRYING, current_time_ms)) {
        broadcastAlert("CLOSEOUT_RETRYING",
                       fmt::format("Closeout retry {}/{} at {}",
                                   _closeout_state.retry_count,
                                   _closeout_state.max_retries,
                                   current_time_ms));
        return true;
    }

    return false;
}

bool FutuRiskMonitor::checkCloseout(uint32_t currentTime, uint32_t closeTime)
{
    RecursiveSpinGuard _g(_halt_domain_lock); // V8-R6/WS-A (递归: markCloseoutTriggered 内部重入)
    if (_closeout_state.state == CloseoutSub::COMPLETED || _closeout_state.state == CloseoutSub::FAILED)
        return false;

    // 5A-1: 时间窗口判定 (时段门/格式校验/跨日映射) 统一走
    //   SessionPhaseManager 静态函数; 此处保留状态机门
    //   (IDLE / night_closeout_done) 与触发副作用 (状态标记/日志/转换)。
    uint32_t currentHour, currentMin;
    if (!SessionPhaseManager::parseHhmm(currentTime, currentHour, currentMin)) {
        WTSLogger::warn("[RISK] Invalid current time format: {}", currentTime);
        return false;
    }

    //==========================================================================
    // 双触发点平仓逻辑
    //
    // 有夜盘的品种有两个平仓触发点:
    //   1. 夜盘收盘前 night_minutes_before 分钟 (如 02:25)
    //   2. 全天收盘前 minutes_before 分钟 (如 15:10)
    //==========================================================================

    // --- 触发点1: 夜盘收盘 ---
    // 跳过已完成的夜盘 closeout（防止 reset 后重触发）
    if (!_closeout_state.night_closeout_done &&
        SessionPhaseManager::inNightCloseoutWindow(
            currentTime, _closeout_config.night_close_time, _closeout_config.night_minutes_before) &&
        _closeout_state.state == CloseoutSub::IDLE) {
        uint32_t nightCloseHour = _closeout_config.night_close_time / 100;
        uint32_t nightCloseMin = _closeout_config.night_close_time % 100;
        bool is_overnight = (nightCloseHour < 6);
        // record this is a night closeout so COMPLETED handler
        // only resets state for night→day transition, not day closeout
        _closeout_state.is_night_closeout = true;
        markCloseoutTriggered(currentTime * 100);
        if (is_overnight) {
            broadcastAlert(
                "CLOSEOUT_TRIGGERED",
                fmt::format("Night closeout triggered at {}:{:02d}, night close {}:{:02d}+1d, {} minutes before",
                            currentHour,
                            currentMin,
                            nightCloseHour,
                            nightCloseMin,
                            _closeout_config.night_minutes_before));
        } else {
            broadcastAlert(
                "CLOSEOUT_TRIGGERED",
                fmt::format("Night closeout triggered at {}:{:02d}, night close {}:{:02d}, {} minutes before",
                            currentHour,
                            currentMin,
                            nightCloseHour,
                            nightCloseMin,
                            _closeout_config.night_minutes_before));
        }
        return true;
    }

    // --- 触发点2: 全天收盘 (白盘) ---
    if (SessionPhaseManager::inDayCloseoutWindow(currentTime, closeTime, _closeout_config.minutes_before) &&
        _closeout_state.state == CloseoutSub::IDLE) {
        // 日志用收盘时间 (与 inDayCloseoutWindow 内同一解析+修正逻辑)
        uint32_t closeHour, closeMin;
        if (closeTime < 10000) {
            closeHour = closeTime / 100;
            closeMin = closeTime % 100;
        } else {
            closeHour = closeTime / 10000;
            closeMin = (closeTime / 100) % 100;
        }
        if (closeHour > 23 || closeMin > 59) {
            closeHour = 15;
            closeMin = 15;
        }

        // day closeout, not night; reset night flag for next session
        _closeout_state.is_night_closeout = false;
        _closeout_state.night_closeout_done = false;
        markCloseoutTriggered(currentTime * 100);
        broadcastAlert("CLOSEOUT_TRIGGERED",
                       fmt::format("Day closeout triggered at {}:{:02d}, close time {}:{:02d}, {} minutes before",
                                   currentHour,
                                   currentMin,
                                   closeHour,
                                   closeMin,
                                   _closeout_config.minutes_before));
        return true;
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
    RecursiveSpinGuard _g(_halt_domain_lock); // V8-R6/WS-A
    if (force || _closeout_state.canTransitionTo(CloseoutSub::IDLE)) {
        _closeout_state.state = CloseoutSub::IDLE;
        _closeout_state.trigger_time = 0;
        _closeout_state.flatten_start = 0;
        _closeout_state.complete_time = 0;
        _closeout_state.fail_time = 0;
        _closeout_state.retry_count = 0;
        _closeout_state.is_night_closeout = false; // reset night flag
        // NOTE: night_closeout_done 不在此重置，只在新白盘 closeout 触发时重置
        // 这样 reset 后夜盘时段不会重触发夜盘 closeout
    } else {
        WTSLogger::warn("FutuRiskMonitor: resetCloseout blocked by state machine, "
                        "current state={} — cannot transition to IDLE",
                        static_cast<int>(_closeout_state.state));
    }
}

PreTradeDecision FutuRiskMonitor::checkPreTradePosition(const std::string& code,
                                                                       const FutuPortfolio* portfolio,
                                                                       const UnifiedOrderTracker* tracker,
                                                                       uint64_t now_ms) const
{
    // v3 软风控：不再 BLOCK，返回 utilization 让 Quoter 做 qty 衰减
    // A3: 委托 checkPreTradePositionImpl, 仅负责 ContractState 快照获取
    if (!portfolio)
        return PreTradeDecision{};

    ContractState cs_buf;
    const ContractState* cs = portfolio->getContractSnapshot(code, cs_buf) ? &cs_buf : nullptr;
    // 2026-08-19 语义边界: 不在此按 max_position 早退 — 风控/策略各自在 impl 内
    // 按自身口径设防 (风控=maxPosition, 策略=contract_max_delta), 避免硬顶未配时
    // 误伤策略库存调控输入.
    if (!cs)
        return PreTradeDecision{};

    return checkPreTradePositionImpl(code, cs, tracker, now_ms);
}

PreTradeDecision FutuRiskMonitor::checkPreTradePosition(const ContractState& cs,
                                                                       const UnifiedOrderTracker* tracker,
                                                                       uint64_t now_ms) const
{
    // A3: 复用 TickContext.cs 快照 (processTick 入口 preCheck 已 getContractSnapshot),
    //     消除每 tick checkPreTradePosition 的重复递归锁+ContractState 拷贝
    return checkPreTradePositionImpl(cs.code, &cs, tracker, now_ms);
}

PreTradeDecision FutuRiskMonitor::checkPreTradePositionImpl(const std::string& code,
                                                                           const ContractState* cs,
                                                                           const UnifiedOrderTracker* tracker,
                                                                           uint64_t now_ms) const
{
    PreTradeDecision result;
    double pending_buy = tracker ? tracker->getPendingBuyQtyAllSources(code) : 0;
    double pending_sell = tracker ? tracker->getPendingSellQtyAllSources(code) : 0;
    result.risk = checkHardPositionRisk(code, cs, pending_buy, pending_sell, now_ms);
    result.strategy = computeInventoryStrategyInputs(code, cs, pending_buy, pending_sell);
    return result;
}

RiskVerdict FutuRiskMonitor::checkHardPositionRisk(const std::string& code,
                                                  const ContractState* cs,
                                                  double pending_buy,
                                                  double pending_sell,
                                                  uint64_t now_ms) const
{
    RiskVerdict risk;
    // === B+: zombie 撤单升级闩锁 - zombie 单悬而未决, 暂停该合约全部报单 ===
    // (独立于 maxPosition 闸门: 即使未配硬顶, zombie 风险也必须停牌)
    // B+ 修复(P1-1): 数据/交易线程并发读, 自旋锁保护
    {
        SpinLockGuard _g(_zombie_halt_lock);
        if (_zombie_halt.count(code) > 0) {
            risk.halt_quoting = true;
        }
    }

    if (!cs || cs->max_position <= 0)
        return risk;

    // 同侧连续成交熔断（风控层硬闸门，按合约独立计数）:
    // 该合约该侧处于暂停期 -> 禁止报价（cancelAll + 不挂新单），到期自动恢复。
    // now_ms 为交易所时钟 (replay 基准, 与 onSideFill 写入一致), 0 = 不启用查询。
    if (now_ms > 0) {
        // V8-R1: 熔断器本身不分方向 (isPaused 无 side 参数), 同侧触发即暂停
        // 全合约报价, cancelAll 也是全撤 — 两字段镜像同一合约级状态属有意
        // 为之, 非每侧独立暂停。若未来需要分侧语义需先给 breaker 加方向维度。
        risk.side_pause_bid = _side_fill_breaker.isPaused(code, now_ms);
        risk.side_pause_ask = risk.side_pause_bid;
    }

    // B+ 修复(P1-1): 告警节流表双线程读改写, 自旋锁保护
    const uint64_t soft_now = _current_time.load(std::memory_order_relaxed);
    bool log_soft = false;
    {
        SpinLockGuard _g(_soft_warn_lock);
        uint64_t& last_soft = _last_soft_warn_ms[code];
        log_soft = (soft_now < last_soft || soft_now - last_soft >= WARN_THROTTLE_MS);
        if (log_soft)
            last_soft = soft_now;
    }

    // === Pending OrderFilter: per-side pending qty drain (风险层) ===
    if (_max_pending_per_side > 0) {
        if (pending_buy > _max_pending_per_side) {
            risk.pending_drain_bid = true;
            if (log_soft)
                WTSLogger::warn("[RISK] {} PENDING_DRAIN: pending_buy={:.0f} > {:.0f} -> drain bid",
                                code,
                                pending_buy,
                                _max_pending_per_side);
        }
        if (pending_sell > _max_pending_per_side) {
            risk.pending_drain_ask = true;
            if (log_soft)
                WTSLogger::warn("[RISK] {} PENDING_DRAIN: pending_sell={:.0f} > {:.0f} -> drain ask",
                                code,
                                pending_sell,
                                _max_pending_per_side);
        }
    }

    // === halt_quoting: 风控措施 (净头寸硬停止) ===
    // 触发依据: 净头寸 (cs->position 为 strategy book net) 严格超过 maxPosition.
    // 动作: 暂停该合约全部报单 (Quoter 入口 cancelAll + 不再挂新单).
    // 恢复: 每 tick 重估, 净头寸回落到 maxPosition 以内自动恢复; 减仓依赖 closeout 或人工介入.
    if (std::abs(cs->position) > cs->max_position) {
        risk.halt_quoting = true;
    }

    return risk;
}

StrategyInputs FutuRiskMonitor::computeInventoryStrategyInputs(const std::string& code,
                                                               const ContractState* cs,
                                                               double pending_buy,
                                                               double pending_sell) const
{
    StrategyInputs strategy;
    // 策略库存调控 = delta 口径 (2026-08-19 语义边界原则):
    //   分子 = 同向 delta + 同向 pending×hedge_ratio, 分母 = contract_max_delta (delta 软限).
    //   maxPosition 是风控硬顶, 只用于 RiskVerdict (checkHardPositionRisk), 不参与此函数.
    if (!cs || cs->contract_max_delta <= 0)
        return strategy;

    const double delta = cs->delta(); // position * hedge_ratio
    const double hr = cs->hedge_ratio;
    double projected_long = (delta > 0 ? delta : 0) + pending_buy * hr;
    double projected_short = (delta < 0 ? std::abs(delta) : 0) + pending_sell * hr;

    strategy.long_delta_util = projected_long / cs->contract_max_delta;
    strategy.short_delta_util = projected_short / cs->contract_max_delta;

    // 热路径告警限频：per-contract 软告警 (cap/block_add)
    // 在持续超限时每 tick 刷屏，按时间节流至最多每 1s 一次。
    // B+ 修复(P1-1): 双线程读改写, 自旋锁保护
    const uint64_t soft_now = _current_time.load(std::memory_order_relaxed);
    bool log_soft = false;
    {
        SpinLockGuard _g(_soft_warn_lock);
        uint64_t& last_soft = _last_soft_warn_ms[code];
        log_soft = (soft_now < last_soft || soft_now - last_soft >= WARN_THROTTLE_MS);
        if (log_soft)
            last_soft = soft_now;
    }

    // v3: delta util >= 1.0 时只设 obligation 标志，不阻断；Quoter 负责
    //     (A) 加仓侧 qty 指数衰减 (util接近1时qty→0)
    //     (B) 减仓侧强制义务报价 (≥10手/≤10ticks)
    if (strategy.long_delta_util >= 1.0) {
        strategy.force_ask_obligation = true;
        if (log_soft)
            WTSLogger::warn("[STRATEGY] {} LONG delta cap reached: delta={:.0f} pending_buy={:.0f} proj_long={:.0f}/{:.0f} "
                            "(util={:.2f}) → ASK obligation",
                            code,
                            delta,
                            pending_buy,
                            projected_long,
                            cs->contract_max_delta,
                            strategy.long_delta_util);
    }
    if (strategy.short_delta_util >= 1.0) {
        strategy.force_bid_obligation = true;
        if (log_soft)
            WTSLogger::warn("[STRATEGY] {} SHORT delta cap reached: delta={:.0f} pending_sell={:.0f} proj_short={:.0f}/{:.0f} "
                            "(util={:.2f}) → BID obligation",
                            code,
                            delta,
                            pending_sell,
                            projected_short,
                            cs->contract_max_delta,
                            strategy.short_delta_util);
    }

    // === block_add: 策略库存管理 (仅 flexible 加仓侧, 非风控措施) ===
    // obligation 加仓侧由被动价承担 (obligation_max_spread_ticks), 不在此阻断.
    // 阈值 = contract_max_delta × positionHardBlockRatio (delta 口径; 配置键名保留兼容).
    if (_rate_limits.position_hard_block_ratio > 0) {
        double abs_delta = std::abs(delta);
        double hard_threshold = cs->contract_max_delta * _rate_limits.position_hard_block_ratio;
        if (abs_delta >= hard_threshold) {
            if (delta > 0) {
                strategy.block_add_long = true;
            } else {
                strategy.block_add_short = true;
            }
            if (log_soft)
                WTSLogger::warn("[STRATEGY] {} BLOCK_ADD: delta={:.0f} >= {:.0f}*{:.2f} -> flexible stop adding {} (inventory management)",
                                code,
                                delta,
                                cs->contract_max_delta,
                                _rate_limits.position_hard_block_ratio,
                                delta > 0 ? "long(bid)" : "short(ask)");
        }
    }

    return strategy;
}

} // namespace futu
