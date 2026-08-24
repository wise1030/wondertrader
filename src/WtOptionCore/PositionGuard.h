#pragma once

#include <cstdint>
#include <string>
#include <functional>
#include <memory>

namespace wt_option {

class PositionGuard {
public:
    struct Config {
        int32_t tolerance = 0;
        double alertCooldownSec = 10.0;
        bool disableOnBreach = true;
        // C1: broker staleness — freeze new-opening trades when the broker
        // position feed is older than this (0 = disabled)
        double staleFreezeSec = 10.0;
        // C1: clamp internal position to broker when |diff| exceeds this
        // (0 = disabled; broker wins, discrepancy still alerted)
        int32_t clampLimit = 0;
        // C2: optimistic-reconciliation undo window (quantbox PositionTracker).
        // A broker-vs-internal adjustment made here is rolled back if a fill
        // arrives within this window (the fill likely explains the diff).
        double undoWindowSec = 2.0;
        Config() = default;
    };

    using GetTimeFn = std::function<double()>;
    using DiscrepancyCallback = std::function<void(const std::string&, int32_t diff)>;

    PositionGuard() : m_cfg() {}
    explicit PositionGuard(const Config& cfg) : m_cfg(cfg) {}

    void onFill(bool isBuy, uint32_t qty);
    void onBrokerPosition(bool isLong, double vol);

    bool isOK() const { return !m_disabled && !isBrokerStale(); }
    /// C1: true when broker feed went quiet beyond staleFreezeSec
    bool isBrokerStale() const;
    int32_t getDiff() const { return m_internalPos - m_brokerPos; }
    int32_t getInternalPos() const { return m_internalPos; }
    int32_t getBrokerPos() const { return m_brokerPos; }

    void reconcile();
    void setGetTimeFn(GetTimeFn fn) { m_getTime = std::move(fn); }
    void setDiscrepancyCallback(DiscrepancyCallback cb) { m_callback = std::move(cb); }

private:
    Config m_cfg;
    int32_t m_internalPos = 0;
    int32_t m_brokerPos = 0;
    bool m_disabled = false;
    bool m_initialized = false;
    double m_lastAlertTime = 0;
    GetTimeFn m_getTime;
    DiscrepancyCallback m_callback;

    // C1/C2 state
    double m_lastBrokerUpdate = 0;      // timestamp of last broker report
    int32_t  m_pendingAdjust = 0;       // C2: adjustment awaiting undo window
    double   m_pendingAdjustTime = 0;

    void applyUndoIfDue(double now);
};

using PositionGuardPtr = std::shared_ptr<PositionGuard>;

} // namespace wt_option
