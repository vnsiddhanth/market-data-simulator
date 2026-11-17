// SPDX-License-Identifier: MIT
//
// Event-driven backtester with a queue-position-aware fill simulator.
//
// For every historical MarketEvent the engine:
//   1. releases strategy actions whose order-entry latency has elapsed
//      (new orders reach the "exchange", cancels take effect);
//   2. walks the event through the strategy's resting orders BEFORE it is
//      applied, updating FIFO queue position and detecting fills (see below);
//   3. applies the event to the reconstructed OrderBook;
//   4. calls Strategy::on_market_data;
//   5. samples mark-to-market equity on a fixed simulated-time grid.
//
// Fill model (documented in docs/BACKTESTING.md):
//   * Strategy orders are "virtual": they do not enter the historical book, so
//     the replayed flow is unchanged (no market-impact model).
//   * A resting order joins the BACK of its price level: queue_ahead =
//     displayed quantity at that price when it arrives.
//   * queue_ahead shrinks when (a) trades print at our price, (b) an order
//     that was ahead of us cancels, or (c) such an order sizes down/up.
//     Because the reconstructed book knows each order's arrival time we can
//     tell exactly which cancels were ahead of us - no probabilistic guess.
//   * Every aggressive event is walked through the queue before it is applied:
//     better-priced displayed liquidity first, then the queue ahead of us,
//     then us. Whatever quantity reaches our order fills it. This covers
//     partial queue consumption, sweeps that trade through our price, and
//     aggressors hitting a quote of ours that improved the displayed best.
//   * Marketable strategy orders take liquidity by walking the displayed
//     depth at arrival time and pay the taker fee.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "mds/backtest/metrics.hpp"
#include "mds/backtest/portfolio.hpp"
#include "mds/backtest/strategy.hpp"
#include "mds/common/event.hpp"
#include "mds/orderbook/order_book.hpp"

namespace mds {

struct BacktestConfig {
    Timestamp order_latency_ns{50 * kNanosPerMicro};   // strategy -> exchange
    Timestamp cancel_latency_ns{50 * kNanosPerMicro};
    double maker_fee_bps{-0.20};  // negative == rebate
    double taker_fee_bps{0.30};
    Price tick_size{100};
    Timestamp equity_sample_interval_ns{kNanosPerSecond};
    std::size_t warmup_events{1000};  // build the book before the strategy trades
    // false for long-running live sessions: fills go to a drainable buffer
    // and no equity curve is accumulated, so memory stays bounded.
    bool record_history{true};
};

struct BacktestResult {
    std::string strategy;
    PerformanceMetrics metrics;
    std::vector<EquityPoint> equity_curve;
    std::vector<ExecutionReport> fills;
    std::uint64_t events{0};
    std::uint64_t orders_sent{0};
    std::uint64_t cancels_sent{0};
    double wall_seconds{0.0};
    double events_per_sec{0.0};
    Timestamp sim_start{0};
    Timestamp sim_end{0};

    [[nodiscard]] std::string to_json() const;
    void write_equity_csv(const std::string& path) const;
    void write_fills_csv(const std::string& path) const;
};

class Backtester final : private StrategyContext {
public:
    explicit Backtester(BacktestConfig cfg = {});

    // Batch replay.
    BacktestResult run(const std::vector<MarketEvent>& events, Strategy& strategy);

    // Incremental API (used by the real-time engine): begin(), then step() per
    // event as it arrives, then finish(). `strategy` must outlive the session.
    void begin(Strategy& strategy);
    void step(const MarketEvent& ev);
    BacktestResult finish();

    [[nodiscard]] const OrderBook& book() const noexcept { return book_; }
    [[nodiscard]] const Portfolio& portfolio() const noexcept { return portfolio_; }
    [[nodiscard]] bool trading() const noexcept { return trading_enabled_; }
    [[nodiscard]] std::size_t working_orders() const noexcept { return open_.size(); }
    [[nodiscard]] std::uint64_t orders_sent() const noexcept { return result_.orders_sent; }
    [[nodiscard]] double mark() const noexcept;  // current mid (or last known)
    // Market trades produced by the most recent step() (the replayed flow, not ours).
    [[nodiscard]] const std::vector<Fill>& last_market_fills() const noexcept {
        return scratch_fills_;
    }
    // Fills since the last call (only populated when record_history == false).
    std::vector<ExecutionReport> drain_recent_fills();

private:
    enum class OrderState : std::uint8_t { PendingNew, Working, Done };

    struct SimOrder {
        OrderId id{0};
        Side side{Side::Buy};
        Price price{kNoPrice};  // kNoPrice == market order
        Qty qty{0};
        Qty remaining{0};
        Qty queue_ahead{0};
        Timestamp arrive_ts{0};   // when it reaches the exchange
        std::uint64_t joined_seq{0};  // book sequence when it joined; smaller seq == ahead of us
        Timestamp cancel_ts{0};   // 0 == no cancel in flight
        OrderState state{OrderState::PendingNew};
    };

    // StrategyContext
    OrderId send_limit(Side side, Price price, Qty qty) override;
    OrderId send_market(Side side, Qty qty) override;
    void cancel(OrderId id) override;
    void cancel_all() override;
    [[nodiscard]] Qty position() const override { return portfolio_.position(); }
    [[nodiscard]] double cash() const override { return portfolio_.cash(); }
    [[nodiscard]] Timestamp now() const override { return now_; }
    [[nodiscard]] std::size_t open_orders() const override { return open_.size(); }
    [[nodiscard]] bool is_open(OrderId id) const override { return open_.count(id) != 0; }
    [[nodiscard]] Price tick_size() const override { return cfg_.tick_size; }

    void release_due_actions(Strategy& strategy);
    void on_order_arrival(SimOrder& o, Strategy& strategy);
    void pre_event_queue_update(const MarketEvent& ev, Strategy& strategy);
    void fill(SimOrder& o, Price price, Qty qty, bool maker, Strategy& strategy);
    void sample_equity(Timestamp ts);
    void sweep_done();
    void reset();

    BacktestConfig cfg_;
    OrderBook book_;
    Portfolio portfolio_;
    std::unordered_map<OrderId, SimOrder> open_;  // pending + working
    std::vector<OrderId> scratch_ids_;
    std::vector<Fill> scratch_fills_;
    OrderId next_id_{1ULL << 62};  // disjoint from exchange ids
    Timestamp now_{0};
    Timestamp next_sample_ts_{0};
    double last_mid_{0.0};
    bool trading_enabled_{false};
    BacktestResult result_;
    Strategy* strategy_{nullptr};
    std::vector<ExecutionReport> recent_fills_;
    std::int64_t wall_start_ns_{0};
};

}  // namespace mds
