// SPDX-License-Identifier: MIT
//
// mds_feed - ITCH over MoldUDP64/UDP: an exchange publisher and a feed handler.
//
// Two processes (or two machines):
//   mds_feed --publish   --host 127.0.0.1 --port 31001 --request-port 31002 --events 2000000 --rate 500000
//   mds_feed --subscribe --port 31001 --request-host 127.0.0.1 --request-port 31002
// Multicast (as exchanges do):
//   mds_feed --publish --host 239.1.1.1 ...     mds_feed --subscribe --group 239.1.1.1 ...
//
// One process, with simulated packet loss to exercise gap recovery:
//   mds_feed --demo --events 1000000 --rate 500000 --drop 0.01
//
// The publisher stamps each ITCH message with the wall clock at send time; the
// handler measures wire-to-book latency as (its clock - exchange timestamp),
// the same way trading firms measure feed latency against exchange time.
#include <atomic>
#include <cstdio>
#include <exception>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "mds/common/cli.hpp"
#include "mds/common/clock.hpp"
#include "mds/common/json.hpp"
#include "mds/common/stats.hpp"
#include "mds/feed/synthetic_feed.hpp"
#include "mds/itch/book_builder.hpp"
#include "mds/net/moldudp64.hpp"

using namespace mds;

namespace {

std::vector<std::uint8_t> generate_itch(std::size_t events, std::uint64_t seed, itch::SyntheticItchEncoder& enc) {
    SyntheticFeedConfig fc;
    fc.seed = seed;
    fc.max_events = events;
    SyntheticFeed feed(fc);
    std::vector<std::uint8_t> bytes;
    bytes.reserve(events * 40);
    itch::Writer w(bytes);
    MarketEvent ev;
    bool first = true;
    Timestamp last = 0;
    while (feed.next(ev)) {
        if (first) {
            enc.begin(w, ev.ts);
            first = false;
        }
        enc.encode(ev, w);
        last = ev.ts;
    }
    enc.end(w, last);
    return bytes;
}

void publish(const CliArgs& args, const std::vector<std::uint8_t>& bytes, std::atomic<bool>* subscriber_done) {
    mold::Publisher pub(args.str("session", "MDSSIM0001"), args.str("host", "127.0.0.1"),
                        static_cast<std::uint16_t>(args.i64("port", 31001)),
                        static_cast<std::uint16_t>(args.i64("request-port", 31002)));
    const double rate = args.f64("rate", 500'000.0);
    const double drop = args.f64("drop", 0.0);
    std::mt19937_64 rng(7);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    const auto t0 = std::chrono::steady_clock::now();
    std::uint64_t i = 0;
    auto due = [&](std::uint64_t k) {
        return t0 + std::chrono::nanoseconds(static_cast<std::int64_t>(static_cast<double>(k) * 1e9 / rate));
    };
    itch::for_each_frame(bytes.data(), bytes.size(), [&](const std::uint8_t* m, std::size_t len) {
        while (rate > 0 && std::chrono::steady_clock::now() < due(i)) pub.serve_requests();
        pub.add(m, len);
        ++i;
        // Send as soon as we have caught up with the schedule (bursty like a real feed).
        if (rate <= 0 || std::chrono::steady_clock::now() < due(i)) pub.flush(drop > 0 && u(rng) < drop);
    });
    pub.end_session();
    const double sent_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("[publisher] %llu messages in %llu packets (%llu dropped on purpose) over %.2f s\n",
                static_cast<unsigned long long>(pub.messages()), static_cast<unsigned long long>(pub.packets_sent()),
                static_cast<unsigned long long>(pub.packets_dropped()), sent_s);
    // Keep serving re-requests until the subscriber is done (demo) or for a grace period.
    const auto grace = std::chrono::steady_clock::now() + std::chrono::seconds(subscriber_done ? 30 : 5);
    while (std::chrono::steady_clock::now() < grace) {
        if (subscriber_done && subscriber_done->load()) break;
        if (!pub.serve_requests()) std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    std::printf("[publisher] retransmission packets served: %llu\n",
                static_cast<unsigned long long>(pub.retransmit_packets()));
}

int subscribe(const CliArgs& args, const FlatOrderBook* expected_book, std::atomic<bool>* done_flag) {
    mold::Subscriber sub(static_cast<std::uint16_t>(args.i64("port", 31001)), args.str("group"),
                         args.str("request-host", "127.0.0.1"),
                         static_cast<std::uint16_t>(args.i64("request-port", 31002)));
    itch::BookBuilder builder(args.str("symbol", "SIM"), 1u << 15);
    std::vector<std::int64_t> wire_lat, recovered_lat;
    wire_lat.reserve(1 << 20);
    std::uint64_t messages = 0;
    const auto timeout = std::chrono::seconds(args.i64("timeout", 60));
    auto last_activity = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point first_msg{};
    while (!sub.finished()) {
        const bool any = sub.poll([&](const std::uint8_t* m, std::size_t len, std::uint64_t, bool recovered) {
            if (!messages) first_msg = std::chrono::steady_clock::now();
            ++messages;
            builder.on_message(m, len);
            if (len >= 11 && m[0] != 'S' && m[0] != 'R') {
                const auto lat = static_cast<std::int64_t>(mold::ns_since_midnight()) -
                                 static_cast<std::int64_t>(itch::be48(m + 5));
                (recovered ? recovered_lat : wire_lat).push_back(lat);
            }
        });
        const auto now = std::chrono::steady_clock::now();
        if (any) {
            last_activity = now;
        } else if (now - last_activity > timeout) {
            std::fprintf(stderr, "[subscriber] timed out waiting for data\n");
            break;
        }
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - first_msg).count();
    if (done_flag) done_flag->store(true);

    const auto w = summarize(wire_lat);
    const auto r = summarize(recovered_lat);
    std::printf("[subscriber] session %s: %llu messages in %llu packets, %.2f M msg/s\n", sub.session().c_str(),
                static_cast<unsigned long long>(messages), static_cast<unsigned long long>(sub.packets()),
                secs > 0 ? messages / secs / 1e6 : 0.0);
    std::printf("  gaps detected %llu, re-requests sent %llu, messages recovered %llu, duplicates ignored %llu, still held %zu\n",
                static_cast<unsigned long long>(sub.gaps()), static_cast<unsigned long long>(sub.requests_sent()),
                static_cast<unsigned long long>(sub.recovered()), static_cast<unsigned long long>(sub.duplicates()),
                sub.held());
    std::printf("  wire-to-book latency (exchange timestamp -> book updated), ns:\n");
    std::printf("    live        p50 %8.0f  p90 %8.0f  p99 %9.0f  p99.9 %9.0f  (%zu samples)\n", w.p50, w.p90, w.p99,
                w.p999, w.samples);
    if (r.samples) {
        std::printf("    recovered   p50 %8.0f  p90 %8.0f  p99 %9.0f  (%zu samples, includes gap detection + round trip)\n",
                    r.p50, r.p90, r.p99, r.samples);
    }
    std::printf("  unknown order refs %llu (non-zero would mean a corrupted book)\n",
                static_cast<unsigned long long>(builder.unknown_refs()));
    const itch::SymbolBook* b = builder.find(args.str("symbol", "SIM"));
    bool match = true;
    if (b && expected_book) {
        for (Side s : {Side::Buy, Side::Sell}) {
            const auto got = b->book.depth(s, 1000), want = expected_book->depth(s, 1000);
            match = match && got.size() == want.size();
            for (std::size_t i = 0; match && i < got.size(); ++i) {
                match = got[i].price == want[i].price && got[i].qty == want[i].qty;
            }
        }
        std::printf("  rebuilt book identical to the exchange's: %s\n", match ? "yes" : "NO");
    }
    if (b) {
        const auto bb = b->book.best_bid(), ba = b->book.best_ask();
        std::printf("  %s: %zu resting orders, bid %.2f ask %.2f\n", b->symbol.c_str(), b->book.order_count(),
                    bb ? to_double(*bb) : 0.0, ba ? to_double(*ba) : 0.0);
    }
    if (args.has("json")) {
        JsonWriter j(args.str("json"));
        j.begin_object().kv("messages", messages).kv("gaps", sub.gaps()).kv("recovered", sub.recovered());
        j.kv("msgs_per_sec", secs > 0 ? messages / secs : 0.0, 1).kv("book_match", match);
        j.key("wire_to_book_ns").begin_object().kv("p50", w.p50, 0).kv("p90", w.p90, 0).kv("p99", w.p99, 0);
        j.kv("p999", w.p999, 0).end_object().end_object();
    }
    return sub.finished() && builder.unknown_refs() == 0 && match ? 0 : 2;
}

}  // namespace

int main(int argc, char** argv) try {
    CliArgs args(argc, argv);
    const auto events = static_cast<std::size_t>(args.i64("events", 1'000'000));
    if (args.has("demo")) {
        itch::SyntheticItchEncoder enc("SIM", 1);
        const auto bytes = generate_itch(events, static_cast<std::uint64_t>(args.i64("seed", 3)), enc);
        std::atomic<bool> done{false};
        int rc = 0;
        std::thread sub([&] { rc = subscribe(args, &enc.exchange_book(), &done); });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));  // let the subscriber bind
        publish(args, bytes, &done);
        sub.join();
        return rc;
    }
    if (args.has("publish")) {
        itch::SyntheticItchEncoder enc("SIM", 1);
        const auto bytes = generate_itch(events, static_cast<std::uint64_t>(args.i64("seed", 3)), enc);
        publish(args, bytes, nullptr);
        return 0;
    }
    if (args.has("subscribe")) return subscribe(args, nullptr, nullptr);
    std::puts(
        "usage:\n"
        "  mds_feed --demo [--events N] [--rate msgs/s] [--drop 0.01]\n"
        "  mds_feed --publish   [--host 127.0.0.1|239.x.x.x] [--port 31001] [--request-port 31002] [--events N] [--rate R] [--drop P]\n"
        "  mds_feed --subscribe [--group 239.x.x.x] [--port 31001] [--request-host H] [--request-port 31002] [--symbol SIM]");
    return args.has("help") ? 0 : 1;
} catch (const std::exception& e) {
    std::fprintf(stderr, "mds_feed: %s\n", e.what());
    return 1;
}
