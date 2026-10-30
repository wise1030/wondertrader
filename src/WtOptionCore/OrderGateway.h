/*!
 * \file OrderGateway.h
 * \brief Narrow production order gateway seam used by OQM/OTG.
 *
 * The gateway intentionally exposes only id-producing submission and cancel
 * operations.  Strategy-side policy (price limits, anomaly registration,
 * pre-trade checks) is composed by the owner of these callbacks before an
 * OrderGateway instance is supplied to OptionTradingGrid.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace wt_option {

class OrderGateway {
public:
    using SubmitQuoteFn = std::function<std::pair<uint32_t, uint32_t>(
        const std::string& code, double bidP, uint32_t bidQ,
        double askP, uint32_t askQ)>;
    using SubmitOrderFn = std::function<std::vector<uint32_t>(
        const std::string& code, bool isBuy, double price, uint32_t qty)>;
    using CancelOrderFn = std::function<bool(uint32_t localid)>;
    using CancelAllFn = std::function<void(const std::string& code)>;

    struct Handlers {
        SubmitQuoteFn submitQuote;
        SubmitOrderFn submitOrder;
        CancelOrderFn cancelOrder;
        CancelAllFn cancelAll;
    };

    explicit OrderGateway(Handlers handlers) : m_handlers(std::move(handlers)) {}

    bool ready() const {
        return static_cast<bool>(m_handlers.submitQuote)
            && static_cast<bool>(m_handlers.submitOrder)
            && static_cast<bool>(m_handlers.cancelOrder)
            && static_cast<bool>(m_handlers.cancelAll);
    }

    std::pair<uint32_t, uint32_t> submitQuote(const std::string& code,
                                              double bidP, uint32_t bidQ,
                                              double askP, uint32_t askQ) const {
        if (!m_handlers.submitQuote) return {0, 0};
        return m_handlers.submitQuote(code, bidP, bidQ, askP, askQ);
    }

    std::vector<uint32_t> submitOrder(const std::string& code, bool isBuy,
                                      double price, uint32_t qty) const {
        if (!m_handlers.submitOrder) return {};
        return m_handlers.submitOrder(code, isBuy, price, qty);
    }

    bool cancelOrder(uint32_t localid) const {
        if (!m_handlers.cancelOrder) return false;
        return m_handlers.cancelOrder(localid);
    }

    void cancelAll(const std::string& code) const {
        if (m_handlers.cancelAll) m_handlers.cancelAll(code);
    }

private:
    Handlers m_handlers;
};

using OrderGatewayPtr = std::shared_ptr<OrderGateway>;

} // namespace wt_option
