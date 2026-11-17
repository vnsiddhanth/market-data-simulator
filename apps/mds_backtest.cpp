// SPDX-License-Identifier: MIT
//
// mds_backtest - replay market data through a strategy.
//
//   mds_backtest --strategy mm --events 2000000
//   mds_backtest --strategy obi --input ticks.bin --latency-us 100
//   mds_backtest --strategy mm --redis 127.0.0.1:6379 --symbol SIM --report out.json
//
// Data sources (first match wins): --input FILE (.csv or .bin), --redis, synthetic.
#include <cstdio>
#include <exception>
#include <fstream>
#include <string>

#include "mds/backtest/backtester.hpp"
#include "mds/common/cli.hpp"
#include "mds/common/clock.hpp"
#include "mds/feed/synthetic_feed.hpp"
#include "mds/storage/tick_store.hpp"
#include "mds/strategies/strategies.hpp"

using namespace mds;

namespace {

void usage() {
    std::puts(
        "usage: mds_backtest [options]\n"
        "  --strategy S        mm | obi (default mm)\n"
        "  --events N          synthetic events when no input is given (default 1000000)\n"
        "  --seed S            synthetic feed seed (default 7)\n"
        "  --input PATH        load events from .csv or .bin (MDSEVT01)\n"
        "  --redis HOST:PORT   load events from Redis\n"
        "  --symbol SYM        Redis symbol (default SIM)\n"
        "  --from NS --to NS   Redis time window in ns (default: everything)\n"
        "  --latency-us U      order/cancel entry latency (default 50)\n"
        "  --maker-bps B       maker fee in bps, negative = rebate (default -0.2)\n"
        "  --taker-bps B       taker fee in bps (default 0.3)\n"
        "  --sample-ms M       equity sampling interval (default 1000)\n"
        "  --report PATH       write JSON report\n"
        "  --equity PATH       write equity curve CSV\n"
        "  --fills PATH        write fills CSV");
}

// Annualising a Sharpe ratio from a handful of samples multiplies sampling
// noise by sqrt(periods per year); below this we flag the figure.
constexpr std::size_t kMinSamplesForSharpe = 250;

bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

}  // namespace

int main(int argc, char** argv) try {
    CliArgs args(argc, argv);
    if (args.has("help")) {
        usage();
        return 0;
    }

    // ---- load data ---------------------------------------------------------
    std::vector<MarketEvent> events;
    const auto l0 = mono_ns();
    std::string source;
    if (args.has("input")) {
        const std::string path = args.str("input");
        events = ends_with(path, ".csv") ? read_csv(path) : read_binary(path);
        source = path;
    } else if (args.has("redis")) {
        std::string host;
        std::uint16_t port;
        RedisClient::parse_endpoint(args.str("redis"), host, port);
        RedisClient redis;
        redis.connect(host, port);
        TickStoreConfig tc;
        tc.symbol = args.str("symbol", "SIM");
        TickStore store(redis, tc);
        store.init();
        events = store.load_events(args.i64("from", 0), args.i64("to", INT64_MAX));
        source = "redis://" + args.str("redis") + "/" + tc.symbol;
    } else {
        SyntheticFeedConfig fc;
        fc.seed = static_cast<std::uint64_t>(args.i64("seed", 7));
        fc.events_per_second = args.f64("rate", fc.events_per_second);
        fc.volatility_ticks_per_sqrt_sec = args.f64("vol", fc.volatility_ticks_per_sqrt_sec);
        fc.informed_fraction = args.f64("informed", fc.informed_fraction);
        fc.impact_ticks_per_lot = args.f64("impact", fc.impact_ticks_per_lot);
        SyntheticFeed feed(fc);
        events = feed.generate(static_cast<std::size_t>(args.i64("events", 1'000'000)));
        source = "synthetic(seed=" + std::to_string(fc.seed) + ")";
    }
    const double load_s = static_cast<double>(mono_ns() - l0) / 1e9;
    std::printf("[data] %zu events from %s in %.3f s\n", events.size(), source.c_str(), load_s);
    if (events.empty()) {
        std::fprintf(stderr, "no events to backtest\n");
        return 1;
    }

    // ---- run -----------------------------------------------------------------
    BacktestConfig bc;
    bc.order_latency_ns = bc.cancel_latency_ns =
        static_cast<Timestamp>(args.f64("latency-us", 50.0) * kNanosPerMicro);
    bc.maker_fee_bps = args.f64("maker-bps", -0.2);
    bc.taker_fee_bps = args.f64("taker-bps", 0.3);
    bc.equity_sample_interval_ns = args.i64("sample-ms", 1000) * kNanosPerMilli;

    auto strategy = make_strategy(args.str("strategy", "mm"));
    Backtester bt(bc);
    const BacktestResult res = bt.run(events, *strategy);
    const auto& m = res.metrics;

    std::printf(
        "[backtest] strategy=%s  %llu events in %.3f s  -> %.0f events/s\n"
        "  simulated span     %.1f s\n"
        "  orders / cancels   %llu / %llu\n"
        "  fills / volume     %llu / %lld   max |pos| %lld\n"
        "  total PnL          %.2f  (realized %.2f, unrealized %.2f, fees %.2f)\n"
        "  PnL per share      %.5f\n"
        "  Sharpe / Sortino   %.2f / %.2f (annualized from %zu samples%s)\n"
        "  max drawdown       %.2f over %.1f s\n"
        "  interval win rate  %.1f%%\n",
        res.strategy.c_str(), static_cast<unsigned long long>(res.events), res.wall_seconds,
        res.events_per_sec, static_cast<double>(res.sim_end - res.sim_start) / 1e9,
        static_cast<unsigned long long>(res.orders_sent),
        static_cast<unsigned long long>(res.cancels_sent), static_cast<unsigned long long>(m.fills),
        static_cast<long long>(m.volume), static_cast<long long>(m.max_abs_position), m.total_pnl,
        m.realized_pnl, m.unrealized_pnl, m.fees, m.pnl_per_share, m.sharpe_annualized,
        m.sortino_annualized, m.samples,
        m.samples < kMinSamplesForSharpe ? " - TOO FEW, not meaningful" : "", m.max_drawdown,
        m.max_drawdown_duration_s, m.win_rate * 100.0);

    if (args.has("report")) {
        std::ofstream(args.str("report")) << res.to_json();
        std::printf("  report  -> %s\n", args.str("report").c_str());
    }
    if (args.has("equity")) {
        res.write_equity_csv(args.str("equity"));
        std::printf("  equity  -> %s\n", args.str("equity").c_str());
    }
    if (args.has("fills")) {
        res.write_fills_csv(args.str("fills"));
        std::printf("  fills   -> %s\n", args.str("fills").c_str());
    }
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "mds_backtest: %s\n", e.what());
    return 1;
}
