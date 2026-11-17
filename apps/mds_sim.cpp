// SPDX-License-Identifier: MIT
//
// mds_sim - run the synthetic exchange feed through the market-data pipeline.
//
//   mds_sim --events 2000000 --queue both
//   mds_sim --events 500000 --redis 127.0.0.1:6379 --symbol SIM --csv ticks.csv
//
// Phases:
//   1. generate   synthetic order flow (or --live: generate on the producer thread)
//   2. pipeline   producer -> queue -> consumer rebuilding the order book
//   3. verify     downstream book must equal the exchange's shadow book
//   4. persist    optional: CSV / binary / Redis (time-series + event chunks)
#include <cstdio>
#include <exception>
#include <iostream>
#include <string>

#include "mds/common/cli.hpp"
#include "mds/common/clock.hpp"
#include "mds/feed/synthetic_feed.hpp"
#include "mds/pipeline/mutex_queue.hpp"
#include "mds/pipeline/pipeline.hpp"
#include "mds/pipeline/spsc_ring.hpp"
#include "mds/storage/tick_store.hpp"

using namespace mds;

namespace {

void usage() {
    std::puts(
        "usage: mds_sim [options]\n"
        "  --events N          number of events (default 1000000)\n"
        "  --seed S            RNG seed (default 42)\n"
        "  --rate R            mean arrivals per simulated second (default 2000)\n"
        "  --queue Q           spsc | mutex | both (default both)\n"
        "  --capacity C        queue capacity, rounded to pow2 (default 65536)\n"
        "  --batch B           consumer drain batch size (default 64)\n"
        "  --pin P,C           pin producer/consumer threads to CPUs P and C\n"
        "  --live              generate events on the producer thread\n"
        "  --depth D           print D levels of the final book (default 5)\n"
        "  --csv PATH          write events as CSV\n"
        "  --bin PATH          write events in MDSEVT01 binary format\n"
        "  --redis HOST:PORT   persist to Redis\n"
        "  --symbol SYM        Redis symbol namespace (default SIM)\n"
        "  --purge             delete existing Redis data for the symbol first\n"
        "  --zset              force the sorted-set fallback instead of RedisTimeSeries");
}

void print_result(const PipelineResult& r) {
    std::printf(
        "  %-14s %10.0f events/s  (%.3f s)  fills=%llu  latency ns p50=%.0f p99=%.0f p99.9=%.0f "
        "max=%.0f  stalls=%llu idle=%llu\n",
        r.queue_name, r.events_per_sec, r.seconds, static_cast<unsigned long long>(r.fills),
        r.latency_ns.p50, r.latency_ns.p99, r.latency_ns.p999, r.latency_ns.max,
        static_cast<unsigned long long>(r.producer_stalls),
        static_cast<unsigned long long>(r.consumer_idle));
}

void print_book(const OrderBook& book, std::size_t depth) {
    const auto bids = book.depth(Side::Buy, depth);
    const auto asks = book.depth(Side::Sell, depth);
    std::printf("\n  %12s %10s | %-10s %-12s\n", "bid qty", "bid", "ask", "ask qty");
    for (std::size_t i = 0; i < depth; ++i) {
        char l[64] = "", r[64] = "";
        if (i < bids.size()) {
            std::snprintf(l, sizeof(l), "%12lld %10.4f", static_cast<long long>(bids[i].qty),
                          to_double(bids[i].price));
        }
        if (i < asks.size()) {
            std::snprintf(r, sizeof(r), "%-10.4f %-12lld", to_double(asks[i].price),
                          static_cast<long long>(asks[i].qty));
        }
        std::printf("  %23s | %s\n", l, r);
    }
    std::printf("  orders=%zu  bid levels=%zu  ask levels=%zu  imbalance(5)=%+.3f\n",
                book.order_count(), book.level_count(Side::Buy), book.level_count(Side::Sell),
                book.imbalance(5));
}

// Full-depth comparison: every level's price, aggregate qty and order count.
bool same_book(const OrderBook& a, const OrderBook& b) {
    if (a.order_count() != b.order_count()) return false;
    for (Side s : {Side::Buy, Side::Sell}) {
        const auto da = a.depth(s, a.level_count(s));
        const auto db = b.depth(s, b.level_count(s));
        if (da.size() != db.size()) return false;
        for (std::size_t i = 0; i < da.size(); ++i) {
            if (da[i].price != db[i].price || da[i].qty != db[i].qty ||
                da[i].orders != db[i].orders) {
                return false;
            }
        }
    }
    return true;
}

template <class Queue>
PipelineResult run_pipeline(const PipelineConfig& pc, const std::vector<MarketEvent>& events,
                            bool live, const SyntheticFeedConfig& fc, OrderBook& book) {
    MarketDataPipeline<Queue> pipeline(pc);
    if (!live) return pipeline.replay(events, book);
    SyntheticFeed feed(fc);
    return pipeline.run([&feed](MarketEvent& ev) { return feed.next(ev); }, book);
}

}  // namespace

int main(int argc, char** argv) try {
    CliArgs args(argc, argv);
    if (args.has("help")) {
        usage();
        return 0;
    }

    SyntheticFeedConfig fc;
    fc.seed = static_cast<std::uint64_t>(args.i64("seed", 42));
    fc.events_per_second = args.f64("rate", fc.events_per_second);
    fc.max_events = static_cast<std::uint64_t>(args.i64("events", 1'000'000));

    PipelineConfig pc;
    pc.queue_capacity = static_cast<std::size_t>(args.i64("capacity", 1 << 16));
    pc.consumer_batch = static_cast<std::size_t>(args.i64("batch", 64));
    if (args.has("pin")) {
        const std::string pin = args.str("pin");
        const auto comma = pin.find(',');
        pc.producer_cpu = std::stoi(pin.substr(0, comma));
        pc.consumer_cpu = std::stoi(pin.substr(comma + 1));
    }
    const std::string queue = args.str("queue", "both");
    const bool live = args.has("live");

    // ---- 1. generate -----------------------------------------------------
    std::vector<MarketEvent> events;
    SyntheticFeed reference(fc);
    const auto g0 = mono_ns();
    events = reference.generate(fc.max_events);
    const double gen_s = static_cast<double>(mono_ns() - g0) / 1e9;
    std::printf("[generate] %zu events in %.3f s (%.0f events/s), simulated span %.2f s\n",
                events.size(), gen_s, static_cast<double>(events.size()) / gen_s,
                events.empty() ? 0.0
                               : static_cast<double>(events.back().ts - events.front().ts) / 1e9);

    // ---- 2. pipeline + 3. verify -------------------------------------------
    std::printf("[pipeline] capacity=%zu batch=%zu mode=%s\n", pc.queue_capacity,
                pc.consumer_batch, live ? "live-generate" : "replay");
    OrderBook final_book;
    double spsc_eps = 0, mutex_eps = 0;
    if (queue == "spsc" || queue == "both") {
        OrderBook book;
        const auto r = run_pipeline<SpscRing<Envelope>>(pc, events, live, fc, book);
        print_result(r);
        spsc_eps = r.events_per_sec;
        std::printf("  %-14s consistency vs exchange book: %s\n", "",
                    same_book(book, reference.exchange_book()) && book.validate() ? "OK" : "MISMATCH");
        final_book = std::move(book);
    }
    if (queue == "mutex" || queue == "both") {
        OrderBook book;
        const auto r = run_pipeline<MutexQueue<Envelope>>(pc, events, live, fc, book);
        print_result(r);
        mutex_eps = r.events_per_sec;
        std::printf("  %-14s consistency vs exchange book: %s\n", "",
                    same_book(book, reference.exchange_book()) && book.validate() ? "OK" : "MISMATCH");
        if (final_book.empty()) final_book = std::move(book);
    }
    if (spsc_eps > 0 && mutex_eps > 0) {
        std::printf("  speed-up (lock-free / mutex): %.2fx\n", spsc_eps / mutex_eps);
    }
    print_book(final_book, static_cast<std::size_t>(args.i64("depth", 5)));

    // ---- 4. persist ---------------------------------------------------------
    if (args.has("csv")) {
        write_csv(args.str("csv"), events);
        std::printf("[persist] wrote %s\n", args.str("csv").c_str());
    }
    if (args.has("bin")) {
        write_binary(args.str("bin"), events);
        std::printf("[persist] wrote %s\n", args.str("bin").c_str());
    }
    if (args.has("redis")) {
        std::string host;
        std::uint16_t port;
        RedisClient::parse_endpoint(args.str("redis"), host, port);
        RedisClient redis;
        redis.connect(host, port);

        TickStoreConfig tc;
        tc.symbol = args.str("symbol", "SIM");
        tc.force_sorted_set_fallback = args.has("zset");
        TickStore store(redis, tc);
        if (args.has("purge")) store.purge();
        store.init();

        OrderBook book;
        std::vector<Fill> fills;
        const auto t0 = mono_ns();
        for (const auto& ev : events) {
            fills.clear();
            book.apply(ev, fills);
            store.record_event(ev);
            store.record_book(ev.ts, book);
            for (const auto& f : fills) store.record_trade(f.ts, f.price, f.qty);
        }
        store.flush();
        const double s = static_cast<double>(mono_ns() - t0) / 1e9;
        const auto& st = store.stats();
        std::printf(
            "[redis] backend=%s events=%llu chunks=%llu series_samples=%llu in %.3f s "
            "(%.0f events/s, %llu round trips, %.1f MB sent)\n",
            store.using_timeseries() ? "RedisTimeSeries" : "sorted-set",
            static_cast<unsigned long long>(st.events), static_cast<unsigned long long>(st.chunks),
            static_cast<unsigned long long>(st.series_samples), s,
            static_cast<double>(st.events) / s,
            static_cast<unsigned long long>(redis.round_trips()),
            static_cast<double>(redis.bytes_sent()) / 1e6);
    }
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "mds_sim: %s\n", e.what());
    return 1;
}
