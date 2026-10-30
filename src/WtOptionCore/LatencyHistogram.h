#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace wt_option {

class LatencyHistogram {
public:
    void add(double valueUs) {
        if (std::isfinite(valueUs) && valueUs >= 0) m_values.push_back(valueUs);
    }

    void clear() { m_values.clear(); }
    size_t count() const { return m_values.size(); }

    double percentile(double p) const {
        if (m_values.empty()) return 0.0;
        std::vector<double> sorted = m_values;
        std::sort(sorted.begin(), sorted.end());
        const size_t idx = static_cast<size_t>(
            std::ceil((p / 100.0) * sorted.size())) - 1;
        return sorted[std::min(idx, sorted.size() - 1)];
    }

    double p50() const { return percentile(50.0); }
    double p95() const { return percentile(95.0); }
    double p99() const { return percentile(99.0); }
    double p999() const { return percentile(99.9); }
    double max() const { return percentile(100.0); }

private:
    std::vector<double> m_values;
};

} // namespace wt_option
