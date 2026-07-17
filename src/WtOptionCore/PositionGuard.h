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
        Config() = default;
    };

    using GetTimeFn = std::function<double()>;
    using DiscrepancyCallback = std::function<void(const std::string&, int32_t diff)>;

    PositionGuard() : m_cfg() {}
    explicit PositionGuard(const Config& cfg) : m_cfg(cfg) {}

    void onFill(bool isBuy, uint32_t qty);
    void onBrokerPosition(bool isLong, double vol);

    bool isOK() const { return !m_disabled; }
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
};

using PositionGuardPtr = std::shared_ptr<PositionGuard>;

} // namespace wt_option
