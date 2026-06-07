/*!
 * \file TradingStateManager.h
 * \brief Unified Trading State Management - Single Source of Truth
 * 
 * Consolidates trading state from UftFutuMmStrategy and StrategyCoordinator
 * to prevent state inconsistency.
 * 
 * Part of WtFutuCore - Futures High-Frequency Market Making Engine
 */
#pragma once

#include <string>
#include <cstdint>

// FIX P0-7: SSOT — syncFromRiskMonitor需要完整类型定义
#include "FutuRiskMonitor.h"

namespace futu {

/// 交易状态 - 单一 Source of Truth
struct TradingState {
    bool trading_halted = false;      ///< 交易暂停（风控触发）
    bool quoting_paused = false;      ///< 报价暂停（下单错误等）
    bool long_blocked = false;        ///< 禁止买入
    bool short_blocked = false;       ///< 禁止卖出
    bool toxicity_paused = false;     ///< 毒性暂停
    bool market_paused = false;       ///< 市场状态暂停（极端波动等）
    bool closeout_mode = false;       ///< 收盘平仓模式
    
    /// 暂停原因追踪
    enum class PauseReason : uint8_t {
        NONE = 0,
        RISK_LIMIT,       ///< 风控限制
        TOXICITY,          ///< 毒性流
        ORDER_ERROR,       ///< 下单错误
        CLOSEOUT,          ///< 收盘平仓
        MANUAL,            ///< 手动暂停
        DELTA_LIMIT,       ///< Delta限制
        MARKET_VOLATILITY  ///< 市场波动暂停(EXTREME vol tier)
    };
    
    PauseReason pause_reason = PauseReason::NONE;
    
    //==========================================================================
    // 统一查询接口
    //==========================================================================
    
    bool canQuote() const { 
        return !trading_halted && !quoting_paused && !toxicity_paused && !market_paused; 
    }
    bool canBuy() const { 
        return canQuote() && !long_blocked; 
    }
    bool canSell() const { 
        return canQuote() && !short_blocked; 
    }
    bool isActive() const {
        return !trading_halted && !quoting_paused && !market_paused;
    }
    
    //==========================================================================
    // 状态变更方法（集中管理，方便日志和审计）
    //==========================================================================
    
    void halt(PauseReason reason) {
        if (!trading_halted) {
            trading_halted = true;
            pause_reason = reason;
        }
    }
    
    void resume() {
        trading_halted = false;
        quoting_paused = false;
        toxicity_paused = false;
        market_paused = false;  // FIX: resume()必须清除market_paused，否则isActive()/canQuote()仍返回false
        long_blocked = false;
        short_blocked = false;
        pause_reason = PauseReason::NONE;
    }
    
    void pauseQuoting(PauseReason reason) {
        quoting_paused = true;
        pause_reason = reason;
    }
    
    void resumeQuoting() {
        quoting_paused = false;
        if (pause_reason == PauseReason::ORDER_ERROR) {
            pause_reason = PauseReason::NONE;
        }
    }
    
    void pauseForToxicity() {
        toxicity_paused = true;
        pause_reason = PauseReason::TOXICITY;
    }
    
    void resumeFromToxicity() {
        toxicity_paused = false;
        if (pause_reason == PauseReason::TOXICITY) {
            pause_reason = PauseReason::NONE;
        }
    }
    
    // FIX P0-7: 统一风控恢复方法，避免直接赋值绕过状态机
    void resumeFromRisk() {
        trading_halted = false;
        quoting_paused = false;
        market_paused = false;  // FIX: 同resume()，必须清除market_paused
        if (pause_reason == PauseReason::RISK_LIMIT) {
            pause_reason = PauseReason::NONE;
        }
    }
    
    void pauseForMarket() {
        market_paused = true;
        pause_reason = PauseReason::MARKET_VOLATILITY;  // FIX: 用独立的MARKET_VOLATILITY而非RISK_LIMIT
    }
    
    void resumeFromMarket() {
        market_paused = false;
        if (pause_reason == PauseReason::MARKET_VOLATILITY && !trading_halted) {
            pause_reason = PauseReason::NONE;
        }
    }
    
    void blockLong() { long_blocked = true; }
    void unblockLong() { long_blocked = false; }
    void blockShort() { short_blocked = true; }
    void unblockShort() { short_blocked = false; }
    
    // FIX P0-7: SSOT — 从RiskMonitor同步状态（唯一合法的跨模块同步路径）
    // 替代UftFutuMmStrategy中对trading_halted/quoting_paused等字段的直接赋值
    void syncFromRiskMonitor(const FutuRiskMonitor* monitor) {
        if (!monitor) return;
        trading_halted = monitor->isTradingHalted();
        quoting_paused = monitor->isQuotingPaused();
        long_blocked = monitor->isLongBlocked();
        short_blocked = monitor->isShortBlocked();
    }
    
    /// 获取暂停原因描述
    const char* getPauseReasonStr() const {
        switch (pause_reason) {
            case PauseReason::NONE:        return "NONE";
            case PauseReason::RISK_LIMIT:  return "RISK_LIMIT";
            case PauseReason::TOXICITY:    return "TOXICITY";
            case PauseReason::ORDER_ERROR: return "ORDER_ERROR";
            case PauseReason::CLOSEOUT:    return "CLOSEOUT";
            case PauseReason::MANUAL:      return "MANUAL";
            case PauseReason::DELTA_LIMIT: return "DELTA_LIMIT";
            default:                       return "UNKNOWN";
        }
    }
};

} // namespace futu
