/*!
 * \file StrikeFinders.cpp
 * \brief Concrete strike finders (quantbox optioncore absorption)
 */
#include "CffexIOStrikeFinder.h"
#include "DceMOStrikeFinder.h"
#include "WTSLogger.h"

#include <stdexcept>

namespace wt_option {

// === CffexIOStrikeFinder ====================================================
uint32_t CffexIOStrikeFinder::getStrikeInterval() const
{
    switch (m_type)
    {
    case MONTHLY:   return 50;
    case QUARTERLY: return 100;
    default:
        // Original threw for WEEKLY (no weekly IO series at migration time);
        // we degrade to the monthly grid instead of crashing a worker thread.
        WTSLogger::log_by_cat("strategy", LL_WARN,
            "CffexIOStrikeFinder: WEEKLY unsupported, falling back to 50-point grid");
        return 50;
    }
}

double CffexIOStrikeFinder::getStrike(double price) const
{
    return roundToInterval(price, getStrikeInterval());
}

// === DceMOStrikeFinder ======================================================
uint32_t DceMOStrikeFinder::getStrikeInterval(double price) const
{
    switch (m_type)
    {
    case MONTHLY:
    case QUARTERLY:
        if (price < 2000)      return 25;
        else if (price < 5000) return 50;
        else                   return 100;
    default:
        WTSLogger::log_by_cat("strategy", LL_WARN,
            "DceMOStrikeFinder: WEEKLY unsupported, falling back to tiered grid");
        return (price < 2000) ? 25 : (price < 5000 ? 50 : 100);
    }
}

double DceMOStrikeFinder::getStrike(double price) const
{
    return roundToInterval(price, getStrikeInterval(price));
}

} // namespace wt_option
