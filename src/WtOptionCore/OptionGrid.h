/*!
 * \file OptionGrid.h
 * \brief 3-level option data grid: expiry → strike → call/put
 *
 * Migrated from quantbox optioncore/OptionGrid.h (355 lines).
 * Preserves: 3-level structure, dynamic contract discovery, synthetic price,
 * ATM strike lookup, computeValues delegation.
 * Replaces: boost::multi_index → vector + unordered_map,
 *   MarketDataContext → IBaseDataMgr, monitorOptions → onTick-driven,
 *   IPriceProvider → double m_underlyingPrice, IStrikeFinder → simple search.
 */
#pragma once

#include "IOptionGrid.h"
#include "OptionData.h"
#include "ExpiryData.h"
#include "StrikeData.h"
#include "IOptionPricer.h"
#include "optioncoretypes.h"
#include "IStrikeFinder.h"

#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <set>
#include <shared_mutex>
#include <memory>
#include <cstdint>

namespace wtp {
    class IBaseDataMgr;
    class WTSSessionInfo;
    class WTSTickData;
}

namespace wt_option {

class IOptionPricer;
class PeriodicCurveFitter;

class OptionGrid : public IOptionGrid {
public:
    OptionGrid(const std::string& optionProduct,
               const std::string& underlyingCode,
               wtp::IBaseDataMgr* bdMgr,
               wtp::WTSSessionInfo* sessInfo);
    virtual ~OptionGrid();

    // --- IOptionGrid interface ---
    const std::string& getSymbol() const override { return m_optionProduct; }
    const std::string& getUnderlyingCode() const override { return m_underlyingCode; }

    OptionDataPtr get(uint32_t expiry, strike_t strike, OptionRight right) override;
    OptionDataPtr get(const std::string& code) override;

    void computeValues(IOptionPricer* pricer = nullptr) override;
    double getUnderlyingPrice() const override;

    const ExpiryTable& expiries() const override { return m_expiries; }
    ExpiryDataPtr getExpiryData(uint32_t expiry) const override;
    ExpiryDataPtr getFrontMonthExpiryData() override;

    const std::vector<OptionDataPtr>& getAllOptions() const override { return m_allOptions; }
    const std::vector<StrikeDataPtr>& getAllStrikes() const override { return m_allStrikes; }
    std::vector<StrikeDataPtr> getStrikesByExpiry(uint32_t expiry) const override;

    size_t numStrikes() const override { return m_allStrikes.size(); }
    size_t numOptions() const override { return m_allOptions.size(); }

    void addListener(IOptionGridListener* listener) override;
    void removeListener(IOptionGridListener* listener) override;

    // --- WT-specific: tick-driven contract discovery ---
    void onTick(const std::string& stdCode, const wtp::WTSTickData* tick);

    // --- TickData version (for async path — no WTSTickData* dependency) ---
    // TickData is the slimmed struct from OptionAsyncEventProcessor
    struct TickDataRef {
        double price = 0;
        double bid = 0;
        double ask = 0;
        double bidQty = 0;
        double askQty = 0;
        uint32_t expireDate = 0;   // B28: exact expiry YYYYMMDD (0 = unknown)
        uint32_t actionTime = 0;   // exchange action time (HHMMSSmmm when available)
        uint64_t updateTime = 0;   // local receive timestamp, epoch milliseconds

        // P1 follow-up (五档深度): must mirror OptionAsyncEventProcessor
        // TickData::DEPTH — the two structs are bridged by value in
        // HftOptionStrategy's on_tick lambda.
        static constexpr int32_t DEPTH = 5;
        double bidPrices[DEPTH] = {};
        double askPrices[DEPTH] = {};
        double bidQtys[DEPTH] = {};
        double askQtys[DEPTH] = {};
        int32_t numBidLevels = 0;
        int32_t numAskLevels = 0;
    };
    void onTick(const std::string& stdCode, const TickDataRef& tick);

    // --- Underlying price ---
    // 2026-11-26 refactor: the grid-level price is DERIVED, not state.
    // getUnderlyingPrice() returns the front expiry's underlying price
    // (front = nearest expiry with daysToExpiry > 0, queried on every call);
    // freshness is derived from that expiry's price-feed timestamp.
    // See frontExpiry() and analysis/front-underlying-roles-analysis-20261126.md.

    // Market freshness gate.  Timeout 0 disables the check.
    void setMarketStaleTimeoutMs(uint64_t ms) { m_marketStaleTimeoutMs = ms; }
    uint64_t getMarketStaleTimeoutMs() const { return m_marketStaleTimeoutMs; }
    bool isUnderlyingFresh(uint64_t nowMs) const;
    bool hasFreshOptionMarket(uint64_t nowMs) const;

    // --- Compute time (set by pricer before computeValues for EMA updates) ---
    void setComputeTime(double t) { m_computeTime = t; }
    double getComputeTime() const { return m_computeTime; }

    // --- Per-expiry underlying routing ---
    // Register a contract code as the pricing underlying for a specific expiry.
    // When that contract's tick arrives, the corresponding ExpiryData gets its
    // own underlying price (separate from the global m_underlyingPrice).
    void setExpiryUnderlying(uint32_t expiry, const std::string& code);
    /// P2/G3: register an EXCLUSIVE per-expiry pricing underlying.  The
    /// global m_underlyingCode is then NOT auto-routed into this expiry
    /// (previously a global tick overwrote the per-expiry price: both were
    /// registered and last-tick-won).
    void setExpiryOwnUnderlying(uint32_t expiry, const std::string& code) {
        if (code.empty()) return;
        m_expiryOwnUnderlying[expiry] = code;
        setExpiryUnderlying(expiry, code);
    }
    bool hasExpiryOwnUnderlying(uint32_t expiry) const {
        return m_expiryOwnUnderlying.find(expiry) != m_expiryOwnUnderlying.end();
    }
    const std::string* expiryOwnUnderlying(uint32_t expiry) const {
        auto it = m_expiryOwnUnderlying.find(expiry);
        return it == m_expiryOwnUnderlying.end() ? nullptr : &it->second;
    }
    /// P0-4: register forward-calculation config for an expiry that may not
    /// exist yet; applied when the ExpiryData is lazily created.
    void setExpiryForwardConfig(uint32_t expiry, bool includeFuture, int minStrikes,
                                const std::map<int32_t, double>& goodPointsTable = {}) {
        m_expiryFwdCfg[expiry] = {includeFuture, minStrikes, goodPointsTable};
    }
    bool isExpiryUnderlying(const std::string& code) const {
        return m_expiryUnderlyingMap.find(code) != m_expiryUnderlyingMap.end();
    }

    // --- Option pricer ---
    void setOptionPricer(IOptionPricerPtr pricer) { m_optionPricer = pricer; }
    IOptionPricerPtr getOptionPricer() const { return m_optionPricer; }

    // --- Curve fitter ---
    void setPeriodicCurveFitter(std::shared_ptr<PeriodicCurveFitter> fitter) { m_fitter = fitter; }
    std::shared_ptr<PeriodicCurveFitter> getPeriodicCurveFitter() const { return m_fitter; }

    // --- Front month (DERIVED, no state — 2026-11-26 refactor) ---
    // frontExpiry() returns the nearest expiry with daysToExpiry > 0
    // (fallback: earliest expiry in the map; nullptr when empty).  The
    // former m_frontMonth/m_frontMonthExpiry state, reevaluateFrontMonth()
    // roll logic and the isFrontUnderlyingCode() grid-price gate were all
    // removed: the grid-level price now derives from this query, so a
    // maintained "which month is front" state was a redundant copy of the
    // expiry table plus the dte predicate.  See
    // analysis/front-underlying-roles-analysis-20261126.md.
    ExpiryDataPtr frontExpiry() const;

    // Refresh daysToExpiration for all expiries (call on session begin)
    void refreshExpiryDays();

    // Remove expired expiries from the grid (quotes are already suppressed by
    // the pricer; this releases the data and keeps session roll bounded).
    void purgeExpired();

    // --- Risk-free rate (B2: flat or curve-based per expiry) ---
    void setRiskFreeRate(double r) { m_riskFreeRate = r; }
    double getRiskFreeRate() const { return m_riskFreeRate; }
    // Rate curve: maps days-to-expiry -> annualized rate. If set, each expiry
    // gets its rate from the curve via linear interpolation; otherwise the
    // flat m_riskFreeRate is used.
    void setRateCurve(const std::vector<std::pair<double, double>>& curve) {
        m_rateCurve = curve;
    }
    double getRateForDays(int32_t days) const;

    // --- ATM strike ---
    StrikeDataPtr getAtmStrike(uint32_t expiry);
    StrikeDataPtr findStrikeFromGrid(uint32_t expiry, double price) const;
    StrikeDataPtr findOrCreateStrike(uint32_t expiry, strike_t strike);

    // --- Forward ---
    double getFrontForward();
    double getAtmForward(uint32_t expiry);

    // --- ATM signal ---
    double getAtmSig() { return m_atmSig; }
    void setAtmSig(double sig) { m_atmSig = sig; }

    // --- Expiry management ---
    bool exists(const std::string& code) const;
    std::vector<uint32_t> getValidExpiries() const;

    // --- Create option from stdCode (dynamic discovery) ---
    OptionDataPtr createOption(const std::string& stdCode);

    // Holiday calendar (from holidays.json, used by ExpiryData when m_bdMgr is null)
    void setHolidays(std::set<uint32_t> h) { m_holidays = std::move(h); }
    size_t numHolidays() const { return m_holidays.size(); }

    // P10: Current trading date (set by strategy from stra_get_date)
    void setCurrentDate(uint32_t d) { m_currentDate = d; }
    uint32_t getCurrentDate() const { return m_currentDate; }
    void setCurSecOfDay(uint32_t s) { m_curSecOfDay = s; }
    uint32_t getCurSecOfDay() const { return m_curSecOfDay; }

    // Forward stale timeout (microseconds). When validCount drops below minStrikes,
    // forwardReady stays true for this duration before being set to false.
    void setForwardStaleTimeoutUs(uint64_t us) { m_forwardStaleTimeoutUs = us; }

    /// R3: per-expiry strike-grid finder (quantbox IStrikeFinder absorption).
    /// When set, getAtmStrike snaps the reference price to the listed grid
    /// before nearest-search — prevents selecting non-listed virtual strikes.
    void setStrikeFinder(uint32_t expiry, IStrikeFinderPtr finder) {
        m_strikeFinders[expiry] = std::move(finder);
    }
    uint64_t getForwardStaleTimeoutUs() const { return m_forwardStaleTimeoutUs; }

private:
    OptionDataPtr __createOption(const std::string& stdCode);
    StrikeDataPtr __findOrCreateStrike(uint32_t expiry, strike_t strike);
    ExpiryDataPtr __getOrCreateExpiryData(uint32_t expiry);
    double __getBestSyntheticPrice(const ExpiryDataPtr& ed);
    void __notifyAddOption(const OptionDataPtr& od);
    void __notifyAddExpiry(const ExpiryDataPtr& ed);
    void __notifyComputeCompleted();

    // Option product info
    std::string m_optionProduct;     // e.g. "cu"
    std::string m_underlyingCode;    // e.g. "SHFE.cu.2502"
    std::string m_exchange;          // e.g. "SHFE"

    // WT framework
    wtp::IBaseDataMgr* m_bdMgr;
    wtp::WTSSessionInfo* m_sessInfo;

    // 3-level data containers
    std::vector<OptionDataPtr> m_allOptions;
    std::vector<StrikeDataPtr> m_allStrikes;
    std::set<uint32_t> m_holidays;  // holiday calendar from holidays.json
    uint32_t m_currentDate = 0;  // P10: current trading date
    /// Simulated seconds-of-day, injected per tick by the strategy (timer
    /// payload) and forwarded to every ExpiryData — see ExpiryData::
    /// getIntradayFraction() (audit 2026-11-13 v2 T1-4).
    uint32_t m_curSecOfDay = 0;
    std::unordered_map<std::string, OptionDataPtr> m_optionsByCode;
    ExpiryTable m_expiries;

    // Index: expiry → list of strikes (for fast lookup)
    std::map<uint32_t, std::vector<StrikeDataPtr>> m_strikesByExpiry;

    // Underlying — the grid-level price is DERIVED, not a per-tick mirror:
    // multi-series scenes read the front expiry's price; single-underlying
    // scenes read the global price state written by the global-tick branch.
    double m_globalUnderlyingPrice = 0;
    uint64_t m_globalUnderlyingUpdateTime = 0;  // epoch ms, 0 = unknown
    uint64_t m_marketStaleTimeoutMs = 3000;

    // Per-expiry underlying: contract code -> list of expiries that use it as pricing underlying
    std::unordered_map<std::string, std::vector<uint32_t>> m_expiryUnderlyingMap;
    // P0-4: pending per-expiry forward config {includeFuture, minStrikes};
    // minStrikes < 0 links the gate to the vol-fit good-points table (third).
    struct FwdCfg { bool includeFuture; int minStrikes; std::map<int32_t, double> goodPointsTable; };
    std::map<uint32_t, FwdCfg> m_expiryFwdCfg;
    // P2/G3: expiries with an exclusive per-expiry pricing underlying
    std::map<uint32_t, std::string> m_expiryOwnUnderlying;

    // Compute time for EMA updates
    double m_computeTime = 0;

    // Risk-free rate (B2)
    double m_riskFreeRate = 0;
    std::vector<std::pair<double, double>> m_rateCurve; // (days, rate) sorted by days

    // Pricer / fitter
    IOptionPricerPtr m_optionPricer;
    std::shared_ptr<PeriodicCurveFitter> m_fitter;

    // ATM signal
    double m_atmSig = 0;

    // ATM forward cache (per compute cycle)
    std::map<uint32_t, double> m_atmFwdCache;

    // Listeners
    std::vector<IOptionGridListener*> m_listeners;

    // Thread safety for container modification
    mutable std::shared_mutex m_gridMutex;

    // Forward stale timeout (microseconds, default 5s)
    uint64_t m_forwardStaleTimeoutUs = 5000000;
    std::map<uint32_t, IStrikeFinderPtr> m_strikeFinders;   // R3
};

} // namespace wt_option
