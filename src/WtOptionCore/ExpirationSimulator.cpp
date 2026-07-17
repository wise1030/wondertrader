#include "ExpirationSimulator.h"
#include "OptionData.h"
#include "ExpiryData.h"
#include "OptionRisk.h"
#include "OptionGrid.h"
#include "../WTSTools/WTSLogger.h"

#include <cmath>
#include <algorithm>

namespace wt_option {

ExpirationSimulator::ExpirationSimulator(double multiplier)
    : m_multiplier(multiplier)
{
}

void ExpirationSimulator::onFill(const std::string& code, bool isBuy,
    double price, uint32_t qty, double fee, uint64_t time)
{
    FillRecord rec;
    rec.code = code;
    rec.isBuy = isBuy;
    rec.price = price;
    rec.qty = qty;
    rec.fee = fee;
    rec.time = time;
    m_fills.push_back(rec);
}

DayResult ExpirationSimulator::checkExpiration(uint32_t curDate, double spotPrice)
{
    DayResult result;
    result.date = curDate;

    // 1. Trading PnL from fills
    double tradingPnl = 0;
    for (const auto& fill : m_fills) {
        double dir = fill.isBuy ? 1.0 : -1.0;
        tradingPnl += dir * fill.price * fill.qty * m_multiplier;
        tradingPnl -= fill.fee;
    }

    // 2. Settlement + close PnL from positions
    double settlementPnl = 0;
    double closePnl = 0;

    if (m_grid) {
        for (const auto& od : m_grid->getAllOptions()) {
            if (!od) continue;
            double pos = od->getPosition();
            if (pos == 0) continue;

            auto ed = od->getExpiryData();
            if (!ed) continue;

            // Check if option expired today
            int32_t dte = ed->daysToExpiry();
            if (dte <= 0) {
                // Settlement: intrinsic value
                double intrinsic = 0;
                if (od->getRight() == OR_Call) {
                    intrinsic = std::max(0.0, spotPrice - od->getStrike());
                } else {
                    intrinsic = std::max(0.0, od->getStrike() - spotPrice);
                }
                settlementPnl += -pos * intrinsic * m_multiplier;
            } else {
                // Close value: mark to mid
                double mid = od->getMid();
                if (mid > 0) {
                    closePnl += -pos * mid * m_multiplier;
                }
            }
        }
    }

    result.tradingPnl = tradingPnl;
    result.settlementPnl = settlementPnl;
    result.closePnl = closePnl;
    result.totalPnl = tradingPnl + settlementPnl + closePnl;
    result.dayPnl = result.totalPnl - m_lastTotalPnl;
    m_lastTotalPnl = result.totalPnl;

    m_dayResults.push_back(result);
    return result;
}

void ExpirationSimulator::printSummary() const
{
    WTSLogger::log_by_cat("strategy", LL_INFO,
        "ExpirationSimulator: {} day results, total PnL={:.2f}",
        m_dayResults.size(), m_dayResults.empty() ? 0.0 : m_dayResults.back().totalPnl);

    for (const auto& dr : m_dayResults) {
        WTSLogger::log_by_cat("strategy", LL_INFO,
            "  Date={} dayPnl={:.2f} totalPnl={:.2f} "
            "(trading={:.2f} settle={:.2f} close={:.2f})",
            dr.date, dr.dayPnl, dr.totalPnl,
            dr.tradingPnl, dr.settlementPnl, dr.closePnl);
    }
}

} // namespace wt_option
