/*!
 * \file ControllableTradingGrid.cpp
 * \brief Execution scheduling: refresh → rank → TPS limit → updateOrders
 */
// Standard headers FIRST to avoid WT namespace pollution
#include <cstring>
#include <cmath>
#include <algorithm>
#include <chrono>

// WT logger before any wt_option headers (WTSLogger pulls spdlog which needs clean namespace)
#include "../WTSTools/WTSLogger.h"

#include "ControllableTradingGrid.h"
#include "OptionGrid.h"
#include "OptionTradingGrid.h"
#include "OptionTradingData.h"
#include "OptionData.h"
#include "OptionValues.h"
#include "OptionTradingData.h"
#include "UnderlyingTradingData.h"
#include "ExpiryData.h"
#include "StrikeData.h"
#include "IScanModule.h"
#include "../Share/fmtlib.h"

#include <cmath>
#include <algorithm>
#include <chrono>

namespace wt_option {

// ============================================================================
// Constructor / Destructor
// ============================================================================
ControllableTradingGrid::ControllableTradingGrid(OptionGridPtr grid, OptionTraderContextPtr ctx)
    : m_grid(grid)
    , m_ctx(ctx)
{
    if (m_grid)
        m_grid->addListener(this);
}

ControllableTradingGrid::~ControllableTradingGrid() {
    if (m_grid)
        m_grid->removeListener(this);
}

// ============================================================================
// Scanner management
// ============================================================================
void ControllableTradingGrid::addScanner(std::shared_ptr<IScanModule> scanner) {
    m_scanners.push_back(scanner);
}

void ControllableTradingGrid::removeScanner(std::shared_ptr<IScanModule> scanner) {
    m_scanners.erase(std::remove(m_scanners.begin(), m_scanners.end(), scanner), m_scanners.end());
}

// ============================================================================
// onOptionHit — scanner signal callback
// ============================================================================
void ControllableTradingGrid::onOptionHit(OptionData* od, int32_t index) {
    if (!od || !m_ctx->enabled) return;
    WTSLogger::log_by_cat("strategy", LL_INFO,
        "CTG::onOptionHit {} index={}", od->getCode(), index);
    // Phase 8: Mark for priority update in next refresh cycle
    m_optUpdateSet.insert(od->getCode());
}

// Phase 8: tradingStopMidDay — pause trading mid-day
void ControllableTradingGrid::tradingStopMidDay() {
    if (m_ctx) {
        m_ctx->enabled = false;
        WTSLogger::log_by_cat("strategy", LL_WARN,
            "CTG::tradingStopMidDay — trading disabled");
        refresh();
    }
}

// Phase 8: resumeTrading — resume after mid-day pause
void ControllableTradingGrid::resumeTrading() {
    if (m_ctx) {
        m_ctx->enabled = true;
        WTSLogger::log_by_cat("strategy", LL_INFO,
            "CTG::resumeTrading — trading enabled");
    }
}

// Phase 8: onSetQMode — runtime QuoteMode switch for a specific option
bool ControllableTradingGrid::onSetQMode(const std::string& code, const std::string& modeStr) {
    if (!m_otg) return false;
    auto otd = m_otg->getTradingData(code);
    if (!otd) return false;
    auto qmode = OptionTradingData::str2qmode(modeStr);
    if (!qmode) return false;
    otd->setQuoteMode(*qmode);
    WTSLogger::log_by_cat("strategy", LL_INFO,
        "CTG::onSetQMode {} → {}", code, modeStr);
    return true;
}

// ============================================================================
// onComputeValuesCompleted — triggered by grid after pricer runs
// ============================================================================
void ControllableTradingGrid::onComputeValuesCompleted(const IOptionGrid* grid) {
    refresh();
}

void ControllableTradingGrid::onAddOption(const OptionDataPtr& od) {
    // Track new option for update
    if (od) m_optUpdateSet.insert(od->getCode());
}

void ControllableTradingGrid::onAddExpiry(const ExpiryDataPtr& ed) {
    // Nothing specific needed for new expiry in WT version
}

// ============================================================================
// refresh — main loop: collect desired markets → updateOrders
// ============================================================================
void ControllableTradingGrid::refresh() {
    if (!m_grid || !m_ctx->enabled) return;

    // Clear previous pending quotes
    m_pendingQuotes.clear();

    // Phase 4: go through OTG → OTD (instead of directly reading OptionValues)
    for (const auto& od : m_grid->getAllOptions()) {
        if (!od) continue;

        // combineMarkets: merge pricer desired → OTD multiMarket
        OptionTradingDataPtr otd = od->getTradingData();
        if (otd) {
            MultiMarket& our_mkt = otd->multiMarket();
            our_mkt.clear();
            if (otd->isActive() && od->values(0).isPriced())
                our_mkt = od->values(0).ourMarket();
        }

        // Determine desired/current for check_markets
        const MultiMarket* desired = nullptr;
        const MultiMarket* current = nullptr;
        if (otd) {
            desired = &otd->multiMarket();
            current = &otd->getCurrentMarket();
        } else {
            desired = &od->values(0).ourMarket();
            current = &od->currentMarket();
        }

        // check_markets: compare desired vs current, skip if unchanged
        UPDATE_TYPE utype = check_markets(*desired, *current);
        if (utype == UT_NONE) continue;

        double bidP = desired->getBestBid().px();
        int32_t bidQ = desired->getBestBid().sz();
        double askP = desired->getBestAsk().px();
        int32_t askQ = desired->getBestAsk().sz();

        PendingQuote pq;
        pq.code = od->getCode();

        if (bidP > 0 && askP > 0) {
            pq.bidP = bidP;
            pq.bidQ = static_cast<uint32_t>(bidQ);
            pq.askP = askP;
            pq.askQ = static_cast<uint32_t>(askQ);
        } else if (bidP > 0) {
            pq.bidP = bidP;
            pq.bidQ = static_cast<uint32_t>(bidQ);
            pq.askP = 0;
        } else if (askP > 0) {
            pq.askP = askP;
            pq.askQ = static_cast<uint32_t>(askQ);
            pq.bidP = 0;
        } else {
            pq.isCancel = true;
        }

        m_pendingQuotes.push_back(std::move(pq));
    }

    // TPS reset
    double now = m_ctx->getTime();
    if (now - m_lastTransactionUpdateTime >= 1.0) {
        m_lastTransactionUpdateTime = now;
        m_txCount = 0;
    }

    static int refreshDebug = 0;
    if (++refreshDebug <= 3) {
        WTSLogger::log_by_cat("strategy", LL_INFO,
            "CTG::refresh pendingQuotes={} options={}",
            m_pendingQuotes.size(), m_grid->numOptions());
    }
}

// ============================================================================
// updateOrders — TPS-limited order execution with rank-based priority
// ============================================================================
int32_t ControllableTradingGrid::updateOrders(
    const std::vector<std::shared_ptr<OptionTradingData>>& optList,
    const std::vector<std::shared_ptr<UnderlyingTradingData>>& udlList) {

    if (!m_ctx->enabled) {
        // Clear all markets when disabled
        return 0;
    }

    // Panic mode: clear all
    if (m_ctx->panicked) {
        return 0;
    }

    // TPS limit
    int32_t txnLimit = m_maxTransactionsPerSec;
    if (m_ctx->panicked) {
        txnLimit += m_maxPanicTPS;
    }

    double now = m_ctx->getTime();
    if (now - m_lastTransactionUpdateTime >= 1.0) {
        m_lastTransactionUpdateTime = now;
        m_txCount = 0;
    }

    // Build sorted update list with ranks
    struct RankedUpdate {
        std::string code;
        int32_t rank;
        UPDATE_TYPE utype;
        double bidP, askP;
        int32_t bidQ, askQ;
    };
    std::vector<RankedUpdate> sortedList;

    // Rank options
    for (const auto& otd : optList) {
        if (!otd) continue;
        // check_markets would compare desired vs current
        // Simplified: assume UPDATE needed if desired market is non-empty
        UPDATE_TYPE utype = UT_UPDATE; // Placeholder
        if (utype != UT_NONE) {
            RankedUpdate ru;
            ru.code = otd->getCode();
            ru.rank = rankOption(otd, utype);
            ru.utype = utype;
            sortedList.push_back(ru);
        }
    }

    // Sort by rank (highest first)
    std::sort(sortedList.begin(), sortedList.end(),
        [](const RankedUpdate& a, const RankedUpdate& b) { return a.rank > b.rank; });

    // Execute with TPS limit
    int32_t totalTx = 0;
    bool cancelOnly = false;

    for (const auto& ru : sortedList) {
        if (m_txCount >= txnLimit)
            cancelOnly = true;

        int32_t txCnt = 0;
        if (cancelOnly) {
            if (m_cancelExec)
                txCnt = m_cancelExec(ru.code);
        } else {
            if (m_quoteExec && ru.bidP > 0 && ru.askP > 0) {
                txCnt = m_quoteExec(ru.code, ru.bidP, static_cast<uint32_t>(ru.bidQ),
                                     ru.askP, static_cast<uint32_t>(ru.askQ));
            } else if (m_cancelExec) {
                txCnt = m_cancelExec(ru.code);
            }
        }

        m_txCount += txCnt;
        totalTx += txCnt;
    }

    return totalTx;
}

// ============================================================================
// rankOption — priority ranking for order updates
// ============================================================================
int32_t ControllableTradingGrid::rankOption(
    const std::shared_ptr<OptionTradingData>& otd, UPDATE_TYPE utype) {
    if (!otd) return 0;

    int32_t rank = 0;

    // Factor 1: Update type priority
    switch (utype) {
        case UT_CANCEL:  rank += 1000; break;
        case UT_NEW:     rank += 100;  break;
        case UT_UPDATE:  rank += 10;   break;
        case UT_NONE:    return 0;
    }

    // Factor 2: Crossing mid (our bid > mid or our ask < mid)
    const MultiMarket& our_mkt = otd->getLastDesiredMarket();
    auto od = otd->getOptionData();
    if (!od) return rank;
    double midpx = od->values(0).theo();
    if (!our_mkt.getBestBid().empty() && midpx < our_mkt.getBestBid().px())
        rank += 10;
    if (!our_mkt.getBestAsk().empty() && midpx > our_mkt.getBestAsk().px())
        rank += 10;

    // Factor 3+4: Delta-based ranking
    double delta = std::fabs(od->values(0).greeks().delta());
    rank += static_cast<int32_t>(std::ceil(10.0 * delta));
    rank += static_cast<int32_t>(std::ceil(10.0 * std::max(delta - 0.5, 0.0)));

    // Factor 5: Spread tightness (delta/our_spread)
    double our_bid_spread = 1e6;
    double our_ask_spread = 1e6;
    if (!our_mkt.getBestBid().empty())
        our_bid_spread = midpx - our_mkt.getBestBid().px();
    if (!our_mkt.getBestAsk().empty())
        our_ask_spread = our_mkt.getBestAsk().px() - midpx;
    if (!our_mkt.getBestBid().empty() || !our_mkt.getBestAsk().empty()) {
        double our_spread = std::min(our_bid_spread, our_ask_spread);
        if (our_spread > 1e-6)
            rank += static_cast<int32_t>(delta / our_spread);
    }

    // Factor 6: Days to expiry < 30 → +5
    auto ed = od->getExpiryData();
    if (ed && ed->daysToExpiry() < 30 && ed->daysToExpiry() >= 0)
        rank += 5;

    return rank;
}

// ============================================================================
// rankFuture — priority ranking for underlying/future updates
// ============================================================================
int32_t ControllableTradingGrid::rankFuture(
    const std::shared_ptr<UnderlyingTradingData>& utd, UPDATE_TYPE utype) {
    if (!utd) return 0;

    int32_t rank = 0;
    switch (utype) {
        case UT_CANCEL:  rank += 1000; break;
        case UT_NEW:     rank += 100;  break;
        case UT_UPDATE:  rank += 50;   break; // Futures get higher priority than options
        case UT_NONE:    return 0;
    }
    return rank;
}

// ============================================================================
// combineMarkets — merge desired market from pricer into trading data
// ============================================================================
void ControllableTradingGrid::combineMarkets(
    const OptionData& od,
    std::vector<std::shared_ptr<OptionTradingData>>& outList) {
    // In original, this merges pricer's ourMarket into OptionTradingData's desired market
    // In WT version, the pricer writes directly to OptionValues::ourMarket()
    // So combineMarkets is a no-op — the data is already there
    // This method exists for interface compatibility
}

void ControllableTradingGrid::onRefresh() {
    refresh();
}

// ============================================================================
// drainPendingQuotes — async quote submission with TPS limit
// Called from worker thread after refresh() has collected pending quotes.
// ============================================================================
void ControllableTradingGrid::drainPendingQuotes() {
    if (m_pendingQuotes.empty()) return;

    // TPS limit
    int32_t txnLimit = m_maxTransactionsPerSec;
    if (m_ctx->panicked) txnLimit += m_maxPanicTPS;

    // Panic mode: cancel all instead of quoting
    if (m_ctx->panicked) {
        for (const auto& pq : m_pendingQuotes) {
            if (m_cancelExec) m_cancelExec(pq.code);
        }
        m_pendingQuotes.clear();
        return;
    }

    // Sort by rank (cancels first, then highest rank)
    // TODO: use rankOption/rankFuture when full wiring is done

    bool cancelOnly = false;
    for (const auto& pq : m_pendingQuotes) {
        if (m_txCount >= txnLimit) {
            cancelOnly = true;
        }

        if (pq.isCancel || cancelOnly) {
            if (m_cancelExec) {
                m_cancelExec(pq.code);
                m_txCount++;
            }
            // Update OTD currentMarket (if OQM exists, it tracks via callbacks)
            auto od = m_grid->get(pq.code);
            if (od) {
                auto otd = od->getTradingData();
                if (otd && !otd->getQuoteManager()) {
                    // No OQM → update currentMarket directly
                    // (OQM path: currentMarket tracked by onOrderStatusChange callbacks)
                    // For cancel, clear current
                }
                if (otd) {
                    // Clear OTD current (will be rebuilt from OQM callbacks or set here)
                    MultiMarket empty;
                    otd->setCurrentMarket(empty);
                }
                od->setCurrentMarket(MultiMarket());
            }
        } else if (pq.bidP > 0 && pq.askP > 0) {
            if (m_quoteExec) {
                m_quoteExec(pq.code, pq.bidP, pq.bidQ, pq.askP, pq.askQ);
                m_txCount++;
            }
            // Update currentMarket
            auto od = m_grid->get(pq.code);
            if (od) {
                MultiMarket mkt;
                mkt.setBest(0, PriceSize(pq.bidP, pq.bidQ));
                mkt.setBest(1, PriceSize(pq.askP, pq.askQ));
                auto otd = od->getTradingData();
                if (otd && !otd->getQuoteManager()) {
                    // No OQM → update OTD currentMarket directly
                    otd->setCurrentMarket(mkt);
                }
                // Also update OptionData for backward compat
                od->setCurrentMarket(mkt);
            }
        } else if (pq.bidP > 0) {
            if (m_orderExec) {
                m_orderExec(pq.code, true, pq.bidP, pq.bidQ);
                m_txCount++;
            }
        } else if (pq.askP > 0) {
            if (m_orderExec) {
                m_orderExec(pq.code, false, pq.askP, pq.askQ);
                m_txCount++;
            }
        }
    }

    m_pendingQuotes.clear();
}

} // namespace wt_option
