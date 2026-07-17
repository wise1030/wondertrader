#include "ComboOrders.h"
#include "OptionData.h"
#include "../WTSTools/WTSLogger.h"

#include <cmath>
#include <algorithm>

namespace wt_option {

// ============================================================================
// SpreadComboOrder
// ============================================================================

SpreadComboOrder::SpreadComboOrder(const std::string& name, uint32_t totalSize,
                                     double tickSize, ComboExecContext* ctx)
    : ComboOrder(name)
    , m_execCtx(ctx)
    , m_tickSize(tickSize)
    , m_totalSize(totalSize)
{
}

void SpreadComboOrder::setupLegs(OptionData* leg1, bool leg1IsBuy,
                                   OptionData* leg2, bool leg2IsBuy,
                                   double leg1Price, double leg2Price)
{
    if (!leg1 || !leg2) return;
    m_leg1Code = leg1->getCode();
    m_leg2Code = leg2->getCode();
    m_leg1IsBuy = leg1IsBuy;
    m_leg2IsBuy = leg2IsBuy;
    m_leg1Price = leg1Price;
    m_leg2Price = leg2Price;
}

ComboOrder::SendResult SpreadComboOrder::sendOrders()
{
    if (!m_execCtx || !m_execCtx->sendOrder) {
        WTSLogger::log_by_cat("strategy", LL_ERROR,
            "SpreadComboOrder {}: no executor", m_name);
        return SendResult::Failed;
    }

    // Send leg1 only first (sequential execution)
    m_leg1LocalId = m_execCtx->sendOrder(m_leg1Code, m_leg1IsBuy, m_leg1Price, m_totalSize);
    if (m_leg1LocalId == 0) {
        WTSLogger::log_by_cat("strategy", LL_WARN,
            "SpreadComboOrder {}: leg1 send failed {}", m_name, m_leg1Code);
        return SendResult::Failed;
    }

    m_leg1Sent = true;
    m_active = true;
    m_sendTime = m_execCtx->getTime ? m_execCtx->getTime() : 0;

    WTSLogger::log_by_cat("strategy", LL_INFO,
        "SpreadComboOrder {} leg1 sent: {} {} {}@{}",
        m_name, m_leg1Code, m_leg1IsBuy ? "BUY" : "SELL", m_totalSize, m_leg1Price);

    return SendResult::Success;
}

void SpreadComboOrder::sendLeg2()
{
    if (m_leg2Sent || !m_execCtx || !m_execCtx->sendOrder) return;

    // Price improvement: +1 tick on hedge leg for better fill probability
    double improvedPrice = m_leg2Price;
    if (m_tickSize > 0) {
        // If buying leg2, raise price by 1 tick (more aggressive)
        // If selling leg2, lower price by 1 tick
        improvedPrice += (m_leg2IsBuy ? 1 : -1) * m_tickSize;
    }

    m_leg2LocalId = m_execCtx->sendOrder(m_leg2Code, m_leg2IsBuy, improvedPrice, m_totalSize);
    if (m_leg2LocalId > 0) {
        m_leg2Sent = true;
        WTSLogger::log_by_cat("strategy", LL_INFO,
            "SpreadComboOrder {} leg2 sent: {} {} {}@{} (improved from {})",
            m_name, m_leg2Code, m_leg2IsBuy ? "BUY" : "SELL",
            m_totalSize, improvedPrice, m_leg2Price);
    } else {
        WTSLogger::log_by_cat("strategy", LL_ERROR,
            "SpreadComboOrder {} leg2 send FAILED: {}", m_name, m_leg2Code);
        // Leg2 failed - cancel leg1 if it has fills remaining
        if (m_leg1LocalId > 0 && m_execCtx->cancelOrder)
            m_execCtx->cancelOrder(m_leg1LocalId);
        m_done = true;
    }
}

void SpreadComboOrder::onFill(const OptionOrder& order, const FillEvent& fill)
{
    if (order.getOrderId() == m_leg1LocalId && !m_leg2Sent) {
        // Leg1 filled - send leg2 with actual fill quantity
        uint32_t fillQty = fill.fillQty;
        if (fillQty > 0 && fillQty != m_totalSize) {
            // Partial fill - adjust leg2 size to actual fill
            WTSLogger::log_by_cat("strategy", LL_INFO,
                "SpreadComboOrder {} leg1 partial fill {}/{}, adjusting leg2",
                m_name, fillQty, m_totalSize);
            m_totalSize = fillQty;  // Adjust hedge size
        }
        sendLeg2();
    }

    // Check if done
    if (checkDone(false)) {
        m_done = true;
        m_active = false;
    }
}

bool SpreadComboOrder::checkDone(bool timeout)
{
    if (timeout) {
        // Timeout - cancel any unfilled legs
        cancelAll();
        m_done = true;
        m_active = false;
        return true;
    }

    // Done if leg2 is sent and both legs are filled or cancelled
    if (!m_leg2Sent) return false;

    // For simplicity: done if leg2 was sent (we don't track individual fills
    // in this simplified version - the OQM handles per-order fills)
    return m_leg2Sent;
}

bool SpreadComboOrder::checkTimeout()
{
    if (!m_active || m_done) return false;
    if (m_sendTime == 0 || !m_execCtx->getTime) return false;

    double now = m_execCtx->getTime();
    if ((now - m_sendTime) > m_timeoutSec) {
        WTSLogger::log_by_cat("strategy", LL_WARN,
            "SpreadComboOrder {} timeout after {:.0f}ms, cancelling",
            m_name, (now - m_sendTime) * 1000);
        return true;
    }
    return false;
}

void SpreadComboOrder::cancelAll()
{
    if (m_execCtx && m_execCtx->cancelOrder) {
        if (m_leg1LocalId > 0 && !m_leg2Sent) {
            m_execCtx->cancelOrder(m_leg1LocalId);
        }
        if (m_leg2LocalId > 0) {
            m_execCtx->cancelOrder(m_leg2LocalId);
        }
    }
    m_active = false;
}

// ============================================================================
// SynComboOrder - 3-leg synthetic future
// ============================================================================

SynComboOrder::SynComboOrder(const std::string& name, uint32_t totalSize,
                               double tickSize, int32_t optFutRatio,
                               ComboExecContext* ctx)
    : ComboOrder(name)
    , m_execCtx(ctx)
    , m_tickSize(tickSize)
    , m_optFutRatio(optFutRatio > 0 ? optFutRatio : 1)
    , m_legExecs(3)  // call, put, future
{
}

void SynComboOrder::setupLegs(OptionData* call, OptionData* put,
                                const std::string& futureCode,
                                bool buyCall, bool buyPut, bool buyFuture,
                                double callPrice, double putPrice, double futurePrice)
{
    if (!call || !put) return;

    // Leg 0: call
    m_legExecs[0].code = call->getCode();
    m_legExecs[0].isBuy = buyCall;
    m_legExecs[0].price = callPrice;
    m_legExecs[0].desiredQty = m_totalSize;

    // Leg 1: put
    m_legExecs[1].code = put->getCode();
    m_legExecs[1].isBuy = buyPut;
    m_legExecs[1].price = putPrice;
    m_legExecs[1].desiredQty = m_totalSize;

    // Leg 2: future (hedge) - size adjusted by option/future ratio
    m_legExecs[2].code = futureCode;
    m_legExecs[2].isBuy = buyFuture;
    m_legExecs[2].price = futurePrice;
    m_legExecs[2].desiredQty = m_totalSize / m_optFutRatio;
}

ComboOrder::SendResult SynComboOrder::sendOrders()
{
    if (!m_execCtx || !m_execCtx->sendOrder) {
        WTSLogger::log_by_cat("strategy", LL_ERROR,
            "SynComboOrder {}: no executor", m_name);
        return SendResult::Failed;
    }

    // Send leg1 (first option) only - sequential execution
    m_nextLegToSend = 0;
    sendNextLeg();

    if (m_legExecs[0].sent) {
        m_active = true;
        m_sendTime = m_execCtx->getTime ? m_execCtx->getTime() : 0;
        return SendResult::Success;
    }
    return SendResult::Failed;
}

void SynComboOrder::sendNextLeg()
{
    if (m_nextLegToSend >= (int)m_legExecs.size()) return;

    auto& leg = m_legExecs[m_nextLegToSend];
    if (leg.sent) return;

    // Price improvement: +1 tick on hedge legs
    double price = leg.price;
    if (m_tickSize > 0 && m_nextLegToSend > 0) {
        price += (leg.isBuy ? 1 : -1) * m_tickSize;
    }

    leg.localId = m_execCtx->sendOrder(leg.code, leg.isBuy, price, leg.desiredQty);
    if (leg.localId > 0) {
        leg.sent = true;
        WTSLogger::log_by_cat("strategy", LL_INFO,
            "SynComboOrder {} leg{} sent: {} {} {}@{}",
            m_name, m_nextLegToSend, leg.code,
            leg.isBuy ? "BUY" : "SELL", leg.desiredQty, price);
    } else {
        WTSLogger::log_by_cat("strategy", LL_ERROR,
            "SynComboOrder {} leg{} send FAILED: {}",
            m_name, m_nextLegToSend, leg.code);
        // Cancel all previous legs
        cancelAll();
        m_done = true;
    }
}

void SynComboOrder::onFill(const OptionOrder& order, const FillEvent& fill)
{
    // Find which leg filled
    for (int i = 0; i < (int)m_legExecs.size(); i++) {
        if (m_legExecs[i].localId == order.getOrderId() && !m_legExecs[i].filled) {
            m_legExecs[i].filled = true;
            m_legExecs[i].filledQty = fill.fillQty;

            WTSLogger::log_by_cat("strategy", LL_INFO,
                "SynComboOrder {} leg{} filled: {} qty={}",
                m_name, i, m_legExecs[i].code, fill.fillQty);

            // Adjust next leg's quantity based on actual fill
            if (i + 1 < (int)m_legExecs.size()) {
                // For future hedge: adjust by option/future ratio
                if (i == 1) {  // put filled -> send future with ratio
                    m_legExecs[2].desiredQty = fill.fillQty / m_optFutRatio;
                } else {
                    m_legExecs[i + 1].desiredQty = fill.fillQty;
                }
                sendNextLeg();
            }
            break;
        }
    }

    if (checkDone(false)) {
        m_done = true;
        m_active = false;
    }
}

bool SynComboOrder::checkDone(bool timeout)
{
    if (timeout) {
        cancelAll();
        m_done = true;
        m_active = false;
        return true;
    }

    // Done when last leg is sent (simplified)
    return m_legExecs[2].sent;
}

bool SynComboOrder::checkTimeout()
{
    if (!m_active || m_done) return false;
    if (m_sendTime == 0 || !m_execCtx->getTime) return false;

    double now = m_execCtx->getTime();
    if ((now - m_sendTime) > m_timeoutSec) {
        WTSLogger::log_by_cat("strategy", LL_WARN,
            "SynComboOrder {} timeout after {:.0f}ms, cancelling",
            m_name, (now - m_sendTime) * 1000);
        return true;
    }
    return false;
}

void SynComboOrder::cancelAll()
{
    if (m_execCtx && m_execCtx->cancelOrder) {
        for (auto& leg : m_legExecs) {
            if (leg.localId > 0 && !leg.filled) {
                m_execCtx->cancelOrder(leg.localId);
            }
        }
    }
    m_active = false;
}

} // namespace wt_option
