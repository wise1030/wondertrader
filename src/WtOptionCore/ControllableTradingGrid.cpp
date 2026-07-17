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
#include "Scanners/IScanModule.h"
#include "CompositeOptionPricer.h"
#include "../Share/fmtlib.h"

#include <cmath>
#include <algorithm>
#include <chrono>

namespace wt_option {

namespace {

// Small epsilon guard for spread-tightness division (matches original FP_EPSILON usage)
static const double FP_EPSILON = 1e-10;

// isBest — is our quote at-or-better than the market best (with 1-tick relaxation)?
// side: 0 = BID, 1 = ASK. Mirrors the original longbeach isBest():
//   - our side empty            → false
//   - market side empty         → true (we are best by default)
//   - BID: our_px >= mkt_px - 1 tick (fade market bid down 1 tick)
//   - ASK: our_px <= mkt_px + 1 tick (fade market ask up 1 tick)
bool isBestSide(int side, const PriceSize& our, double mktPx, bool mktHas, double tick) {
    if (our.empty()) return false;
    if (!mktHas)     return true;
    if (tick <= 0)   tick = 0; // no relaxation if tick unknown
    if (side == 0)   return our.px() >= (mktPx - tick);   // BID
    else             return our.px() <= (mktPx + tick);   // ASK
}

} // anonymous namespace

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
void ControllableTradingGrid::onScannerHit(const ScannerHitEvent& event) {
    if (!event.option || !m_ctx->enabled) return;
    WTSLogger::log_by_cat("strategy", LL_INFO,
        "CTG::onScannerHit {} signal={:.4f} reason={}",
        event.option->getCode(), event.signal, event.reason);

    // Trigger scanner order execution if callback is set
    if (m_scannerExec && event.signal > 0) {
        m_scannerExec(event.option->getCode(), event.signal, event.option);
    }

    // Mark for priority update in next refresh cycle
    m_optUpdateSet.insert(event.option->getCode());
}

// Phase 8: tradingStopMidDay — pause trading mid-day
void ControllableTradingGrid::tradingStopMidDay() {
    if (m_ctx) {
        m_ctx->enabled = false;
        WTSLogger::log_by_cat("strategy", LL_WARN,
            "CTG::tradingStopMidDay - trading disabled");
        refresh();
        // CRITICAL: drainPendingQuotes unconditionally to actually send CANCEL
        // orders to the exchange. Without this, CANCEL quotes are staged in
        // m_pendingQuotes but never sent (all callers gate drain on enabled==true).
        drainPendingQuotes();
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

    // B2: If mode unchanged, skip (original behavior)
    if (otd->getQuoteMode() == *qmode) return false;

    otd->setQuoteMode(*qmode);

    // B2: Clear ourMarket values so stale quotes don't persist (original clears
    // both slot 0 and slot 2).
    auto od = m_grid->get(code);
    if (od) {
        od->values(0).ourMarket().clear();
        od->values(2).ourMarket().clear();
    }

    // B2: Trigger computeValues to reprice with the new quote mode.
    // Reset the pricer's time guard so computeValues doesn't no-op.
    if (m_grid) {
        auto pricer = m_grid->getOptionPricer();
        if (pricer) {
            auto cop = std::dynamic_pointer_cast<CompositeOptionPricer>(pricer);
            if (cop) cop->resetLastComputeTime();
            m_grid->computeValues(pricer.get());
        }
    }

    WTSLogger::log_by_cat("strategy", LL_INFO,
        "CTG::onSetQMode {} -> {}", code, modeStr);
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
    // B15: Track expiry readiness - forward and fit readiness for each expiry.
    // When readiness changes, log for monitoring. The pricer and CurveFitter
    // set these flags on ExpiryData during computeValues / doFit.
    if (!ed) return;
    uint32_t exp = ed->getExpiry();
    bool wasReady = m_expiryReady.count(exp) ? m_expiryReady[exp] : false;
    bool isReady = ed->isValuesReady();
    m_expiryReady[exp] = isReady;
    if (isReady && !wasReady) {
        WTSLogger::log_by_cat("strategy", LL_INFO,
            "CTG: Expiry {} values ready (fwd={} fit={})",
            exp, ed->isForwardReady(), ed->isFitReady());
    }
}

// ============================================================================
// refresh - main loop: collect desired markets -> updateOrders
// ============================================================================
void ControllableTradingGrid::refresh() {
    if (!m_grid) return;

    // B1: When disabled, clear ALL desired markets (option + future) so
    // existing quotes get cancelled. Matches original behavior.
    if (!m_ctx->enabled) {
        for (const auto& od : m_grid->getAllOptions()) {
            if (!od) continue;
            auto otd = od->getTradingData();
            if (otd) otd->multiMarket().clear();
        }
        if (m_otg) {
            for (const auto& pair : m_otg->getAllUnderlyingTradingData()) {
                if (pair.second) pair.second->multiMarket().clear();
            }
        }
        // Still collect pending quotes so the drain can cancel existing orders
    }

    // B16: Preserve dropped quotes from previous cycle (TPS-limited).
    // Previously, m_pendingQuotes.clear() destroyed them. Now we save them
    // and prepend to the newly-collected quotes so they get retried.
    std::vector<PendingQuote> retainedDrops;
    m_pendingQuotes.swap(retainedDrops);  // clear m_pendingQuotes, keep drops

    const bool panicClear = m_ctx->panicked;

    for (const auto& od : m_grid->getAllOptions()) {
        if (!od) continue;

        OptionTradingDataPtr otd = od->getTradingData();
        if (otd) {
            MultiMarket& our_mkt = otd->multiMarket();
            our_mkt.clear();
            if (!panicClear && otd->isActive() && od->values(0).isPriced()) {
                our_mkt = od->values(0).ourMarket();
                // B13: Multi-source market merge - if a secondary source
                // (values slot 2) is also priced, merge its ourMarket by
                // taking the better bid/ask (original take_inner slot 2).
                if (od->values(2).isPriced()) {
                    const MultiMarket& src2 = od->values(2).ourMarket();
                    if (!src2.getBestBid().empty()) {
                        if (our_mkt.getBestBid().empty() ||
                            src2.getBestBid().px() > our_mkt.getBestBid().px())
                            our_mkt.setBid(src2.getBestBid());
                    }
                    if (!src2.getBestAsk().empty()) {
                        if (our_mkt.getBestAsk().empty() ||
                            src2.getBestAsk().px() < our_mkt.getBestAsk().px())
                            our_mkt.setAsk(src2.getBestAsk());
                    }
                }
            }
        }

        const MultiMarket* desired = nullptr;
        const MultiMarket* current = nullptr;
        if (otd) {
            desired = &otd->multiMarket();
            current = &otd->getCurrentMarket();
        } else {
            desired = &od->values(0).ourMarket();
            current = &od->currentMarket();
        }

        UPDATE_TYPE utype = check_markets(*desired, *current);
        if (utype == UT_NONE) continue;

        double bidP = desired->getBestBid().px();
        int32_t bidQ = desired->getBestBid().sz();
        double askP = desired->getBestAsk().px();
        int32_t askQ = desired->getBestAsk().sz();

        PendingQuote pq;
        pq.code = od->getCode();
        pq.utype = utype;

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

        pq.rank = rankOption(otd, utype);
        m_pendingQuotes.push_back(std::move(pq));
    }

    // Collect future/underlying pending quotes
    if (m_otg) {
        for (const auto& pair : m_otg->getAllUnderlyingTradingData()) {
            auto utd = pair.second;
            if (!utd || !utd->isActive()) continue;

            MultiMarket& our_mkt = utd->multiMarket();
            our_mkt = utd->ourMarket();

            const MultiMarket& desired = utd->multiMarket();
            const MultiMarket& current = utd->getCurrentMarket();

            UPDATE_TYPE utype = check_markets(desired, current);
            if (utype == UT_NONE) continue;

            double bidP = desired.getBestBid().px();
            int32_t bidQ = desired.getBestBid().sz();
            double askP = desired.getBestAsk().px();
            int32_t askQ = desired.getBestAsk().sz();

            PendingQuote pq;
            pq.code = utd->getCode();
            pq.utype = utype;
            pq.isFuture = true;

            if (bidP > 0 && askP > 0) {
                pq.bidP = bidP;
                pq.bidQ = static_cast<uint32_t>(bidQ);
                pq.askP = askP;
                pq.askQ = static_cast<uint32_t>(askQ);
            } else if (bidP > 0) {
                pq.bidP = bidP;
                pq.bidQ = static_cast<uint32_t>(bidQ);
            } else if (askP > 0) {
                pq.askP = askP;
                pq.askQ = static_cast<uint32_t>(askQ);
            } else {
                pq.isCancel = true;
            }

            pq.rank = rankFuture(utd, utype);
            m_pendingQuotes.push_back(std::move(pq));
        }
    }

    // Sort by rank descending: highest priority first across options + futures
    std::sort(m_pendingQuotes.begin(), m_pendingQuotes.end(),
        [](const PendingQuote& a, const PendingQuote& b) { return a.rank > b.rank; });

    // B16: Prepend retained dropped quotes (from previous TPS-limited cycle)
    // so they get retried with high priority before new quotes.
    if (!retainedDrops.empty()) {
        m_pendingQuotes.insert(m_pendingQuotes.begin(),
            std::make_move_iterator(retainedDrops.begin()),
            std::make_move_iterator(retainedDrops.end()));
    }

    double now = m_ctx->getTime();
    if (now - m_lastTransactionUpdateTime >= 1.0) {
        m_lastTransactionUpdateTime = now;
        m_txCount = 0;
    }
}

// ============================================================================
// rankOption — priority ranking for order updates
// Faithful port of the original longbeach rankOption (7 factors + type weight).
// Crossing check uses the LIVE MARKET mid (not the theoretical value).
// Type weight: CANCEL=+1000, UPDATE=+500, NEW=499-rank (NEW deprioritised).
// ============================================================================
int32_t ControllableTradingGrid::rankOption(
    const std::shared_ptr<OptionTradingData>& otd, UPDATE_TYPE utype) {
    if (!otd) return 0;

    auto od = otd->getOptionData();
    if (!od) return 0;

    int32_t rank = 0;

    // Live market best bid/ask and mid (from the exchange book, not our theo)
    const OptionMarket& mkt = od->getMarket();
    bool mktHasBid = mkt.bid > 0;
    bool mktHasAsk = mkt.ask > 0;
    double midpx = od->getMid();

    const MultiMarket& our_mkt = otd->multiMarket();
    double delta = od->values(0).greeks().delta();
    double tick = od->getTickSize();

    // Factor 1: our bid crosses market mid
    if (our_mkt.hasBids() && midpx > 0 && (midpx < our_mkt.getBestBid().px()))
        rank += 10;
    // Factor 2: our ask crosses market mid
    if (our_mkt.hasAsks() && midpx > 0 && (midpx > our_mkt.getBestAsk().px()))
        rank += 10;

    // Factor 3: our quote is best in the market (1-tick relaxation)
    bool best_bid = isBestSide(0, our_mkt.getBestBid(), mkt.bid, mktHasBid, tick);
    bool best_ask = isBestSide(1, our_mkt.getBestAsk(), mkt.ask, mktHasAsk, tick);
    if (best_bid || best_ask)
        rank += 5;
    // Factor 4: our quote present but NOT best
    if ((our_mkt.hasBids() && !best_bid) || (our_mkt.hasAsks() && !best_ask))
        rank += 1;

    // Factor 5: delta-based urgency (+1 per 0.1 delta, +1 extra per 0.1 above 0.5)
    rank += static_cast<int32_t>(std::ceil(10.0 * std::fabs(delta)));
    rank += static_cast<int32_t>(std::ceil(10.0 * std::max(std::fabs(delta) - 0.5, 0.0)));

    // Factor 6: spread tightness relative to theoretical value
    double theo = od->values(0).theo();
    double our_bid_spread = 1e6;
    double our_ask_spread = 1e6;
    if (our_mkt.hasBids())
        our_bid_spread = theo - our_mkt.getBestBid().px();
    if (our_mkt.hasAsks())
        our_ask_spread = our_mkt.getBestAsk().px() - theo;
    if (our_mkt.hasBids() || our_mkt.hasAsks()) {
        double our_spread = std::min(our_bid_spread, our_ask_spread);
        rank += static_cast<int32_t>(std::fabs(delta) / std::max(FP_EPSILON, our_spread));
    }

    // Factor 7: near-expiry urgency
    auto ed = od->getExpiryData();
    if (ed && ed->daysToExpiry() < 30 && ed->daysToExpiry() >= 0)
        rank += 5;

    // Type weight — original scheme:
    //   CANCEL → +1000 (highest), UPDATE → +500, NEW → 499-rank (deprioritised)
    switch (utype) {
        case UT_CANCEL:  rank += 1000; break;
        case UT_UPDATE:  rank += 500;  break;
        case UT_NEW:     rank = 499 - rank; break;
        case UT_NONE:    return 0;
        default:         break;
    }

    return rank;
}

// ============================================================================
// rankFuture — priority ranking for underlying/future updates
// Faithful port of the original longbeach rankFuture (all 7 factors).
// ============================================================================
int32_t ControllableTradingGrid::rankFuture(
    const std::shared_ptr<UnderlyingTradingData>& utd, UPDATE_TYPE utype) {
    if (!utd) return 0;

    int32_t rank = 0;

    const MultiMarket& our_mkt = utd->multiMarket();
    double midpx = utd->getMid();
    double bid = utd->getBid();
    double ask = utd->getAsk();
    bool mktHasBid = bid > 0;
    bool mktHasAsk = ask > 0;
    double tick = utd->getTickSize();

    // Factor 1: our bid crosses market mid
    if (our_mkt.hasBids() && midpx > 0 && (midpx < our_mkt.getBestBid().px()))
        rank += 10;
    // Factor 2: our ask crosses market mid
    if (our_mkt.hasAsks() && midpx > 0 && (midpx > our_mkt.getBestAsk().px()))
        rank += 10;

    // Factor 3: our quote is best in the market (1-tick relaxation)
    bool best_bid = isBestSide(0, our_mkt.getBestBid(), bid, mktHasBid, tick);
    bool best_ask = isBestSide(1, our_mkt.getBestAsk(), ask, mktHasAsk, tick);
    if (best_bid || best_ask)
        rank += 5;
    // Factor 4: our quote present but NOT best
    if ((our_mkt.hasBids() && !best_bid) || (our_mkt.hasAsks() && !best_ask))
        rank += 1;

    // Factor 5: futures carry one delta equivalent
    rank += 15;

    // Factor 6: spread tightness (delta=1 for futures) relative to theo mid
    double delta = 1.0;
    double theoMid = utd->values(0).theoPrices().mid;
    double our_bid_spread = 1e6;
    double our_ask_spread = 1e6;
    if (our_mkt.hasBids())
        our_bid_spread = theoMid - our_mkt.getBestBid().px();
    if (our_mkt.hasAsks())
        our_ask_spread = our_mkt.getBestAsk().px() - theoMid;
    if (our_mkt.hasBids() || our_mkt.hasAsks()) {
        double our_spread = std::min(our_bid_spread, our_ask_spread);
        rank += static_cast<int32_t>(std::fabs(delta) / std::max(FP_EPSILON, our_spread));
    }

    // Factor 7: make all futures important
    rank += 5;

    // Type weight — original scheme (same as rankOption)
    switch (utype) {
        case UT_CANCEL:  rank += 1000; break;
        case UT_UPDATE:  rank += 500;  break;
        case UT_NEW:     rank = 499 - rank; break;
        case UT_NONE:    return 0;
        default:         break;
    }

    return rank;
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

    // TPS limit - panic mode gets enhanced TPS for faster risk reduction
    int32_t txnLimit = m_maxTransactionsPerSec;
    if (m_ctx->panicked) txnLimit += m_maxPanicTPS;

    // Panic mode no longer cancels everything immediately.  Instead, the
    // refresh() above has already cleared all option desired markets
    // (producing UT_CANCEL quotes) while keeping future markets for hedging.
    // The normal drain loop processes these cancels with the enhanced TPS.

    bool cancelOnly = false;
    for (const auto& pq : m_pendingQuotes) {
        if (m_txCount >= txnLimit) {
            cancelOnly = true;
        }

        // When over the TPS limit, a pure NEW order carries no existing order to
        // cancel, so processing it in cancel-only mode would be a wasted pass and
        // could still leak transactions. Skip NEW quotes entirely and count the drop.
        if (cancelOnly && pq.utype == UT_NEW && !pq.isCancel) {
            ++m_txDrop;
            m_droppedQuotes.push_back(pq);  // B16: Save for retry
            continue;
        }

        if (pq.isFuture) {
            if (!m_otg) continue;
            auto utd = m_otg->getUnderlyingTradingData(pq.code);
            if (!utd || !utd->getQuoteManager()) continue;
            // In panic mode, futures still process normally (for hedging).
            int32_t txns = utd->updateOrders(pq.isCancel || cancelOnly);
            m_txCount += txns;
        } else {
            auto od = m_grid->get(pq.code);
            if (!od) continue;
            auto otd = od->getTradingData();
            if (!otd || !otd->getQuoteManager()) continue;
            int32_t txns = otd->updateOrders(pq.isCancel || cancelOnly);
            m_txCount += txns;
        }
    }

    m_pendingQuotes.clear();

    // B16: Log drop count and retain dropped quotes for next cycle retry.
    // Dropped quotes are saved in m_droppedQuotes during the loop above,
    // then moved back to m_pendingQuotes for the next refresh() cycle.
    // refresh() now preserves them via swap + prepend.
    if (m_txDrop > 0) {
        WTSLogger::log_by_cat("strategy", LL_WARN,
            "CTG: {} quotes dropped (TPS limit {} reached, txCount={})",
            m_txDrop, txnLimit, m_txCount);
        // Move dropped quotes back to pending for next cycle
        for (auto& pq : m_droppedQuotes) {
            m_pendingQuotes.push_back(std::move(pq));
        }
        m_droppedQuotes.clear();
    }
}

} // namespace wt_option
