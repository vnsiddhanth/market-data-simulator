// SPDX-License-Identifier: MIT
#include <vector>

#include "mds/backtest/backtester.hpp"
#include "mds/backtest/metrics.hpp"
#include "mds/backtest/portfolio.hpp"
#include "mds/feed/synthetic_feed.hpp"
#include "mds/strategies/strategies.hpp"
#include "test_framework.hpp"

using namespace mds;

namespace {
constexpr Price px(double p) { return static_cast<Price>(p * kPriceScale + 0.5); }
}  // namespace

// ---- portfolio accounting ------------------------------------------------------

TEST(portfolio, round_trip_realizes_pnl) {
    Portfolio p;
    p.on_fill(Side::Buy, px(100), 100, 0.0);
    p.on_fill(Side::Buy, px(102), 100, 0.0);  // avg cost 101
    CHECK_NEAR(p.avg_cost(), 101.0, 1e-9);
    p.on_fill(Side::Sell, px(103), 150, 0.0);  // realize 150 * 2
    CHECK_NEAR(p.realized(), 300.0, 1e-9);
    CHECK_EQ(p.position(), 50);
    CHECK_NEAR(p.unrealized(105.0), 200.0, 1e-9);
    CHECK_NEAR(p.equity(105.0), 500.0, 1e-9);  // realized + unrealized
}

TEST(portfolio, flip_through_zero_resets_cost) {
    Portfolio p;
    p.on_fill(Side::Buy, px(100), 100, 0.0);
    p.on_fill(Side::Sell, px(99), 300, 0.0);  // close 100 @ -1, open short 200 @ 99
    CHECK_NEAR(p.realized(), -100.0, 1e-9);
    CHECK_EQ(p.position(), -200);
    CHECK_NEAR(p.avg_cost(), 99.0, 1e-9);
    p.on_fill(Side::Buy, px(97), 200, 0.0);
    CHECK_NEAR(p.realized(), 300.0, 1e-9);
    CHECK_EQ(p.position(), 0);
}

TEST(portfolio, fees_reduce_cash) {
    Portfolio p;
    p.on_fill(Side::Buy, px(100), 10, 0.5);
    p.on_fill(Side::Sell, px(100), 10, -0.2);  // rebate
    CHECK_NEAR(p.cash(), -0.3, 1e-9);
    CHECK_NEAR(p.fees(), 0.3, 1e-9);
}

// ---- metrics ----------------------------------------------------------------------

TEST(metrics, drawdown_and_sharpe) {
    std::vector<EquityPoint> c;
    const double eq[] = {0, 10, 5, 15, 2, 20};
    for (int i = 0; i < 6; ++i) c.push_back({i * kNanosPerSecond, eq[i], 100.0, 0});
    double dur = 0;
    CHECK_NEAR(max_drawdown(c, &dur), 13.0, 1e-12);
    CHECK_NEAR(dur, 1.0, 1e-12);
    const auto m = compute_metrics(c, 1.0);
    CHECK_NEAR(m.total_pnl, 20.0, 1e-12);
    CHECK(m.sharpe_annualized > 0);
    CHECK_NEAR(m.win_rate, 0.6, 1e-12);
}

// ---- fill simulator (scripted market) ------------------------------------------------

namespace {

// Places a single passive buy at a fixed price on start and records fills.
class OneShotBid final : public Strategy {
public:
    OneShotBid(Price p, Qty q) : price_(p), qty_(q) {}
    std::string name() const override { return "one_shot_bid"; }
    void on_start(StrategyContext& ctx) override { id_ = ctx.send_limit(Side::Buy, price_, qty_); }
    void on_market_data(StrategyContext&, const OrderBook&, const MarketEvent&) override {}
    void on_fill(StrategyContext&, const ExecutionReport& r) override { filled_ += r.qty; }
    Qty filled() const { return filled_; }

private:
    Price price_;
    Qty qty_;
    OrderId id_{0};
    Qty filled_{0};
};

BacktestConfig scripted_config() {
    BacktestConfig c;
    c.order_latency_ns = 10;
    c.cancel_latency_ns = 10;
    c.warmup_events = 2;
    c.maker_fee_bps = 0;
    c.taker_fee_bps = 0;
    return c;
}

}  // namespace

TEST(backtester, queue_position_respected) {
    // Book: 300 resting ahead of us at 100.00. Our 100 joins behind them.
    std::vector<MarketEvent> ev = {
        MarketEvent::add(0, 1, Side::Buy, px(100), 300),
        MarketEvent::add(1, 2, Side::Sell, px(100.05), 1000),
        MarketEvent::add(100, 3, Side::Sell, px(101), 1),         // strategy starts; bid arrives at 110
        MarketEvent::market(200, 4, Side::Sell, 250),             // eats 250 of the 300 ahead
        MarketEvent::market(300, 5, Side::Sell, 100),             // 50 ahead + 50 for us
    };
    OneShotBid s(px(100), 100);
    Backtester bt(scripted_config());
    const auto r = bt.run(ev, s);
    CHECK_EQ(s.filled(), 50);
    CHECK_EQ(r.metrics.volume, 50);
}

TEST(backtester, cancel_ahead_advances_queue) {
    std::vector<MarketEvent> ev = {
        MarketEvent::add(0, 1, Side::Buy, px(100), 300),
        MarketEvent::add(1, 2, Side::Sell, px(100.05), 1000),
        MarketEvent::add(100, 3, Side::Sell, px(101), 1),  // strategy starts; bid joins behind #1
        MarketEvent::cancel(150, 1),                       // the 300 ahead of us cancel
        MarketEvent::market(200, 4, Side::Sell, 80),       // nothing left ahead: fills us
    };
    OneShotBid s(px(100), 100);
    Backtester bt(scripted_config());
    bt.run(ev, s);
    // #1 is gone from the real book, so the market order finds no displayed bid
    // at all - but our simulated bid is there and the aggressor hits it.
    CHECK_EQ(s.filled(), 80);
}

TEST(backtester, partial_queue_then_exhausted_liquidity) {
    // Same as queue_position_respected but checks the queue never goes negative
    // and further flow keeps filling the remainder.
    std::vector<MarketEvent> ev = {
        MarketEvent::add(0, 1, Side::Buy, px(100), 100),
        MarketEvent::add(1, 2, Side::Sell, px(100.05), 1000),
        MarketEvent::add(100, 3, Side::Sell, px(101), 1),
        MarketEvent::market(200, 4, Side::Sell, 130),  // 100 ahead, 30 for us
        MarketEvent::market(300, 5, Side::Sell, 30),   // 30 more for us
        MarketEvent::market(400, 6, Side::Sell, 500),  // remaining 40
    };
    OneShotBid s(px(100), 100);
    Backtester bt(scripted_config());
    bt.run(ev, s);
    CHECK_EQ(s.filled(), 100);
}

TEST(backtester, trade_through_fills_fully) {
    std::vector<MarketEvent> ev = {
        MarketEvent::add(0, 1, Side::Buy, px(100), 500),
        MarketEvent::add(1, 2, Side::Buy, px(99), 500),
        MarketEvent::add(2, 3, Side::Sell, px(101), 10),
        MarketEvent::add(100, 4, Side::Sell, px(102), 1),
        MarketEvent::market(200, 5, Side::Sell, 600),  // sweeps 100 and trades at 99
    };
    OneShotBid s(px(100), 100);
    Backtester bt(scripted_config());
    bt.run(ev, s);
    CHECK_EQ(s.filled(), 100);
}

TEST(backtester, latency_delays_order_arrival) {
    std::vector<MarketEvent> ev = {
        MarketEvent::add(0, 1, Side::Buy, px(100), 100),
        MarketEvent::add(1, 2, Side::Sell, px(100.05), 1000),
        MarketEvent::add(2, 3, Side::Sell, px(101), 1),     // strategy starts here, sends bid
        MarketEvent::market(5, 4, Side::Sell, 500),         // before arrival (2 + 1000)
    };
    BacktestConfig c = scripted_config();
    c.order_latency_ns = 1000;
    OneShotBid s(px(100.01), 100);
    Backtester bt(c);
    bt.run(ev, s);
    CHECK_EQ(s.filled(), 0);  // the order was still in flight
}

TEST(backtester, strategies_run_end_to_end) {
    SyntheticFeedConfig fc;
    fc.seed = 21;
    const auto events = SyntheticFeed(fc).generate(150'000);
    for (const char* name : {"mm", "obi"}) {
        auto strat = make_strategy(name);
        Backtester bt;
        const auto r = bt.run(events, *strat);
        CHECK_EQ(r.events, events.size());
        CHECK(r.orders_sent > 0);
        CHECK(r.metrics.fills > 0);
        CHECK(r.equity_curve.size() > 2);
        CHECK(r.events_per_sec > 10'000);  // headline requirement, with lots of margin
        // Accounting identity: equity change == realized + unrealized - fees.
        CHECK_NEAR(r.metrics.total_pnl,
                   r.metrics.realized_pnl + r.metrics.unrealized_pnl - r.metrics.fees, 1e-4);
    }
}
