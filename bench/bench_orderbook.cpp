// SPDX-License-Identifier: MIT
//
// bench_orderbook - single-threaded order book throughput and per-operation
// latency on realistic synthetic order flow.
//
//   bench_orderbook --events 2000000
//
// Latency is measured with two steady_clock reads around every apply() call,
// which adds ~20-40 ns of timer overhead per sample; the throughput pass runs
// without any timing on the hot path, so use it for headline numbers.
#include <algorithm>
#include <array>
#include <cstdio>
#include <exception>
#include <vector>

#include "mds/common/cli.hpp"
#include "mds/common/clock.hpp"
#include "mds/common/stats.hpp"
#include "mds/feed/synthetic_feed.hpp"
#include "mds/orderbook/order_book.hpp"

using namespace mds;

int main(int argc, char** argv) try {
    CliArgs args(argc, argv);
    const auto n = static_cast<std::size_t>(args.i64("events", 2'000'000));
    const int reps = static_cast<int>(args.i64("reps", 5));

    SyntheticFeedConfig fc;
    fc.seed = static_cast<std::uint64_t>(args.i64("seed", 99));
    SyntheticFeed feed(fc);
    const auto events = feed.generate(n);

    std::array<std::size_t, 4> mix{};
    for (const auto& e : events) ++mix[static_cast<std::size_t>(e.type)];
    std::printf("bench_orderbook: %zu events  (add %.1f%%  cancel %.1f%%  modify %.1f%%  market %.1f%%)\n",
                events.size(), 100.0 * mix[0] / events.size(), 100.0 * mix[1] / events.size(),
                100.0 * mix[2] / events.size(), 100.0 * mix[3] / events.size());

    // ---- throughput (no timing on the hot path) ------------------------------
    std::vector<double> rates;
    std::vector<Fill> fills;
    fills.reserve(256);
    std::uint64_t total_fills = 0;
    for (int r = 0; r < reps; ++r) {
        OrderBook book;
        total_fills = 0;
        const auto t0 = mono_ns();
        for (const auto& e : events) {
            fills.clear();
            book.apply(e, fills);
            total_fills += fills.size();
        }
        const auto t1 = mono_ns();
        rates.push_back(static_cast<double>(events.size()) / (static_cast<double>(t1 - t0) / 1e9));
    }
    std::sort(rates.begin(), rates.end());
    std::printf("\n[throughput] median %.2f M events/s over %d reps (fills=%llu)\n",
                rates[rates.size() / 2] / 1e6, reps, static_cast<unsigned long long>(total_fills));

    // ---- per-operation latency ---------------------------------------------
    std::array<std::vector<std::int64_t>, 4> lat;
    for (auto& v : lat) v.reserve(events.size() / 2);
    OrderBook book;
    for (const auto& e : events) {
        fills.clear();
        const auto t0 = mono_ns();
        book.apply(e, fills);
        const auto t1 = mono_ns();
        lat[static_cast<std::size_t>(e.type)].push_back(t1 - t0);
    }
    std::printf("\n[latency ns, includes timer overhead]\n");
    std::printf("  %-8s %10s %8s %8s %8s %8s %10s\n", "op", "samples", "mean", "p50", "p99",
                "p99.9", "max");
    const char* names[] = {"add", "cancel", "modify", "market"};
    for (std::size_t i = 0; i < lat.size(); ++i) {
        const auto p = summarize(lat[i]);
        std::printf("  %-8s %10zu %8.0f %8.0f %8.0f %8.0f %10.0f\n", names[i], p.samples, p.mean,
                    p.p50, p.p99, p.p999, p.max);
    }
    const auto& st = book.stats();
    std::printf("\n[book] orders=%zu bid_levels=%zu ask_levels=%zu compactions=%llu rejects=%llu valid=%s\n",
                book.order_count(), book.level_count(Side::Buy), book.level_count(Side::Sell),
                static_cast<unsigned long long>(st.compactions),
                static_cast<unsigned long long>(st.rejects), book.validate() ? "yes" : "NO");
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "bench_orderbook: %s\n", e.what());
    return 1;
}
