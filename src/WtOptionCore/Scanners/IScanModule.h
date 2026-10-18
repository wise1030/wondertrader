/*!
 * \file IScanModule.h
 * \brief Scanner module base class for option trading strategies
 * 
 * Migrated from longbeach/optiontrader/IScanModule.h
 */

#pragma once

#include "../optioncoretypes.h"
#include "../OptionData.h"
#include "../OptionGrid.h"
#include <memory>
#include <string>
#include <map>
#include <functional>

namespace wt_option {

// Forward declarations
class OptionTraderContext;
using OptionTraderContextPtr = std::shared_ptr<OptionTraderContext>;

/**
 * @brief Scanner hit event data
 */
struct ScannerHitEvent {
    OptionData* option;         // Option that triggered
    int32_t valueIndex;         // Which value set triggered (0-4)
    double signal;              // Signal strength
    std::string reason;         // Hit reason description
    uint64_t timestamp;         // Event timestamp
    
    ScannerHitEvent() : option(nullptr), valueIndex(0), signal(0), timestamp(0) {}
};

/**
 * @brief Scanner event listener interface
 */
class IScannerListener {
public:
    virtual ~IScannerListener() = default;
    
    /**
     * @brief Called when scanner detects an opportunity
     */
    virtual void onScannerHit(const ScannerHitEvent& event) = 0;
};

/**
 * @brief Base class for all scanner modules
 * 
 * Scanners analyze option data and generate trading signals.
 */
class IScanModule {
public:
    IScanModule(const std::string& name);
    IScanModule() = default;  // backward compat for stub-derived scanners
    virtual ~IScanModule() = default;

    // Identity - returns by value for stub compat
    virtual std::string getName() const { return m_name; }

    // Lifecycle
    virtual void onStart() {}
    virtual void onStop() {}

    // Enable/disable
    bool isEnabled() const { return m_enabled; }
    void setEnabled(bool enabled);
    bool isActive() const { return m_active; }

    // Per-expiry override (ScannerConfig.expiryOverrides); default enabled.
    void setExpiryEnabled(uint32_t expiry, bool enabled) { m_expiryEnabled[expiry] = enabled; }
    bool isExpiryEnabled(uint32_t expiry) const {
        auto it = m_expiryEnabled.find(expiry);
        return it == m_expiryEnabled.end() ? true : it->second;
    }
    virtual void onEnable() {}
    virtual void onDisable() {}

    // Processing
    virtual void onTick(const OptionGrid* grid) {}
    virtual void onOptionUpdate(OptionData* option) {}
    virtual void onUnderlyingUpdate(double price) {}

    // Panic handling
    virtual void onPanic() {}

    // Refresh
    virtual void refresh() {}

    // Stub compat: onOptionHit - called when a scanner detects a hit.
    // Default implementation delegates to onOptionUpdate.
    virtual void onOptionHit(OptionData* od, int32_t valueIndex) { (void)valueIndex; onOptionUpdate(od); }

    // Listeners
    void addListener(IScannerListener* listener);
    void removeListener(IScannerListener* listener);
    
protected:
    /**
     * @brief Notify listeners of a scanner hit
     */
    void notifyHit(const ScannerHitEvent& event);
    
    /**
     * @brief Get current timestamp
     */
    uint64_t getTimestamp() const;
    
    /**
     * @brief Log message
     */
    void log(const std::string& message);

    // Derived scanners call this when a signal fires; keeps event construction
    // (timestamp/valueIndex/reason) consistent across all scanners.
    void publishHit(OptionData* option, double signal, const std::string& reason);

    bool m_active = false;    // session-start gate; dispatch also checks enabled
    
private:
    std::string m_name;
    bool m_enabled = false;   // BUGFIX: default ctor left this indeterminate
    std::vector<IScannerListener*> m_listeners;
    std::map<uint32_t, bool> m_expiryEnabled;
};

using IScanModulePtr = std::shared_ptr<IScanModule>;

/**
 * @brief Per-expiry override for scanner configuration
 * 
 * Mirrors longbeach MMScanner::ExpiryContext pattern:
 * each expiry can override common scanner params.
 */
struct ScannerExpiryOverrides {
    bool enabled = true;            // Per-expiry enable/disable
    int32_t maxPosOpt = -1;         // Max option position (-1 = use global)
    int32_t maxPosFut = -1;         // Max future/hedge position (-1 = use global)
    int32_t maxOrderSize = -1;      // Max order size (-1 = use global)
    double minProfit = -1;          // Min profit threshold (-1 = use global)
    double minProfitVol = -1;       // Min vol profit threshold (-1 = use global)
    
    /// Check if field is overridden (not default -1)
    static bool isSet(int32_t v) { return v >= 0; }
    static bool isSet(double v)  { return v >= 0; }
};

/**
 * @brief Scanner configuration base
 * 
 * Supports per-expiry overrides following longbeach pattern:
 * global params + optional per-expiry overrides with fallback.
 */
struct ScannerConfig {
    std::string name;
    bool enabled = true;
    int32_t priority = 0;

    // Common numeric overrides (R6 follow-up: the factory bridge used to
    // ignore all per-scanner params and always run code defaults).
    double min_edge = 0.5;
    double delta_min = 0.1;
    double delta_max = 0.9;
    double max_spread = 10.0;
    int32_t min_size = 1;

    // GARCH-specific overrides
    double garch_omega = 1e-10;
    double garch_alpha = 0.08;
    double garch_beta = 0.90;
    int32_t warmup_returns = 50;
    double annualization_factor = 60480.0;
    
    /// Per-expiry overrides, keyed by expiry date (e.g. 202505)
    std::map<uint32_t, ScannerExpiryOverrides> expiryOverrides;
    
    /// Check if scanning is enabled for a specific expiry
    bool isExpiryEnabled(uint32_t expiry) const {
        if (!enabled) return false;
        auto it = expiryOverrides.find(expiry);
        if (it != expiryOverrides.end()) return it->second.enabled;
        return true; // default: enabled if not explicitly overridden
    }
    
    /// Get expiry override (nullptr if none)
    const ScannerExpiryOverrides* getExpiryOverride(uint32_t expiry) const {
        auto it = expiryOverrides.find(expiry);
        return (it != expiryOverrides.end()) ? &it->second : nullptr;
    }
    
    virtual ~ScannerConfig() = default;
};

using ScannerConfigPtr = std::shared_ptr<ScannerConfig>;

/**
 * @brief Scanner factory for creating scanners from configuration
 */
class ScannerFactory {
public:
    using CreatorFunc = std::function<IScanModulePtr(const ScannerConfig&)>;
    
    static ScannerFactory& instance();
    
    void registerScanner(const std::string& type, CreatorFunc creator);
    IScanModulePtr createScanner(const std::string& type, const ScannerConfig& config);
    
private:
    ScannerFactory() = default;
    std::map<std::string, CreatorFunc> m_creators;
};

// Macro for scanner registration
#define REGISTER_SCANNER(type, classname) \
    static bool _registered_##classname = []() { \
        ScannerFactory::instance().registerScanner(type, \
            [](const ScannerConfig& cfg) { return std::make_shared<classname>(cfg); }); \
        return true; \
    }()

} // namespace wt_option
