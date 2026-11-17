// SPDX-License-Identifier: MIT
//
// Small statistics helpers used by benchmarks, the pipeline and the
// backtester: an online mean/variance accumulator (Welford) and a percentile
// summary over a sample buffer.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mds {

// Welford's numerically stable single-pass mean / variance.
class RunningStats {
public:
    void add(double x) noexcept {
        ++n_;
        const double delta = x - mean_;
        mean_ += delta / static_cast<double>(n_);
        m2_ += delta * (x - mean_);
        min_ = n_ == 1 ? x : std::min(min_, x);
        max_ = n_ == 1 ? x : std::max(max_, x);
    }
    [[nodiscard]] std::size_t count() const noexcept { return n_; }
    [[nodiscard]] double mean() const noexcept { return mean_; }
    [[nodiscard]] double variance() const noexcept {
        return n_ > 1 ? m2_ / static_cast<double>(n_ - 1) : 0.0;
    }
    [[nodiscard]] double stddev() const noexcept { return std::sqrt(variance()); }
    [[nodiscard]] double min() const noexcept { return min_; }
    [[nodiscard]] double max() const noexcept { return max_; }

private:
    std::size_t n_{0};
    double mean_{0.0};
    double m2_{0.0};
    double min_{0.0};
    double max_{0.0};
};

struct Percentiles {
    double p50{0}, p90{0}, p99{0}, p999{0}, max{0}, mean{0};
    std::size_t samples{0};
};

// Sorts `samples` in place (callers pass a scratch copy).
inline Percentiles summarize(std::vector<std::int64_t>& samples) {
    Percentiles p;
    p.samples = samples.size();
    if (samples.empty()) return p;
    std::sort(samples.begin(), samples.end());
    auto at = [&](double q) {
        const auto idx = static_cast<std::size_t>(q * static_cast<double>(samples.size() - 1));
        return static_cast<double>(samples[idx]);
    };
    double sum = 0;
    for (auto s : samples) sum += static_cast<double>(s);
    p.p50 = at(0.50);
    p.p90 = at(0.90);
    p.p99 = at(0.99);
    p.p999 = at(0.999);
    p.max = static_cast<double>(samples.back());
    p.mean = sum / static_cast<double>(samples.size());
    return p;
}

}  // namespace mds
