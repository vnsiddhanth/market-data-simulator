// SPDX-License-Identifier: MIT
//
// bench_orderbook - single-threaded order book throughput and per-operation
// latency on realistic synthetic order flow, comparing the two books:
//
//   map   OrderBook      std::map levels + std::deque FIFO + unordered_map
//   flat  FlatOrderBook  flat price ladder + intrusive pool + bitmap + flat hash
//
//   bench_orderbook --events 2000000 [--book map|flat|both]
//
// Per-operation latency is taken with the CPU time-stamp counter (RDTSC,
// ~7 ns per read) around every apply(); the throughput pass has no timing on
// the hot path, so use it for headline numbers.
#include <algorithm>
#include <array>
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

#include "mds/common/cli.hpp"
#include "mds/common/clock.hpp"
#include "mds/common/json.hpp"
#include "mds/common/stats.hpp"
#include "mds/feed/synthetic_feed.hpp"
#include "mds/common/tsc.hpp"
#include "mds/orderbook/flat_order_book.hpp"
#include "mds/orderbook/order_book.hpp"

using namespace mds;

namespace {

const char* kOpNames[] = {"add", "cancel", "modify", "market"};

struct BookResultRow {
    const char* name;
    double events_per_sec;
    std::vector<Percentiles> ops;
    std::uint64_t fills;
    bool valid;
};

template <class Book, class Make>
BookResultRow run_book(const char* name, Make make, const std::vector<MarketEvent>& events, int reps) {
    std::vector<double> rates;
    std::vector<Fill> fills;
    fills.reserve(256);
    std::uint64_t total_fills = 0;
    for (int r = 0; r < reps; ++r) {
        Book book = make();
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

    std::array<std::vector<std::int64_t>, 4> lat;
    for (auto& v : lat) v.reserve(events.size() / 2);
    Book book = make();
    for (const auto& e : events) {
        fills.clear();
        const std::uint64_t c0 = TscClock::now();
        book.apply(e, fills);
        const std::uint64_t c1 = TscClock::now();
        lat[static_cast<std::size_t>(e.type)].push_back(static_cast<std::int64_t>(TscClock::to_ns(c1 - c0)));
    }
    BookResultRow row{name, rates[rates.size() / 2], {}, total_fills, book.validate()};
    for (auto& v : lat) row.ops.push_back(summarize(v));
    return row;
}

}  // namespace

int main(int argc, char** argv) try {
    CliArgs args(argc, argv);
    const auto n = static_cast<std::size_t>(args.i64("events", 2'000'000));
    const int reps = static_cast<int>(args.i64("reps", 5));
    const std::string which = args.str("book", "both");
    TscClock::calibrate();

    SyntheticFeedConfig fc;
    fc.seed = static_cast<std::uint64_t>(args.i64("seed", 99));
    SyntheticFeed feed(fc);
    const auto events = feed.generate(n);

    std::array<std::size_t, 4> mix{};
    for (const auto& e : events) ++mix[static_cast<std::size_t>(e.type)];
    std::printf("bench_orderbook: %zu events  (add %.1f%%  cancel %.1f%%  modify %.1f%%  market %.1f%%)  tsc=%s %.2f GHz\n",
                events.size(), 100.0 * mix[0] / events.size(), 100.0 * mix[1] / events.size(),
                100.0 * mix[2] / events.size(), 100.0 * mix[3] / events.size(),
                TscClock::hardware() ? "rdtsc" : "steady_clock", TscClock::ghz());

    std::vector<BookResultRow> rows;
    if (which == "map" || which == "both") {
        rows.push_back(run_book<OrderBook>("map", [] { return OrderBook(0, 1 << 16); }, events, reps));
    }
    if (which == "flat" || which == "both") {
        rows.push_back(run_book<FlatOrderBook>("flat", [] { return FlatOrderBook(FlatBookConfig{}); }, events, reps));
    }

    std::printf("\n[throughput, median of %d]\n", reps);
    for (const auto& r : rows) {
        std::printf("  %-6s %8.2f M events/s   fills=%llu  valid=%s%s\n", r.name, r.events_per_sec / 1e6,
                    static_cast<unsigned long long>(r.fills), r.valid ? "yes" : "NO",
                    rows.size() == 2 && &r == &rows[1]
                        ? (std::string("   ") + std::to_string(rows[1].events_per_sec / rows[0].events_per_sec).substr(0, 4) + "x vs map").c_str()
                        : "");
    }
    std::printf("\n[latency per operation, ns, RDTSC-timed]\n");
    std::printf("  %-6s %-8s %10s %8s %8s %8s %8s\n", "book", "op", "samples", "mean", "p50", "p99", "p99.9");
    for (const auto& r : rows) {
        for (std::size_t i = 0; i < r.ops.size(); ++i) {
            const auto& p = r.ops[i];
            std::printf("  %-6s %-8s %10zu %8.0f %8.0f %8.0f %8.0f\n", r.name, kOpNames[i], p.samples, p.mean,
                        p.p50, p.p99, p.p999);
        }
    }

    if (args.has("json")) {
        JsonWriter j(args.str("json"));
        const BookResultRow& top = rows.back();  // flat when both ran: the HFT book
        j.begin_object();
        j.kv("events", static_cast<std::uint64_t>(events.size()));
        j.kv("book", top.name);
        j.kv("events_per_sec", top.events_per_sec, 1);
        auto ops = [&](const BookResultRow& r) {
            j.key("ops").begin_array();
            for (std::size_t k = 0; k < r.ops.size(); ++k) {
                j.begin_object().kv("op", kOpNames[k]).kv("samples", static_cast<std::uint64_t>(r.ops[k].samples));
                j.kv("mean_ns", r.ops[k].mean, 1).kv("p50_ns", r.ops[k].p50, 0);
                j.kv("p99_ns", r.ops[k].p99, 0).kv("p999_ns", r.ops[k].p999, 0);
                j.end_object();
            }
            j.end_array();
        };
        ops(top);
        j.key("books").begin_array();
        for (const auto& r : rows) {
            j.begin_object().kv("name", r.name).kv("events_per_sec", r.events_per_sec, 1).kv("valid", r.valid);
            ops(r);
            j.end_object();
        }
        j.end_array();
        j.end_object();
    }
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "bench_orderbook: %s\n", e.what());
    return 1;
}
