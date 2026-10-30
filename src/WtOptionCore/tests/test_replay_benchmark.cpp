// Lightweight replay-style latency harness.
// This is not an exchange feed simulator; it measures the local tick→quote
// update loop over a synthetic option chain and reports latency percentiles.
#include "../LatencyHistogram.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

struct SyntheticQuote {
    double bid = 0;
    double ask = 0;
    double theo = 0;
};

int main(int argc, char** argv) {
    const int numOptions = argc > 1 ? std::atoi(argv[1]) : 1000;
    const int ticksPerOption = argc > 2 ? std::atoi(argv[2]) : 100;
    std::vector<SyntheticQuote> quotes(static_cast<size_t>(numOptions));

    wt_option::LatencyHistogram hist;
    volatile double sink = 0.0;

    for (int t = 0; t < ticksPerOption; ++t) {
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < numOptions; ++i) {
            auto& q = quotes[static_cast<size_t>(i)];
            q.bid = 100.0 + 0.01 * ((i + t) % 10);
            q.ask = q.bid + 0.02;
            q.theo = (q.bid + q.ask) * 0.5 + 0.001 * ((i * t) % 3);
            sink += q.theo;   // keep the loop observable
        }
        const auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count();
        hist.add(static_cast<double>(elapsedUs));
    }

    std::cout << "REPLAY_BENCH options=" << numOptions
              << " ticks=" << ticksPerOption
              << " samples=" << hist.count()
              << " p50_us=" << hist.p50()
              << " p95_us=" << hist.p95()
              << " p99_us=" << hist.p99()
              << " p999_us=" << hist.p999()
              << " max_us=" << hist.max()
              << " sink=" << sink << std::endl;
    return 0;
}
