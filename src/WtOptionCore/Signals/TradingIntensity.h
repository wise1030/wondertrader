/*!
 * \file TradingIntensity.h
 * \brief Online estimate of fill intensity vs quote distance (P1 batch-3.2).
 *
 * Avellaneda-Stoikov style: the fill intensity of a quote decays with its
 * distance from the mid as lambda(delta) = A * exp(-k * delta). A small k
 * (slow decay = fills happen far from the mid) means the book is TOXIC —
 * quotes get picked off across a wide band — and the spread should widen.
 * A large k (fills concentrated at the touch) supports tighter quoting.
 *
 * We bucket observed fills by their distance-to-mid in ticks and fit
 * log(count) ~ a - k * delta by least squares over a rolling window.
 * The output is a spread multiplier clamp(kRef / k, 1, maxWiden) with a
 * neutral 1.0 while the sample is thin.
 *
 * Pure logic, no WT dependencies — unit-testable.
 */
#pragma once

#include <cmath>
#include <cstdint>
#include <deque>

namespace wt_option {

class TradingIntensity {
public:
    struct Config {
        int      buckets      = 6;       // delta buckets: 0,1,2,3,4,5+ ticks
        int      minSamples   = 30;      // below this -> neutral multiplier
        double   kRef         = 1.0;     // reference decay for multiplier = 1
        double   maxWiden     = 3.0;     // cap on the spread multiplier
        uint32_t windowFills  = 300;     // rolling window size (fills)
    };

    TradingIntensity() : _cfg() {}
    explicit TradingIntensity(const Config& cfg) : _cfg(cfg) {}

    /// Record one fill. deltaTicks = |fillPx - mid| / tick (>=0).
    void onFill(double deltaTicks) {
        if (!(deltaTicks >= 0)) return;
        int b = static_cast<int>(deltaTicks + 0.5);
        if (b >= _cfg.buckets) b = _cfg.buckets - 1;
        _window.push_back(b);
        if (_window.size() > _cfg.windowFills) _window.pop_front();
    }

    /// Least-squares slope of log(count+1) over bucket index (decay k).
    /// Returns NEGATIVE when no slope can be fitted (thin sample or a single
    /// occupied bucket — e.g. all fills at the touch, the least toxic case);
    /// returns 0 for a FLAT/positive slope (fills at every distance — the
    /// most toxic case); >0 is the decay rate.
    double estimateK() const {
        if ((int)_window.size() < _cfg.minSamples) return -1.0;
        double cnt[8] = {0};
        for (int b : _window) ++cnt[b];
        // fit y = a - k*x over x=0..buckets-1, y=log(cnt+1)
        const int n = _cfg.buckets;
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        int used = 0;
        for (int i = 0; i < n; ++i) {
            if (cnt[i] <= 0) continue;      // empty bucket carries no slope info
            const double y = std::log(cnt[i] + 1.0);
            sx += i; sy += y; sxx += (double)i * i; sxy += i * y;
            ++used;
        }
        if (used < 2) return -1.0;
        const double denom = used * sxx - sx * sx;
        if (denom <= 1e-9) return -1.0;
        const double slope = (used * sxy - sx * sy) / denom;   // negative = decay
        return slope < 0 ? -slope : 0.0;
    }

    /// Spread multiplier: k below kRef (slow decay / toxic) widens the spread,
    /// never narrows below 1. Unfittable/thin -> neutral; FLAT slope -> maxWiden.
    double spreadMultiplier() const {
        const double k = estimateK();
        if (k < 0.0) return 1.0;                     // thin/single-bucket: neutral
        if (k <= 1e-6) return _cfg.maxWiden;         // flat = maximally toxic
        const double m = _cfg.kRef / k;
        if (m < 1.0) return 1.0;
        return m > _cfg.maxWiden ? _cfg.maxWiden : m;
    }

    size_t sampleCount() const { return _window.size(); }
    void reset() { _window.clear(); }

private:
    Config _cfg;
    std::deque<int> _window;
};

} // namespace wt_option
