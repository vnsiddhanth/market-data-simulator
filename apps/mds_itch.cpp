// SPDX-License-Identifier: MIT
//
// mds_itch - Nasdaq TotalView-ITCH 5.0 tools.
//
// Generate a synthetic ITCH file from the simulated exchange:
//   mds_itch --generate out/sim.itch --events 2000000 --symbol SIM
//
// Replay an ITCH file (synthetic, or a real Nasdaq sample such as
// https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/ 01302019.NASDAQ_ITCH50, gunzipped):
//   mds_itch --replay 01302019.NASDAQ_ITCH50                     all symbols
//   mds_itch --replay 01302019.NASDAQ_ITCH50 --symbol AAPL      one symbol, deep ladder
//   mds_itch --replay file --parse-only                          raw decode speed
//
// Reports decode + book-building throughput (messages/s, MB/s), message mix,
// the most active symbols and the selected symbol's book.
#include <algorithm>
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

#include "mds/common/cli.hpp"
#include "mds/common/clock.hpp"
#include "mds/common/json.hpp"
#include "mds/feed/synthetic_feed.hpp"
#include "mds/itch/book_builder.hpp"

using namespace mds;
using namespace mds::itch;

namespace {

struct CountOnly {  // parse-only handler: decode every field, touch nothing else
    std::uint64_t sum{0};
    void on_system(const SystemEvent& m) { sum += m.h.ts; }
    void on_directory(const StockDirectory& m) { sum += m.stock.size(); }
    void on_add(const AddOrder& m) { sum += m.ref ^ m.price; }
    void on_executed(const OrderExecuted& m) { sum += m.ref ^ m.shares; }
    void on_executed_price(const OrderExecutedPrice& m) { sum += m.ref ^ m.price; }
    void on_cancel(const OrderCancel& m) { sum += m.ref ^ m.shares; }
    void on_delete(const OrderDelete& m) { sum += m.ref; }
    void on_replace(const OrderReplace& m) { sum += m.new_ref ^ m.price; }
    void on_trade(const Trade& m) { sum += m.match; }
};

void print_book(const FlatOrderBook& b, std::size_t levels) {
    const auto bids = b.depth(Side::Buy, levels), asks = b.depth(Side::Sell, levels);
    std::printf("  %12s %10s | %-10s %-12s\n", "bid qty", "bid", "ask", "ask qty");
    for (std::size_t i = 0; i < levels; ++i) {
        char l[64] = "", r[64] = "";
        if (i < bids.size()) std::snprintf(l, sizeof l, "%12lld %10.4f", static_cast<long long>(bids[i].qty), to_double(bids[i].price));
        if (i < asks.size()) std::snprintf(r, sizeof r, "%-10.4f %-12lld", to_double(asks[i].price), static_cast<long long>(asks[i].qty));
        std::printf("  %23s | %s\n", l, r);
    }
}

int generate(const CliArgs& args) {
    const std::string path = args.str("generate");
    SyntheticFeedConfig fc;
    fc.seed = static_cast<std::uint64_t>(args.i64("seed", 42));
    fc.max_events = static_cast<std::uint64_t>(args.i64("events", 1'000'000));
    SyntheticFeed feed(fc);
    std::vector<std::uint8_t> buf;
    buf.reserve(fc.max_events * 40);
    Writer w(buf);
    SyntheticItchEncoder enc(args.str("symbol", "SIM"), 1);
    MarketEvent ev;
    bool first = true;
    Timestamp last = 0;
    const auto t0 = mono_ns();
    while (feed.next(ev)) {
        if (first) {
            enc.begin(w, ev.ts);
            first = false;
        }
        enc.encode(ev, w);
        last = ev.ts;
    }
    enc.end(w, last);
    const double s = static_cast<double>(mono_ns() - t0) / 1e9;
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f || std::fwrite(buf.data(), 1, buf.size(), f) != buf.size()) throw std::runtime_error("cannot write " + path);
    std::fclose(f);
    std::printf("[generate] %llu events -> %llu ITCH messages, %.1f MB in %.3f s -> %s\n",
                static_cast<unsigned long long>(fc.max_events), static_cast<unsigned long long>(w.messages()),
                static_cast<double>(buf.size()) / 1e6, s, path.c_str());
    print_book(enc.exchange_book(), 5);
    return 0;
}

int replay(const CliArgs& args) {
    const std::string path = args.str("replay");
    if (args.has("parse-only")) {
        FileReader reader(path);
        CountOnly h;
        const auto t0 = mono_ns();
        const auto n = reader.run([&](const std::uint8_t* m, std::size_t len) { parse(m, len, h); });
        const double s = static_cast<double>(mono_ns() - t0) / 1e9;
        std::printf("[parse-only] %llu messages, %.1f MB in %.3f s -> %.1f M msg/s, %.0f MB/s (checksum %llu)\n",
                    static_cast<unsigned long long>(n), reader.bytes() / 1e6, s, n / s / 1e6, reader.bytes() / 1e6 / s,
                    static_cast<unsigned long long>(h.sum));
        return 0;
    }

    const std::string symbol = args.str("symbol");
    BookBuilder builder(symbol, static_cast<std::uint32_t>(args.i64("window", 0)));
    FileReader reader(path);
    const auto t0 = mono_ns();
    const auto n = reader.run([&](const std::uint8_t* m, std::size_t len) { builder.on_message(m, len); });
    const double s = static_cast<double>(mono_ns() - t0) / 1e9;

    std::printf("[replay] %s\n", path.c_str());
    std::printf("  %llu messages, %.1f MB in %.3f s -> %.2f M msg/s, %.0f MB/s (decode + book building)\n",
                static_cast<unsigned long long>(n), reader.bytes() / 1e6, s, n / s / 1e6, reader.bytes() / 1e6 / s);
    std::printf("  unknown order refs: %llu\n", static_cast<unsigned long long>(builder.unknown_refs()));
    std::printf("\n  message mix:");
    const auto& tc = builder.type_counts();
    for (int t = 0; t < 256; ++t) {
        if (tc[t]) std::printf("  %c=%llu", t, static_cast<unsigned long long>(tc[t]));
    }
    std::printf("\n");

    const auto syms = builder.symbols();
    const auto top = static_cast<std::size_t>(args.i64("top", 10));
    std::printf("\n  %-8s %10s %9s %9s %9s %8s %10s %10s %8s\n", "symbol", "messages", "adds", "execs", "deletes",
                "orders", "bid", "ask", "rejects");
    for (std::size_t i = 0; i < syms.size() && i < top; ++i) {
        const SymbolBook& b = *syms[i];
        const auto bb = b.book.best_bid(), ba = b.book.best_ask();
        std::printf("  %-8s %10llu %9llu %9llu %9llu %8zu %10.4f %10.4f %8llu\n", b.symbol.c_str(),
                    static_cast<unsigned long long>(b.messages), static_cast<unsigned long long>(b.adds),
                    static_cast<unsigned long long>(b.executions), static_cast<unsigned long long>(b.deletes),
                    b.book.order_count(), bb ? to_double(*bb) : 0.0, ba ? to_double(*ba) : 0.0,
                    static_cast<unsigned long long>(b.rejected));
    }
    const SymbolBook* focus = !symbol.empty() ? builder.find(symbol) : (syms.empty() ? nullptr : syms.front());
    if (focus) {
        std::printf("\n  book: %s (%zu orders, ladder valid=%s)\n", focus->symbol.c_str(), focus->book.order_count(),
                    focus->book.validate() ? "yes" : "NO");
        print_book(focus->book, static_cast<std::size_t>(args.i64("depth", 5)));
    }
    if (args.has("json")) {
        JsonWriter j(args.str("json"));
        j.begin_object().kv("messages", static_cast<std::uint64_t>(n)).kv("bytes", reader.bytes());
        j.kv("seconds", s, 4).kv("msgs_per_sec", n / s, 1).kv("symbols", static_cast<std::uint64_t>(syms.size()));
        j.kv("unknown_refs", builder.unknown_refs());
        j.end_object();
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) try {
    CliArgs args(argc, argv);
    if (args.has("generate")) return generate(args);
    if (args.has("replay")) return replay(args);
    std::puts(
        "usage:\n"
        "  mds_itch --generate PATH [--events N] [--symbol SIM] [--seed S]\n"
        "  mds_itch --replay PATH [--symbol SYM] [--top N] [--depth D] [--window TICKS] [--parse-only] [--json PATH]\n"
        "Real data: Nasdaq publishes sample TotalView-ITCH 5.0 files at https://emi.nasdaq.com/ITCH/ (gunzip first).");
    return args.has("help") ? 0 : 1;
} catch (const std::exception& e) {
    std::fprintf(stderr, "mds_itch: %s\n", e.what());
    return 1;
}
