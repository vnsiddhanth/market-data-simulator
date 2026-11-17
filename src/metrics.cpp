// SPDX-License-Identifier: MIT
#include "mds/backtest/metrics.hpp"

#include <algorithm>
#include <cmath>

#include "mds/common/stats.hpp"

namespace mds {

double max_drawdown(const std::vector<EquityPoint>& curve, double* duration_s) {
    double peak = curve.empty() ? 0.0 : curve.front().equity;
    Timestamp peak_ts = curve.empty() ? 0 : curve.front().ts;
    double mdd = 0.0;
    Timestamp longest = 0;
    for (const auto& p : curve) {
        if (p.equity >= peak) {
            peak = p.equity;
            peak_ts = p.ts;
        } else {
            mdd = std::max(mdd, peak - p.equity);
            longest = std::max(longest, p.ts - peak_ts);
        }
    }
    if (duration_s) *duration_s = static_cast<double>(longest) / 1e9;
    return mdd;
}

PerformanceMetrics compute_metrics(const std::vector<EquityPoint>& curve, double sample_interval_s) {
    PerformanceMetrics m;
    m.samples = curve.size();
    if (curve.size() < 2 || sample_interval_s <= 0) return m;

    RunningStats all;
    RunningStats downside;
    std::size_t wins = 0;
    std::size_t nonzero = 0;
    for (std::size_t i = 1; i < curve.size(); ++i) {
        const double d = curve[i].equity - curve[i - 1].equity;
        all.add(d);
        downside.add(std::min(d, 0.0));
        if (d != 0.0) {
            ++nonzero;
            if (d > 0.0) ++wins;
        }
    }

    constexpr double kTradingSecondsPerYear = 252.0 * 6.5 * 3600.0;
    const double periods_per_year = kTradingSecondsPerYear / sample_interval_s;
    const double ann = std::sqrt(periods_per_year);

    m.sharpe_annualized = all.stddev() > 0 ? all.mean() / all.stddev() * ann : 0.0;
    // Downside deviation: RMS of negative increments (target return 0).
    double dd_sq = downside.variance() * static_cast<double>(downside.count() - 1) /
                       static_cast<double>(downside.count()) +
                   downside.mean() * downside.mean();
    const double dd = std::sqrt(dd_sq);
    m.sortino_annualized = dd > 0 ? all.mean() / dd * ann : 0.0;
    m.win_rate = nonzero ? static_cast<double>(wins) / static_cast<double>(nonzero) : 0.0;
    m.total_pnl = curve.back().equity - curve.front().equity;
    m.max_drawdown = max_drawdown(curve, &m.max_drawdown_duration_s);
    return m;
}

}  // namespace mds
