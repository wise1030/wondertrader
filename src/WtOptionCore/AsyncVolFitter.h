#pragma once

#include "IVolCurve.h"
#include "SingleFlightTaskRunner.h"
#include "OptionPricer2.h"
#include "GvvVolCurve.h"
#include "PowerVolCurve.h"

#include <atomic>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace wt_option {

/// Immutable input for one async vol-fit job.  The worker builds this at a
/// batch boundary and never mutates it afterwards.
struct VolFitExpirySnapshot {
    uint32_t expiry = 0;
    double forward = 0.0;
    double maturity = 0.0;
    double atmVol = 0.0;
    IVolCurve::dataset_t points;   // {K-F, iv/atmVol}
    uint64_t version = 0;
};

class AsyncVolFitter {
public:
    explicit AsyncVolFitter(std::string curve2Type = "gvv")
        : m_curve2Type(std::move(curve2Type)) {}
    ~AsyncVolFitter() { m_runner.stop(); }

    void setCurve2Type(const std::string& t) {
        if (t == "power" || t == "gvv") m_curve2Type = t;
    }

    /// Replace any queued snapshot set.  Single-flight: while a fit is running
    /// a duplicate submit returns false; caller may retry next period.
    bool submit(std::vector<VolFitExpirySnapshot> snapshots) {
        if (snapshots.empty()) return false;
        {
            std::lock_guard<std::mutex> lock(m_mtx);
            m_pending = std::move(snapshots);
            m_ready.store(false, std::memory_order_release);
        }
        return m_runner.submit("volfit", [this]() -> SingleFlightTaskRunner::TaskResult {
            std::vector<VolFitExpirySnapshot> input;
            {
                std::lock_guard<std::mutex> lock(m_mtx);
                input = m_pending;
            }
            auto fitted = fit(input);
            {
                std::lock_guard<std::mutex> lock(m_mtx);
                m_result = std::move(fitted);
                m_ready.store(true, std::memory_order_release);
            }
            return {"volfit", true, ""};
        });
    }

    bool hasResult() const { return m_ready.load(std::memory_order_acquire); }

    /// Worker-only batch-boundary publish.  Returns false when no completed
    /// result is available.
    bool tryPublish(OptionPricer2& pricer) {
        if (!m_ready.load(std::memory_order_acquire)) return false;
        std::vector<OptionPricer2::FittedCurveSet> result;
        {
            std::lock_guard<std::mutex> lock(m_mtx);
            result.swap(m_result);
            m_ready.store(false, std::memory_order_release);
        }
        pricer.publishFittedCurves(result);
        return true;
    }

private:
    std::vector<OptionPricer2::FittedCurveSet>
    fit(const std::vector<VolFitExpirySnapshot>& input) const {
        std::vector<OptionPricer2::FittedCurveSet> out;
        out.reserve(input.size());
        for (const auto& s : input) {
            if (s.version == 0 || !(s.forward > 0) || !(s.maturity > 0)
                || !(s.atmVol > 0) || s.points.empty())
                continue;

            IVolCurvePtr curve;
            auto gvv = std::make_shared<GvvVolCurve>();
            gvv->setATMForward(s.forward);
            gvv->setMaturity(s.maturity);
            gvv->setATMVol(s.atmVol);
            if (gvv->fit(s.points)) {
                curve = gvv;
            } else {
                // Numeric fallback: still publish a versioned curve so the
                // async path is observable; GVV quality can be tuned separately.
                auto power = std::make_shared<PowerVolCurve>(GvvVolCurve::SPOTVOL);
                power->setATMForward(s.forward);
                power->setMaturity(s.maturity);
                power->setATMVol(s.atmVol);
                if (!power->fit(s.points)) continue;
                curve = power;
            }

            IVolCurvePtr curve2;
            if (m_curve2Type == "power")
                curve2 = std::make_shared<PowerVolCurve>(GvvVolCurve::RHO);
            else
                curve2 = std::make_shared<GvvVolCurve>();
            curve2->setATMForward(s.forward);
            curve2->setMaturity(s.maturity);
            curve2->setATMVol(s.atmVol);
            curve2->fit(s.points);

            out.push_back({s.expiry, curve, curve2, s.atmVol, s.version});
        }
        return out;
    }

private:
    SingleFlightTaskRunner m_runner;
    mutable std::mutex m_mtx;
    std::vector<VolFitExpirySnapshot> m_pending;
    std::vector<OptionPricer2::FittedCurveSet> m_result;
    std::atomic<bool> m_ready{false};
    std::string m_curve2Type;
};

} // namespace wt_option
