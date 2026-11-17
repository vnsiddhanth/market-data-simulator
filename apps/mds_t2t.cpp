// SPDX-License-Identifier: MIT
//
// mds_t2t - tick-to-trade latency benchmark of a complete HFT hot path.
//
//   [feed thread]      "NIC": hands raw ITCH packets to the strategy core,
//          |           stamping each with the CPU time-stamp counter (rx)
//          v  SPSC ring of 64-byte packet slots (one cache line each)
//   [strategy thread]  ITCH decode -> FlatOrderBook update -> signal ->
//          |           pre-trade risk gate -> OUCH 4.2 Enter Order encode
//          v  SPSC ring of order slots
//   [gateway thread]   takes the encoded order and transmits it (in memory,
//                      or a real UDP send with --udp), stamping tx
//
//   tick-to-trade = tx - rx  (market data in -> order out)
//
// All three threads busy-spin (no sleeps, no syscalls on the hot path) and can
// be pinned to dedicated cores, as production HFT systems do. Each stage is
// timestamped so the report breaks latency down per hop.
//
//   mds_t2t --events 2000000 --rate 500000 --pin 2,4,6
//   mds_t2t --rate 0                 # no pacing: maximum throughput (latency includes queueing)
//   mds_t2t --udp 127.0.0.1:9100     # include a real kernel UDP send in the gateway
#include <algorithm>
#include <cstring>
#include <atomic>
#include <cstdio>
#include <exception>
#include <string>
#include <thread>
#include <vector>

#include "mds/common/affinity.hpp"
#include "mds/common/cli.hpp"
#include "mds/common/clock.hpp"
#include "mds/common/json.hpp"
#include "mds/common/stats.hpp"
#include "mds/common/tsc.hpp"
#include "mds/common/udp.hpp"
#include "mds/feed/synthetic_feed.hpp"
#include "mds/hft/ouch.hpp"
#include "mds/hft/risk.hpp"
#include "mds/itch/book_builder.hpp"
#include "mds/pipeline/spsc_ring.hpp"

using namespace mds;

namespace {

struct alignas(64) Packet {        // exactly one cache line
    std::uint64_t rx_tsc;
    std::uint16_t len;
    std::uint8_t data[54];
};
static_assert(sizeof(Packet) == 64, "one packet per cache line");

struct alignas(64) OrderSlot {
    std::uint64_t rx_tsc;      // market data arrived
    std::uint64_t book_tsc;    // book updated
    std::uint64_t signal_tsc;  // decision + risk passed + encoded
    std::uint8_t len;
    std::uint8_t msg[hft::ouch::kLenEnterOrder];
};

struct Spans {
    std::vector<std::int64_t> t2t, rx_to_book, book_to_signal, signal_to_tx;
};

double ns(std::uint64_t cycles) { return TscClock::to_ns(cycles); }

}  // namespace

int main(int argc, char** argv) try {
    CliArgs args(argc, argv);
    if (args.has("help")) {
        std::puts("usage: mds_t2t [--events N] [--rate msgs/s (0 = unpaced)] [--pin F,S,G] [--udp HOST:PORT] [--json PATH]");
        return 0;
    }
    TscClock::calibrate();
    const auto n_events = static_cast<std::size_t>(args.i64("events", 2'000'000));
    const double rate = args.f64("rate", 500'000.0);
    int cpu[3] = {-1, -1, -1};
    if (args.has("pin")) {
        const std::string pin = args.str("pin");
        std::size_t pos = 0;
        for (int i = 0; i < 3 && pos != std::string::npos; ++i) {
            const std::size_t comma = pin.find(',', pos);
            cpu[i] = std::stoi(pin.substr(pos, comma - pos));
            pos = comma == std::string::npos ? comma : comma + 1;
        }
    }

    // ---- build the market data: synthetic exchange -> ITCH packets ---------------
    SyntheticFeedConfig fc;
    fc.seed = static_cast<std::uint64_t>(args.i64("seed", 5));
    fc.max_events = n_events;
    SyntheticFeed feed(fc);
    std::vector<std::uint8_t> itch_bytes;
    itch_bytes.reserve(n_events * 40);
    itch::Writer w(itch_bytes);
    itch::SyntheticItchEncoder enc("SIM", 1);
    MarketEvent ev;
    bool first = true;
    while (feed.next(ev)) {
        if (first) {
            enc.begin(w, ev.ts);
            first = false;
        }
        enc.encode(ev, w);
    }
    std::vector<Packet> packets;
    packets.reserve(w.messages());
    itch::for_each_frame(itch_bytes.data(), itch_bytes.size(), [&](const std::uint8_t* m, std::size_t len) {
        Packet p{};
        p.len = static_cast<std::uint16_t>(len);
        std::memcpy(p.data, m, len);
        packets.push_back(p);
    });

    // ---- pipeline ------------------------------------------------------------------
    SpscRing<Packet> md_ring(1 << 14);
    SpscRing<OrderSlot> order_ring(1 << 12);
    std::atomic<int> ready{0};
    std::atomic<bool> go{false}, feed_done{false}, strategy_done{false};
    std::uint64_t orders_out = 0, udp_failures = 0;
    hft::RiskGate risk{hft::RiskLimits{}};
    Spans spans;
    spans.t2t.reserve(packets.size() / 2);

    std::thread gateway([&] {
        pin_current_thread(cpu[2]);
        UdpSocket udp;
        bool use_udp = false;
        if (args.has("udp")) {
            const std::string ep = args.str("udp");
            const auto colon = ep.rfind(':');
            udp.connect(ep.substr(0, colon), static_cast<std::uint16_t>(std::stoi(ep.substr(colon + 1))));
            use_udp = true;
        }
        std::vector<std::uint8_t> wire;  // in-memory "wire" when not using UDP
        wire.reserve(1 << 20);
        ready.fetch_add(1);
        while (!go.load(std::memory_order_acquire)) cpu_relax();
        OrderSlot o{};
        for (;;) {
            if (!order_ring.try_pop(o)) {
                // Strategy finished and nothing left in flight: done.
                if (strategy_done.load(std::memory_order_acquire) && !order_ring.try_pop(o)) break;
                if (!order_ring.empty_approx() || o.len == 0) {
                    cpu_relax();
                    continue;
                }
            }
            if (o.len == 0) continue;
            if (use_udp) {
                if (!udp.send(o.msg, o.len)) ++udp_failures;
            } else {
                if (wire.size() + o.len > wire.capacity()) wire.clear();
                wire.insert(wire.end(), o.msg, o.msg + o.len);
            }
            const std::uint64_t tx = TscClock::now();
            spans.t2t.push_back(static_cast<std::int64_t>(ns(tx - o.rx_tsc)));
            spans.rx_to_book.push_back(static_cast<std::int64_t>(ns(o.book_tsc - o.rx_tsc)));
            spans.book_to_signal.push_back(static_cast<std::int64_t>(ns(o.signal_tsc - o.book_tsc)));
            spans.signal_to_tx.push_back(static_cast<std::int64_t>(ns(tx - o.signal_tsc)));
            ++orders_out;
            o.len = 0;
        }
    });

    std::thread strategy([&] {
        pin_current_thread(cpu[1]);
        itch::BookBuilder builder("SIM", 1u << 15);
        ready.fetch_add(1);
        while (!go.load(std::memory_order_acquire)) cpu_relax();
        Packet batch[32];
        std::int64_t prev_bid = 0, prev_ask = 0;
        std::uint64_t token = 1;
        for (;;) {
            const std::size_t n = md_ring.try_pop_n(batch, 32);
            if (n == 0) {
                if (feed_done.load(std::memory_order_acquire) && md_ring.empty_approx()) break;
                cpu_relax();
                continue;
            }
            for (std::size_t i = 0; i < n; ++i) {
                const Packet& p = batch[i];
                builder.on_message(p.data, p.len);
                const std::uint64_t book_tsc = TscClock::now();
                const itch::SymbolBook* sb = builder.book(1);
                if (!sb) continue;
                const FlatOrderBook& book = sb->book;
                const auto bb = book.best_bid(), ba = book.best_ask();
                if (!bb || !ba) continue;
                // Signal: the inside quote moved -> re-quote on the side the
                // book leans towards, joining the best price.
                if (*bb == prev_bid && *ba == prev_ask) continue;
                prev_bid = *bb;
                prev_ask = *ba;
                const Side side = book.best_bid_qty() >= book.best_ask_qty() ? Side::Buy : Side::Sell;
                const Price px = side == Side::Buy ? *bb : *ba;
                const Price mid = (*bb + *ba) / 2;
                const std::int64_t now_ns = static_cast<std::int64_t>(ns(book_tsc));
                if (risk.check(side, px, 100, mid, now_ns) != hft::RiskReject::None) continue;
                risk.on_order_closed();  // IOC-style: assume the quote is replaced next tick
                OrderSlot o;
                o.rx_tsc = p.rx_tsc;
                o.book_tsc = book_tsc;
                o.len = static_cast<std::uint8_t>(hft::ouch::enter_order(o.msg, token++, side, 100, "SIM",
                                                                         static_cast<std::uint32_t>(px), hft::ouch::kTifDay));
                o.signal_tsc = TscClock::now();
                while (!order_ring.try_push(o)) cpu_relax();
            }
        }
        strategy_done.store(true, std::memory_order_release);
    });

    // ---- feed ("NIC") thread: this one --------------------------------------------
    pin_current_thread(cpu[0]);
    while (ready.load() < 2) cpu_relax();
    const double cycles_per_packet = rate > 0 ? (1e9 / rate) / TscClock::to_ns(1) : 0.0;
    std::uint64_t stalls = 0;
    const auto wall0 = mono_ns();
    go.store(true, std::memory_order_release);
    std::uint64_t next_send = TscClock::now();
    // Scheduling-jitter detector: a busy-spinning thread should never see a
    // gap of more than a few hundred ns between loop iterations. A larger gap
    // means the OS / hypervisor took the CPU away - jitter, not our code.
    const std::uint64_t gap_threshold = static_cast<std::uint64_t>(20'000.0 / TscClock::to_ns(1));  // 20 us
    std::uint64_t prev = TscClock::now(), hiccups = 0, hiccup_cycles = 0, max_gap = 0;
    auto watch = [&] {
        const std::uint64_t now = TscClock::now();
        const std::uint64_t gap = now - prev;
        prev = now;
        if (gap > gap_threshold) {
            ++hiccups;
            hiccup_cycles += gap;
            if (gap > max_gap) max_gap = gap;
        }
    };
    for (std::size_t i = 0; i < packets.size(); ++i) {
        if (cycles_per_packet > 0) {
            while (TscClock::now() < next_send) watch();  // pace in cycles, no syscalls
            next_send += static_cast<std::uint64_t>(cycles_per_packet);
        }
        watch();
        Packet& p = packets[i];
        p.rx_tsc = TscClock::now();
        while (!md_ring.try_push(p)) {
            ++stalls;
            cpu_relax();
        }
    }
    feed_done.store(true, std::memory_order_release);
    strategy.join();
    gateway.join();
    const double wall = static_cast<double>(mono_ns() - wall0) / 1e9;

    // ---- report -------------------------------------------------------------------------
    const auto t2t = summarize(spans.t2t);
    const auto a = summarize(spans.rx_to_book), b = summarize(spans.book_to_signal), c = summarize(spans.signal_to_tx);
    std::printf("mds_t2t: %zu ITCH packets in %.3f s (%.2f M msg/s), pacing %s, tsc %.2f GHz, pinned %d,%d,%d%s\n",
                packets.size(), wall, packets.size() / wall / 1e6,
                rate > 0 ? (std::to_string(static_cast<long long>(rate)) + " msg/s").c_str() : "off",
                TscClock::ghz(), cpu[0], cpu[1], cpu[2], args.has("udp") ? "  gateway=UDP" : "  gateway=memory");
    std::printf("  scheduling jitter on the feed core: %llu gaps > 20 us, %.1f ms lost in total, worst %.2f ms%s\n",
                static_cast<unsigned long long>(hiccups), ns(hiccup_cycles) / 1e6, ns(max_gap) / 1e6,
                hiccups ? "  (OS/VM preemption: isolate cores to remove)" : "");
    std::printf("  orders sent %llu   feed stalls %llu   udp send failures %llu\n",
                static_cast<unsigned long long>(orders_out), static_cast<unsigned long long>(stalls),
                static_cast<unsigned long long>(udp_failures));
    std::printf("  risk: accepted %llu, rate_limit %llu, price_collar %llu, kill_switch %llu\n",
                static_cast<unsigned long long>(risk.count(hft::RiskReject::None)),
                static_cast<unsigned long long>(risk.count(hft::RiskReject::RateLimit)),
                static_cast<unsigned long long>(risk.count(hft::RiskReject::PriceCollar)),
                static_cast<unsigned long long>(risk.count(hft::RiskReject::KillSwitch)));
    std::printf("\n  %-34s %8s %8s %8s %8s %10s\n", "latency (ns)", "p50", "p90", "p99", "p99.9", "max");
    auto row = [](const char* name, const Percentiles& p) {
        std::printf("  %-34s %8.0f %8.0f %8.0f %8.0f %10.0f\n", name, p.p50, p.p90, p.p99, p.p999, p.max);
    };
    row("tick-to-trade (rx -> order out)", t2t);
    row("  rx -> book updated (hop + ITCH)", a);
    row("  book -> decision + risk + OUCH", b);
    row("  decision -> gateway tx (hop)", c);

    if (args.has("json")) {
        JsonWriter j(args.str("json"));
        j.begin_object().kv("packets", static_cast<std::uint64_t>(packets.size())).kv("orders", orders_out);
        j.kv("jitter_gaps", hiccups).kv("jitter_ms", ns(hiccup_cycles) / 1e6, 3).kv("jitter_max_ms", ns(max_gap) / 1e6, 3);
        j.kv("rate", rate, 0).kv("msgs_per_sec", packets.size() / wall, 1).kv("udp", args.has("udp"));
        auto obj = [&](const char* k, const Percentiles& p) {
            j.key(k).begin_object().kv("p50", p.p50, 0).kv("p90", p.p90, 0).kv("p99", p.p99, 0);
            j.kv("p999", p.p999, 0).kv("max", p.max, 0).end_object();
        };
        obj("tick_to_trade_ns", t2t);
        obj("rx_to_book_ns", a);
        obj("book_to_signal_ns", b);
        obj("signal_to_tx_ns", c);
        j.end_object();
    }
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "mds_t2t: %s\n", e.what());
    return 1;
}
