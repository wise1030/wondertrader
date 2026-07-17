#pragma once

#include <string>
#include <vector>
#include <map>
#include <functional>
#include <memory>
#include <cstdint>

namespace wt_option {

class OptionGrid;

struct PredictionState {
    double forward = 0;         // predicted forward price
    double atmVol = 0;          // predicted ATM vol
    double confidence = 0;      // 0-1 confidence level
    uint64_t timestamp = 0;    // prediction timestamp
};

class IPredictor {
public:
    virtual ~IPredictor() = default;

    virtual const std::string& getName() const = 0;
    virtual bool init() = 0;
    virtual void update(const PredictionState& input) = 0;
    virtual const PredictionState& getPrediction() const = 0;
    virtual bool isReady() const = 0;
};

using IPredictorPtr = std::shared_ptr<IPredictor>;

class TriggerEngine {
public:
    using TriggerCallback = std::function<void()>;

    void addTrigger(double intervalSec, TriggerCallback cb) {
        m_triggers.push_back({intervalSec, std::move(cb), 0});
    }

    void onTick(double now) {
        for (auto& t : m_triggers) {
            if ((now - t.lastFire) >= t.interval) {
                t.lastFire = now;
                t.cb();
            }
        }
    }

private:
    struct Trigger {
        double interval = 0;
        TriggerCallback cb;
        double lastFire = 0;
    };
    std::vector<Trigger> m_triggers;
};

using TriggerEnginePtr = std::shared_ptr<TriggerEngine>;

class SignalsPredictor : public IPredictor {
public:
    SignalsPredictor(const std::string& name = "SignalsPredictor")
        : m_name(name) {}

    const std::string& getName() const override { return m_name; }
    bool init() override { return true; }

    void update(const PredictionState& input) override {
        // Simple EMA smoothing
        if (!m_ready) {
            m_prediction = input;
            m_ready = true;
        } else {
            double alpha = 0.3;  // smoothing factor
            m_prediction.forward   = alpha * input.forward + (1-alpha) * m_prediction.forward;
            m_prediction.atmVol    = alpha * input.atmVol + (1-alpha) * m_prediction.atmVol;
            m_prediction.confidence = input.confidence;
            m_prediction.timestamp = input.timestamp;
        }
    }

    const PredictionState& getPrediction() const override { return m_prediction; }
    bool isReady() const override { return m_ready; }

private:
    std::string m_name;
    PredictionState m_prediction;
    bool m_ready = false;
};

} // namespace wt_option
