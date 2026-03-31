/*!
 * \file UnderlyingTradingData.cpp
 * \brief Underlying trading data implementation
 */

#include "UnderlyingTradingData.h"

namespace wt_option {

UnderlyingTradingData::UnderlyingTradingData(const std::string& code)
    : m_code(code)
    , m_position(0)
    , m_quoteMode(QuoteMode::Off)
{
}

void UnderlyingTradingData::updateMarket(const UnderlyingMarket& market) {
    m_market = market;
    // Auto-update values?
    // In longbeach, values are computed by Pricer.
    // For underlying, Theo is usually Mid or Forward.
}

void UnderlyingTradingData::addFill(int32_t qty, double price) {
    m_position += qty;
}

} // namespace wt_option
