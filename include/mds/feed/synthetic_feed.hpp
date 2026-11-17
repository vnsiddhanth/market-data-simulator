// SPDX-License-Identifier: MIT
//
// Synthetic exchange feed.
//
// Generates a realistic, fully deterministic (seeded) order-flow stream:
//
//   * Arrivals follow a Poisson process (exponential inter-arrival times) at
//     `events_per_second`, giving bursty, irregular timestamps.
//   * A latent "fair value" follows an arithmetic random walk scaled by
//     sqrt(dt) (discretised Brownian motion), so price moves are proportional
//     to elapsed time, not to the number of events.
//   * Limit prices are placed a geometrically-distributed number of ticks
//     away from fair value, producing the familiar depth profile where
//     liquidity is concentrated near the touch.
//   * Order sizes are log-normal and rounded to lots (heavy right tail).
//   * As fair value drifts through resting orders, newly-arriving limit
//     orders become marketable and trade - trades emerge from the dynamics
//     rather than being scripted.
//   * A configurable share of aggressive orders is "informed": it trades in
//     the direction of the next fair-value innovation. This is
//     what creates adverse selection for liquidity providers; with purely
//     random takers a market maker would earn the spread risk-free.
//   * Optional permanent price impact: every trade moves fair value in the
//     aggressor's direction (Kyle's lambda). Without it, sweeps are pure
//     noise that the book always reverts, which again makes liquidity
//     provision look risk-free.
//
// The generator runs its own shadow OrderBook ("the exchange") so that it
// only ever cancels or modifies orders that are still live. Consequently a
// downstream book rebuilt from the emitted events must end up identical to
// the shadow book - a strong end-to-end determinism check used in the tests.
#pragma once

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "mds/common/event.hpp"
#include "mds/orderbook/order_book.hpp"

namespace mds {

struct SyntheticFeedConfig {
    std::uint64_t seed{42};
    SymbolId symbol{0};
    double start_price{100.0};       // in currency units
    Price tick_size{100};            // in Price units: 100 == $0.01 at kPriceScale 10'000
    double events_per_second{2'000.0};  // a liquid mid-cap; throughput benchmarks are rate-independent
    double volatility_ticks_per_sqrt_sec{1.5};  // ~30% annualised at $100 / 1c ticks, before impact
    double p_add{0.52};
    double p_cancel{0.38};
    double p_modify{0.04};
    double p_market{0.06};           // remainder after the above is renormalised
    double placement_decay{0.35};    // geometric p for "ticks from fair value"
    double aggressive_fraction{0.03};  // share of adds deliberately priced through
    double informed_fraction{0.6};     // share of aggressors that know the next fair-value move
    double impact_ticks_per_lot{0.15};  // permanent fair-value move per traded lot (aggressor side)
    double lot_size{100.0};
    double log_qty_mean{5.3};        // exp(5.3) ~ 200 shares median
    double log_qty_sigma{0.8};
    std::size_t min_resting_orders{200};  // keep the book from emptying out
    Timestamp start_ts{1'700'000'000LL * kNanosPerSecond};
    std::uint64_t max_events{0};  // 0 == unbounded
};

class SyntheticFeed {
public:
    explicit SyntheticFeed(SyntheticFeedConfig cfg = {});

    // Produces the next event; returns false once max_events is reached.
    bool next(MarketEvent& out);

    // Convenience: generate `n` events eagerly.
    std::vector<MarketEvent> generate(std::size_t n);

    [[nodiscard]] const OrderBook& exchange_book() const noexcept { return book_; }
    [[nodiscard]] std::uint64_t emitted() const noexcept { return emitted_; }
    [[nodiscard]] double fair_value() const noexcept;
    [[nodiscard]] const SyntheticFeedConfig& config() const noexcept { return cfg_; }

private:
    MarketEvent make_add(Timestamp ts);
    bool make_cancel(Timestamp ts, MarketEvent& out);
    bool make_modify(Timestamp ts, MarketEvent& out);
    MarketEvent make_market(Timestamp ts);
    Side taker_side();
    Qty draw_qty();
    bool pick_live_order(OrderId& id);

    SyntheticFeedConfig cfg_;
    std::mt19937_64 rng_;
    std::exponential_distribution<double> inter_arrival_;
    std::normal_distribution<double> gauss_{0.0, 1.0};
    std::uniform_real_distribution<double> uniform_{0.0, 1.0};
    std::geometric_distribution<int> placement_;
    std::lognormal_distribution<double> qty_dist_;

    OrderBook book_;
    std::vector<OrderId> live_ids_;  // superset of live orders; filled ones pruned lazily
    std::vector<Fill> scratch_fills_;
    double fair_ticks_;   // latent fair value, in ticks
    double next_innovation_{0.0};
    bool last_aggressive_{false};  // was the event being generated a deliberate aggressor?  // pre-drawn N(0,1) for the next fair-value step
    double ts_accum_ns_;  // fractional-nanosecond accumulator for timestamps
    OrderId next_id_{1};
    std::uint64_t emitted_{0};
};

// ---- persistence helpers ----------------------------------------------------

// Human-readable CSV: ts,symbol,type,side,id,price,qty   (price in currency units)
void write_csv(const std::string& path, const std::vector<MarketEvent>& events);
std::vector<MarketEvent> read_csv(const std::string& path);

// Compact binary: 8-byte magic "MDSEVT01", uint64 count, then raw 40-byte records.
void write_binary(const std::string& path, const std::vector<MarketEvent>& events);
std::vector<MarketEvent> read_binary(const std::string& path);

}  // namespace mds
