// SPDX-License-Identifier: MIT
//
// Performance metrics computed from a sampled equity curve.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "mds/common/types.hpp"

namespace mds {

struct EquityPoint {
    Timestamp ts{0};
    double equity{0.0};
    double mid{0.0};
    Qty position{0};
};

struct PerformanceMetrics {
    double total_pnl{0.0};
    double realized_pnl{0.0};
    double unrealized_pnl{0.0};
    double fees{0.0};
    double sharpe_annualized{0.0};
    double sortino_annualized{0.0};
    double max_drawdown{0.0};          // currency units, >= 0
    double max_drawdown_duration_s{0.0};
    double pnl_per_share{0.0};
    double win_rate{0.0};              // share of positive equity-curve intervals
    std::uint64_t fills{0};
    Qty volume{0};
    Qty max_abs_position{0};
    std::size_t samples{0};
};

// Sharpe / Sortino use per-interval PnL increments (the strategy has no
// committed capital, so "returns" are PnL deltas) annualised with
// sqrt(intervals per year), assuming 252 trading days of 6.5 hours.
PerformanceMetrics compute_metrics(const std::vector<EquityPoint>& curve, double sample_interval_s);

double max_drawdown(const std::vector<EquityPoint>& curve, double* duration_s = nullptr);

}  // namespace mds
