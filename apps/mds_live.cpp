// SPDX-License-Identifier: MIT
//
// mds_live - real-time market simulation with live paper trading.
//
//   mds_live --rate 2000 --speed 1 --strategy mm --snapshot-ms 250
//
// Unlike mds_sim (which replays as fast as possible to measure throughput),
// this paces the synthetic exchange to the WALL CLOCK:
//
//   [feed thread]  generates events, sleeps until each event's time,
//                  stamps it and pushes it into the lock-free SPSC ring
//   [main thread]  pops, rebuilds the order book, runs a strategy on it
//                  (paper trading through the backtester's fill model) and
//                  every --snapshot-ms prints one JSON line to stdout
//
// The web backend reads those JSON Lines and streams them to browsers. Because
// events now arrive at their natural pace, the reported latency is genuine
// per-message hand-off latency (enqueue -> book updated), not queue backlog.
//
// Output records (one JSON object per line):
//   {"type":"hello", ...config}             once at start
//   {"type":"tick",  ...}                   every snapshot interval
//   {"type":"bye",   ...summary}            on SIGINT/SIGTERM or --duration-s
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <exception>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "mds/backtest/backtester.hpp"
#include "mds/common/cli.hpp"
#include "mds/common/clock.hpp"
#include "mds/common/json.hpp"
#include "mds/common/stats.hpp"
#include "mds/feed/synthetic_feed.hpp"
#include "mds/pipeline/pipeline.hpp"
#include "mds/pipeline/spsc_ring.hpp"
#include "mds/strategies/strategies.hpp"

using namespace mds;

namespace {

std::atomic<bool> g_stop{false};
extern "C" void on_signal(int) { g_stop.store(true); }

// Watches the market without trading (strategy "none").
class Observer final : public Strategy {
public:
    std::string name() const override { return "none"; }
    void on_market_data(StrategyContext&, const OrderBook&, const MarketEvent&) override {}
};

void usage() {
    std::puts(
        "usage: mds_live [options]\n"
        "  --rate R           events per simulated second (default 2000)\n"
        "  --speed X          simulated seconds per wall second (default 1)\n"
        "  --strategy S       mm | obi | none (default mm)\n"
        "  --snapshot-ms M    output interval in wall milliseconds (default 250)\n"
        "  --levels L         book levels per side in each snapshot (default 10)\n"
        "  --seed S           feed seed (default: time-based)\n"
        "  --vol V --informed F --impact I   market realism knobs\n"
        "  --latency-us U     strategy order latency (default 50)\n"
        "  --duration-s D     stop after D wall seconds (default 0 = run until signalled)\n"
        "  --capacity C       SPSC ring capacity (default 4096)");
}

struct TradePrint {
    Timestamp ts;
    Price price;
    Qty qty;
    Side aggressor;
};

void write_levels(JsonWriter& j, const std::vector<LevelView>& levels) {
    j.begin_array();
    for (const auto& l : levels) {
        j.begin_array();
        j.value(to_double(l.price), 4);
        j.value(static_cast<std::int64_t>(l.qty));
        j.value(static_cast<std::int64_t>(l.orders));
        j.end_array();
    }
    j.end_array();
}

}  // namespace

int main(int argc, char** argv) try {
    CliArgs args(argc, argv);
    if (args.has("help")) {
        usage();
        return 0;
    }
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    SyntheticFeedConfig fc;
    fc.seed = static_cast<std::uint64_t>(
        args.i64("seed", std::chrono::system_clock::now().time_since_epoch().count()));
    fc.events_per_second = std::clamp(args.f64("rate", 2000.0), 1.0, 200'000.0);
    fc.volatility_ticks_per_sqrt_sec = args.f64("vol", fc.volatility_ticks_per_sqrt_sec);
    fc.informed_fraction = args.f64("informed", fc.informed_fraction);
    fc.impact_ticks_per_lot = args.f64("impact", fc.impact_ticks_per_lot);
    // Start the simulated clock at "now" so the UI shows a sensible time.
    fc.start_ts = std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();

    const double speed = std::clamp(args.f64("speed", 1.0), 0.01, 1000.0);
    const auto snapshot_ns =
        std::max<std::int64_t>(50, args.i64("snapshot-ms", 250)) * kNanosPerMilli;
    const auto levels = static_cast<std::size_t>(std::clamp<std::int64_t>(args.i64("levels", 10), 1, 50));
    const double duration_s = args.f64("duration-s", 0.0);
    const std::string strategy_name = args.str("strategy", "mm");

    BacktestConfig bc;
    bc.order_latency_ns = bc.cancel_latency_ns =
        static_cast<Timestamp>(args.f64("latency-us", 50.0) * kNanosPerMicro);
    bc.record_history = false;  // runs indefinitely: bounded memory
    bc.warmup_events = 500;
    std::unique_ptr<Strategy> strategy =
        strategy_name == "none" ? std::make_unique<Observer>() : make_strategy(strategy_name);

    SpscRing<Envelope> ring(static_cast<std::size_t>(args.i64("capacity", 4096)));
    std::atomic<std::uint64_t> producer_stalls{0};

    JsonWriter out(stdout);
    out.begin_object()
        .kv("type", "hello")
        .kv("seed", static_cast<std::uint64_t>(fc.seed))
        .kv("rate", fc.events_per_second, 1)
        .kv("speed", speed, 3)
        .kv("strategy", strategy->name())
        .kv("snapshot_ms", static_cast<std::int64_t>(snapshot_ns / kNanosPerMilli))
        .kv("levels", static_cast<std::int64_t>(levels))
        .kv("vol", fc.volatility_ticks_per_sqrt_sec, 3)
        .kv("informed", fc.informed_fraction, 3)
        .kv("impact", fc.impact_ticks_per_lot, 3)
        .kv("queue", SpscRing<Envelope>::name())
        .kv("capacity", static_cast<std::int64_t>(ring.capacity()))
        .end_object();
    out.end_line();

    // ---- producer: wall-clock-paced exchange feed ---------------------------
    std::thread producer([&] {
        SyntheticFeed feed(fc);
        Envelope env;
        const auto wall0 = std::chrono::steady_clock::now();
        Backoff backoff;
        while (!g_stop.load(std::memory_order_relaxed) && feed.next(env.event)) {
            const auto due = wall0 + std::chrono::nanoseconds(static_cast<std::int64_t>(
                                         static_cast<double>(env.event.ts - fc.start_ts) / speed));
            if (due > std::chrono::steady_clock::now() + std::chrono::microseconds(200)) {
                std::this_thread::sleep_until(due);
            }
            env.enqueue_ns = mono_ns();
            while (!ring.try_push(env)) {
                producer_stalls.fetch_add(1, std::memory_order_relaxed);
                backoff.pause();
                if (g_stop.load(std::memory_order_relaxed)) return;
            }
            backoff.reset();
        }
    });

    // ---- consumer: book + strategy + snapshots ------------------------------
    Backtester bt(bc);
    bt.begin(*strategy);

    std::vector<Envelope> batch(256);
    std::vector<std::int64_t> latencies;
    latencies.reserve(1 << 14);
    std::vector<TradePrint> trades;
    std::vector<LevelView> bids, asks;
    std::uint64_t events_total = 0, events_window = 0, seq = 0, trades_total = 0;
    Qty volume_window = 0, volume_total = 0;
    std::uint32_t idle_spins = 0;

    const auto wall_start = mono_ns();
    auto next_snapshot = wall_start + snapshot_ns;
    auto window_start = wall_start;

    while (!g_stop.load(std::memory_order_relaxed)) {
        const std::size_t n = ring.try_pop_n(batch.data(), batch.size());
        if (n == 0) {
            // Paced input means long idle gaps: spin briefly for low latency,
            // then sleep so an idle market doesn't burn a whole core.
            if (++idle_spins < 64) {
                cpu_relax();
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        } else {
            idle_spins = 0;
            for (std::size_t i = 0; i < n; ++i) {
                bt.step(batch[i].event);
                latencies.push_back(mono_ns() - batch[i].enqueue_ns);
                for (const Fill& f : bt.last_market_fills()) {
                    trades.push_back(TradePrint{f.ts, f.price, f.qty, f.aggressor});
                    volume_window += f.qty;
                    ++trades_total;
                }
            }
            events_total += n;
            events_window += n;
        }

        const auto now = mono_ns();
        if (now < next_snapshot) continue;

        // ---- emit one snapshot ------------------------------------------------
        const double window_s = static_cast<double>(now - window_start) / 1e9;
        const Percentiles lat = summarize(latencies);
        const OrderBook& book = bt.book();
        book.depth_into(Side::Buy, levels, bids);
        book.depth_into(Side::Sell, levels, asks);
        const Portfolio& pf = bt.portfolio();
        const double mark = bt.mark();
        volume_total += volume_window;
        const auto strat_fills = bt.drain_recent_fills();

        out.begin_object().kv("type", "tick").kv("seq", ++seq);
        out.kv("wall_s", static_cast<double>(now - wall_start) / 1e9, 3);
        out.kv("sim_ts", static_cast<std::int64_t>(book.last_update()));
        out.kv("events", events_total);
        out.kv("events_per_sec", window_s > 0 ? static_cast<double>(events_window) / window_s : 0.0, 1);

        out.key("book").begin_object();
        out.key("bids");
        write_levels(out, bids);
        out.key("asks");
        write_levels(out, asks);
        if (auto bb = book.best_bid()) out.kv("best_bid", to_double(*bb), 4);
        if (auto ba = book.best_ask()) out.kv("best_ask", to_double(*ba), 4);
        if (auto mid = book.mid_price()) out.kv("mid", *mid, 4);
        if (auto sp = book.spread()) out.kv("spread", to_double(*sp), 4);
        out.kv("imbalance", book.imbalance(5), 4);
        out.kv("orders", static_cast<std::uint64_t>(book.order_count()));
        out.end_object();

        // Most recent prints only: the tape on screen shows a few dozen rows.
        out.key("trades").begin_array();
        const std::size_t first = trades.size() > 40 ? trades.size() - 40 : 0;
        for (std::size_t i = first; i < trades.size(); ++i) {
            const auto& t = trades[i];
            out.begin_array()
                .value(static_cast<std::int64_t>(t.ts))
                .value(to_double(t.price), 4)
                .value(static_cast<std::int64_t>(t.qty))
                .value(t.aggressor == Side::Buy ? "B" : "S")
                .end_array();
        }
        out.end_array();
        out.kv("trade_count", static_cast<std::uint64_t>(trades.size()));
        out.kv("volume", static_cast<std::int64_t>(volume_window));
        out.kv("trades_total", trades_total);
        out.kv("volume_total", static_cast<std::int64_t>(volume_total));

        out.key("latency_us").begin_object();
        out.kv("p50", lat.p50 / 1e3, 2).kv("p99", lat.p99 / 1e3, 2).kv("max", lat.max / 1e3, 2);
        out.kv("samples", static_cast<std::uint64_t>(lat.samples));
        out.end_object();
        out.key("queue").begin_object();
        out.kv("depth", static_cast<std::uint64_t>(ring.size_approx()));
        out.kv("capacity", static_cast<std::uint64_t>(ring.capacity()));
        out.kv("producer_stalls", producer_stalls.load(std::memory_order_relaxed));
        out.end_object();

        out.key("strategy").begin_object();
        out.kv("name", strategy->name());
        out.kv("trading", bt.trading());
        out.kv("position", static_cast<std::int64_t>(pf.position()));
        out.kv("pnl", pf.equity(mark), 2);
        out.kv("realized", pf.realized(), 2);
        out.kv("unrealized", pf.unrealized(mark), 2);
        out.kv("fees", pf.fees(), 2);
        out.kv("fills_total", static_cast<std::uint64_t>(pf.fills()));
        out.kv("volume_total", static_cast<std::int64_t>(pf.volume()));
        out.kv("orders_sent", bt.orders_sent());
        out.kv("working_orders", static_cast<std::uint64_t>(bt.working_orders()));
        out.key("fills").begin_array();
        for (const auto& f : strat_fills) {
            out.begin_array()
                .value(static_cast<std::int64_t>(f.ts))
                .value(f.side == Side::Buy ? "B" : "S")
                .value(to_double(f.price), 4)
                .value(static_cast<std::int64_t>(f.qty))
                .value(f.maker)
                .end_array();
        }
        out.end_array();
        out.end_object();

        out.end_object();
        out.end_line();

        latencies.clear();
        trades.clear();
        events_window = 0;
        volume_window = 0;
        window_start = now;
        next_snapshot = now + snapshot_ns;
        if (duration_s > 0 && static_cast<double>(now - wall_start) / 1e9 >= duration_s) {
            g_stop.store(true);
        }
    }

    g_stop.store(true);
    producer.join();
    const BacktestResult res = bt.finish();
    out.begin_object()
        .kv("type", "bye")
        .kv("events", events_total)
        .kv("wall_s", static_cast<double>(mono_ns() - wall_start) / 1e9, 3)
        .kv("pnl", res.metrics.realized_pnl + res.metrics.unrealized_pnl - res.metrics.fees, 2)
        .kv("fills", static_cast<std::uint64_t>(res.metrics.fills))
        .end_object();
    out.end_line();
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "mds_live: %s\n", e.what());
    return 1;
}
