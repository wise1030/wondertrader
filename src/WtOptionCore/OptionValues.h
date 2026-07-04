/*!
 * \file OptionValues.h
 * \brief Per-option value container (migrated from quantbox, no longbeach dependency)
 *
 * Original: longbeach::optioncore::OptionValues depended on longbeach::MultiMarket
 * (a multi-level order book class), longbeach::math::EMAFilter, and OptionGreeks.
 *
 * Migration notes:
 *  - MultiMarket is replaced with a lightweight wt_option::MultiMarket that
 *    preserves the public surface used by OptionValues (clear/getMidPrice/
 *    getAsks/getBids/empty/set). Callers that only touched those methods are
 *    source-compatible. Full multi-level order-book semantics (MktLevels,
 *    ExactMult, MarketCmp) are NOT preserved — those are book-construction
 *    concerns that belong in a separate migrated Book class, not in this
 *    pure-math header. The struct here is intentionally minimal.
 *  - EMAFilter is replaced with a self-contained wt_option::EMAFilter that
 *    reproduces the exponential-decay mean of the original (alpha = ln2/hl),
 *    taking seconds as the window unit and timestamps as double seconds.
 *  - boost::shared_ptr -> std::shared_ptr (none used in this header).
 */
#pragma once

#include "optioncoretypes.h"
#include "OptionGreeks.h"

#include <cmath>
#include <cstring>    // memset
#include <string>
#include <vector>
#include <limits>

namespace wt_option {

// ---------------------------------------------------------------------------
// Lightweight EMA filter (replaces longbeach::math::EMAFilter)
// ---------------------------------------------------------------------------
// Window is specified in seconds (half-life). Timestamps are POSIX seconds
// (double). Reproduces the original's exponential-decay running mean.
class EMAFilter
{
public:
    EMAFilter() { reset(); }
    explicit EMAFilter(double windowSeconds) {
        reset();
        setWindow(windowSeconds);
    }

    double getSum()      const { return m_curSum; }
    double getMean()     const { return m_curMean; }
    double mean_raw()    const { return m_mean_raw; }
    bool   isOK()        const { return m_isOK; }

    void setWindow(double windowSeconds)
    {
        double hl = (windowSeconds > 0.01) ? windowSeconds : 0.01;
        m_alpha = std::log(2.0) / hl;
        reset();
    }

    void reset()
    {
        m_curSum = 0.0;
        m_weightSum = 0.0;
        m_curMean = 0.0;
        m_mean_raw = 0.0;
        m_count = 0;
        m_hasTv = false;
        m_curTv = 0.0;
        m_isOK = false;
    }

    // tv = timestamp in seconds, v = value, sz = weight (default 1.0)
    void update(double tv, double v, double sz = 1.0)
    {
        if (std::isnan(v)) return;

        if (!m_hasTv) {
            m_curSum = v * sz;
            m_weightSum = sz;
            m_curMean = v;
        } else {
            double secs = tv - m_curTv;
            if (secs < 0.0) return; // ignore late updates
            double decay = std::exp(-m_alpha * secs);
            m_curSum = v * sz + decay * m_curSum;
            m_weightSum = sz + decay * m_weightSum;
            m_curMean = m_curSum / m_weightSum;
        }
        if (v != 0.0) {
            m_count++;
            double delta = std::fabs(v) - m_mean_raw;
            m_mean_raw += delta / m_count;
        }
        m_curTv = tv;
        m_hasTv = true;
        m_isOK = true;
    }

protected:
    double m_alpha;
    bool   m_hasTv;
    double m_curTv;
    double m_curSum;
    double m_weightSum;
    double m_curMean;
    double m_mean_raw;
    int32_t m_count;
    bool   m_isOK;
};

// Helper for the seconds(120) literal used in the original constructor.
inline double seconds(double s) { return s; }

// ---------------------------------------------------------------------------
// Lightweight MultiMarket (replaces longbeach::MultiMarket)
// ---------------------------------------------------------------------------
// Original was a multi-level order book keyed by integerised price with
// sorted MktLevels. For the pure-math migration we keep only the operations
// OptionValues and downstream pricers actually use: a single-level best
// bid/ask with mid/empty/clear/set helpers. Callers needing full book
// semantics should use a dedicated Book type.
struct PriceSize
{
    PriceSize() : m_px(0.0), m_sz(0) {}
    PriceSize(double px, int32_t sz) : m_px(px), m_sz(sz) {}
    double  px() const { return m_px; }
    int32_t sz() const { return m_sz; }
    bool    empty() const { return m_sz == 0; }
    void    set(double px, int32_t sz) { m_px = px; m_sz = sz; }
    bool    operator==(const PriceSize& o) const { return m_px == o.m_px && m_sz == o.m_sz; }
    bool    operator!=(const PriceSize& o) const { return !(*this == o); }
protected:
    double  m_px;
    int32_t m_sz;
};

class MultiMarket
{
public:
    MultiMarket() {}

    void clear()
    {
        m_bestBid = PriceSize();
        m_bestAsk = PriceSize();
    }

    void eraseBids() { m_bestBid = PriceSize(); }
    void eraseAsks() { m_bestAsk = PriceSize(); }

    void setBest(int side, const PriceSize& ps) {
        if (side == 0) m_bestBid = ps; else m_bestAsk = ps;
    }
    PriceSize getBest(int side) const {
        return (side == 0) ? m_bestBid : m_bestAsk;
    }
    void clear_and_set(int side, const PriceSize& ps) {
        if (side == 0) { m_bestBid = ps; m_bestAsk = PriceSize(); }
        else { m_bestAsk = ps; m_bestBid = PriceSize(); }
    }

    bool empty() const { return m_bestBid.empty() && m_bestAsk.empty(); }
    bool hasBids() const { return !m_bestBid.empty(); }
    bool hasAsks() const { return !m_bestAsk.empty(); }

    double getMidPrice() const
    {
        if (hasBids() && hasAsks())
            return (m_bestBid.px() + m_bestAsk.px()) / 2.0;
        return 0.0;
    }

    const PriceSize& getBestBid() const { return m_bestBid; }
    const PriceSize& getBestAsk() const { return m_bestAsk; }
    PriceSize&       getBestBid()       { return m_bestBid; }
    PriceSize&       getBestAsk()       { return m_bestAsk; }

    // Compatibility aliases echoing the original's "front" naming.
    const PriceSize& bids() const { return m_bestBid; }
    const PriceSize& asks() const { return m_bestAsk; }

    void setBid(const PriceSize& bid) { m_bestBid = bid; }
    void setAsk(const PriceSize& ask) { m_bestAsk = ask; }
    void setBid(double px, int32_t sz) { m_bestBid = PriceSize(px, sz); }
    void setAsk(double px, int32_t sz) { m_bestAsk = PriceSize(px, sz); }

private:
    PriceSize m_bestBid;
    PriceSize m_bestAsk;
};

// ---------------------------------------------------------------------------
// OptionValues
// ---------------------------------------------------------------------------
class OptionValues
{
public:
    OptionValues()
        : m_fees( 0 )
        , m_impliedVol( 0 )
        , m_impliedVolSprd( 0 )
        , m_impliedBidVol( 0 )
        , m_impliedAskVol( 0 )
        , m_theoVol( 0 )
        , m_theoVolSprd( 0 )
        , m_theoBidVol( 0 )
        , m_theoAskVol( 0 )
        , vega_risk_norm(0)
        , targ_vega_tw(0)
        , m_fwd( 0 )
//        , m_stkfwd( 0 )
        , m_theo( 0 )
        , m_bPriced( false )
        , m_riskShiftVega( 0 )
        , m_adj()
        , m_alpha()
        , m_ema_sprd_vs_atmfwd(seconds(120))
    {
    }

    OptionGreeks& greeks() { return m_greeks; }
    const OptionGreeks& greeks() const { return m_greeks; }

//    double strikeforward() const { return m_stkfwd; }
    double theo_vol() const { return m_theoVol; }

//    double getForward() const { return m_fwd; }
    double getImpliedVol() const { return m_impliedVol; }
    double getImpliedVolSprd() const { return m_impliedVolSprd; }
    double getImpliedBidVol() const { return m_impliedBidVol; }
    double getImpliedAskVol() const { return m_impliedAskVol; }

    double impliedVol() const { return m_impliedVol; }
    double impliedVolSprd() const { return m_impliedVolSprd; }
    double impliedVolBid() const { return m_impliedBidVol; }
    double impliedVolAsk() const { return m_impliedAskVol; }


    double getTheoVol() const { return m_theoVol; }
    double getTheoVolSprd() const { return m_theoVolSprd; }
    double getTheoBidVol() const { return m_theoBidVol; }
    double getTheoAskVol() const { return m_theoAskVol; }

    double theoVol() const { return m_theoVol; }
    double theoVolSprd() const { return m_theoVolSprd; }
    double theoVolBid() const { return m_theoBidVol; }
    double theoVolAsk() const { return m_theoAskVol; }

    double theo() const { return m_theo; }
    double forward() const { return m_fwd; }

    const MultiMarket& ourMarket() const { return m_ourMarket; }
    MultiMarket& ourMarket() { return m_ourMarket; }

    struct Prices
    {
    public:
        Prices() { clear(); }
        void clear()
        {
            mid = bid = ask = 0;
        }
        double mid, bid, ask;
    };
    /// theoretical prices
    const Prices& theoPrices() const { return m_theoPrices; }
    Prices& theoPrices() { return m_theoPrices; }

    void setPriced( bool bPriced ) { m_bPriced = bPriced; }
    bool isPriced() const { return m_bPriced; }

    void setRiskShiftVega( double s ) { m_riskShiftVega = s; }
    double getRiskShiftVega() const { return m_riskShiftVega; }

    OptionValues& setTheo(double v) { m_theo=v; return *this; }
    OptionValues& setTheoVol(double v) { m_theoVol=v; return *this; }
    OptionValues& setForward(double fwd) { m_fwd=fwd; return *this; }
//    OptionValues& setStrikeForward(double fwd) { m_stkfwd=fwd; return *this; }

    double m_fees;

    double m_impliedVol;
    double m_impliedVolSprd;
    double m_impliedBidVol;
    double m_impliedAskVol;

    double m_theoVol;
    double m_theoVolSprd;
    double m_theoBidVol;
    double m_theoAskVol;

    double vega_risk_norm;
    double targ_vega_tw;

private:
    // our prices
    double m_fwd;
//    double m_stkfwd;
    double m_theo;

public:
    struct Adjustments
    {
        double delta_risk;
        double delta_risk2;
        double vega_risk;
        double vega_risk2;
        double risk_v_disp;
        double bump_fwd;
        double bump_vol;
        double total;
        double total_risk;

        void clear()
        {
            memset(this, 0, sizeof(*this));
            total = NAN;
        }
    };
    Adjustments& adj() { return m_adj; }
    const Adjustments& adj() const { return m_adj; }

    struct Alphas
    {
        double atmsig;
        double rollema;
        double sizebias;
        double vegaflow;
        double frontfut_skew;
        double frontatmv_flow;
        double deltaflow;

        double delta_total;
        double vega_total;
        double total;

        void clear()
        {
            memset(this, 0, sizeof(*this));
            total = NAN;
        }
    };
    Alphas& alpha() { return m_alpha; }
    const Alphas& alpha() const { return m_alpha; }

    EMAFilter& ema_sprd_vs_atmfwd() { return m_ema_sprd_vs_atmfwd; }

public:
    struct OtcAttributes
    {
        OtcAttributes()
            : tradeid("")
            , premium(0)
            , trade_vol(0)
        {}

        std::string tradeid;
        double premium;
        double trade_vol;
    };
    OtcAttributes& otc() { return m_otcAttributes; }

private:
    OptionGreeks m_greeks;
    MultiMarket m_ourMarket;

    bool m_bPriced;
    double m_riskShiftVega;
    Adjustments m_adj;
    Alphas m_alpha;
    Prices m_theoPrices;
    OtcAttributes m_otcAttributes;

    EMAFilter m_ema_sprd_vs_atmfwd;
};

} // namespace wt_option
