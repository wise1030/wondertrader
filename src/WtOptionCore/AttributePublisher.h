#pragma once

#include "optioncoretypes.h"
#include "OptionValues.h"
#include "OptionGreeks.h"

#include <string>
#include <map>
#include <chrono>
#include <cstdint>
#include <functional>

namespace wt_option {

class OptionData;
class OptionTradingData;
class UnderlyingTradingData;
class OptionRisk;
class OptionGrid;

class AttributePublisher
{
public:
    AttributePublisher() = default;
    ~AttributePublisher() = default;

    // Throttle: minimum seconds between publishes (original: 1500ms)
    void setPublishInterval(double seconds) { m_publishInterval = seconds; }
    double getPublishInterval() const { return m_publishInterval; }

    // Collect attributes from a single option's trading data
    void collectOption(const std::string& code, OptionTradingData* otd, const OptionData* od);
    // Collect attributes from a single underlying's trading data
    void collectUnderlying(const std::string& code, UnderlyingTradingData* utd);

    // Publish collected attributes via WTSLogger (structured log lines)
    // Returns true if published, false if throttled.
    bool publish(double now);

    // Force-publish a single instrument's attributes immediately
    void publishSingle(const std::string& code, bool isOption);

    // Refresh: mark all attributes dirty for a full republish
    void refresh();

    // Risk summary: portfolio Greeks from OptionRisk
    void setRisk(OptionRisk* risk) { m_risk = risk; }
    void setGrid(OptionGrid* grid) { m_grid = grid; }

    // B17: Active counters
    int32_t getActiveOptions() const { return m_activeOptions; }
    int32_t getActiveFutures() const { return m_activeFutures; }
    int32_t getActiveSides() const { return m_activeSides; }

private:
    struct OptionAttrs {
        double delta = 0;
        double position = 0;
        bool enabled = false;
        double mbid = 0, mask = 0;     // market bid/ask
        double obid = 0, oask = 0;     // our bid/ask
        int32_t obid_sz = 0, oask_sz = 0;
        double fwd = 0;
        double theo = 0;
        double impliedVol = 0;
        bool priced = false;
    };

    struct UnderlyingAttrs {
        double delta = 0;
        double position = 0;
        bool enabled = false;
        double mbid = 0, mask = 0;
        double obid = 0, oask = 0;
        double fwd = 0;
    };

    std::map<std::string, OptionAttrs> m_optAttrs;
    std::map<std::string, UnderlyingAttrs> m_undAttrs;

    double m_publishInterval = 1.5;  // seconds
    double m_lastPublishTime = 0;
    bool m_dirty = true;

    OptionRisk* m_risk = nullptr;
    OptionGrid* m_grid = nullptr;

    // B17: Active counters
    int32_t m_activeOptions = 0;
    int32_t m_activeFutures = 0;
    int32_t m_activeSides = 0;
};

using AttributePublisherPtr = std::shared_ptr<AttributePublisher>;

} // namespace wt_option
