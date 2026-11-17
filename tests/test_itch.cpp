// SPDX-License-Identifier: MIT
#include <vector>

#include "mds/feed/synthetic_feed.hpp"
#include "mds/itch/book_builder.hpp"
#include "mds/itch/itch.hpp"
#include "mds/orderbook/order_book.hpp"
#include "test_framework.hpp"

using namespace mds;
using namespace mds::itch;

namespace {

struct Capture {
    std::vector<char> types;
    AddOrder add{};
    OrderReplace rep{};
    OrderExecuted exe{};
    void on_system(const SystemEvent&) { types.push_back('S'); }
    void on_directory(const StockDirectory&) { types.push_back('R'); }
    void on_add(const AddOrder& m) { types.push_back('A'); add = m; }
    void on_executed(const OrderExecuted& m) { types.push_back('E'); exe = m; }
    void on_executed_price(const OrderExecutedPrice&) { types.push_back('C'); }
    void on_cancel(const OrderCancel&) { types.push_back('X'); }
    void on_delete(const OrderDelete&) { types.push_back('D'); }
    void on_replace(const OrderReplace& m) { types.push_back('U'); rep = m; }
    void on_trade(const Trade&) { types.push_back('P'); }
};

}  // namespace

TEST(itch, big_endian_helpers) {
    std::uint8_t b[8];
    put64(b, 0x0102030405060708ULL);
    CHECK_EQ(b[0], 1);
    CHECK_EQ(b[7], 8);
    CHECK_EQ(be64(b), 0x0102030405060708ULL);
    put48(b, 0x0000AABBCCDDEEFFULL);
    CHECK_EQ(be48(b), 0x0000AABBCCDDEEFFULL);
}

TEST(itch, encode_parse_roundtrip_exact_layout) {
    std::vector<std::uint8_t> buf;
    Writer w(buf);
    w.add(7, 34'200'000'000'000ULL, 123456789, Side::Sell, 300, "AAPL", 1'503'400);  // $150.34
    w.replace(7, 34'200'000'000'001ULL, 123456789, 987654321, 500, 1'503'500);
    w.executed(7, 34'200'000'000'002ULL, 987654321, 200, 42);
    // Spec layouts: 2-byte length prefix + body.
    CHECK_EQ(buf.size(), (2 + kLenAdd) + (2 + kLenReplace) + (2 + kLenExecuted));
    CHECK_EQ(be16(buf.data()), kLenAdd);
    CHECK_EQ(buf[2], 'A');
    CHECK_EQ(buf[2 + 19], 'S');                    // side at offset 19
    CHECK_EQ(be32(buf.data() + 2 + 32), 1'503'400u); // price at offset 32
    Capture c;
    const auto used = for_each_frame(buf.data(), buf.size(), [&](const std::uint8_t* m, std::size_t len) {
        CHECK(parse(m, len, c) == ParseResult::Handled);
    });
    CHECK_EQ(used, buf.size());
    CHECK_EQ(std::string(c.types.begin(), c.types.end()), std::string("AUE"));
    CHECK_EQ(c.add.h.locate, 7);
    CHECK_EQ(c.add.h.ts, 34'200'000'000'000ULL);
    CHECK_EQ(c.add.ref, 123456789u);
    CHECK(c.add.side == Side::Sell);
    CHECK_EQ(c.add.shares, 300u);
    CHECK_EQ(std::string(c.add.stock), std::string("AAPL"));
    CHECK_EQ(c.rep.new_ref, 987654321u);
    CHECK_EQ(c.exe.match, 42u);
}

TEST(itch, truncated_and_unknown_messages) {
    Capture c;
    std::uint8_t bad[12] = {'A', 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    CHECK(parse(bad, sizeof bad, c) == ParseResult::Truncated);
    std::uint8_t other[20] = {'H'};
    CHECK(parse(other, sizeof other, c) == ParseResult::Ignored);
    // Partial frame at the end of a buffer is left for the next read.
    std::vector<std::uint8_t> buf;
    Writer w(buf);
    w.del(1, 1, 99);
    const auto full = buf.size();
    buf.push_back(0);
    CHECK_EQ(for_each_frame(buf.data(), buf.size(), [](const std::uint8_t*, std::size_t) {}), full);
}

TEST(itch, synthetic_exchange_feed_rebuilds_identical_book) {
    // Simulated exchange -> ITCH bytes -> feed handler -> book must equal the
    // exchange's own book and the reference map book of the raw events.
    SyntheticFeedConfig fc;
    fc.seed = 17;
    const auto events = SyntheticFeed(fc).generate(200'000);
    std::vector<std::uint8_t> buf;
    Writer w(buf);
    SyntheticItchEncoder enc("SIM", 1);
    enc.begin(w, events.front().ts);
    OrderBook reference;
    std::vector<Fill> fills;
    for (const auto& ev : events) {
        enc.encode(ev, w);
        fills.clear();
        reference.apply(ev, fills);
    }
    enc.end(w, events.back().ts);

    BookBuilder builder;
    const auto used = for_each_frame(buf.data(), buf.size(), [&](const std::uint8_t* m, std::size_t len) {
        CHECK(builder.on_message(m, len) != ParseResult::Truncated);
    });
    CHECK_EQ(used, buf.size());
    CHECK_EQ(builder.unknown_refs(), 0u);
    const SymbolBook* b = builder.find("SIM");
    CHECK(b != nullptr);
    CHECK(b->book.validate());
    CHECK_EQ(b->rejected, 0u);
    CHECK(b->executions > 1000);
    CHECK(b->replaces > 0);
    CHECK(b->cancels > 0);
    for (Side s : {Side::Buy, Side::Sell}) {
        const auto got = b->book.depth(s, 1000);
        const auto want = reference.depth(s, 1000);
        CHECK_EQ(got.size(), want.size());
        for (std::size_t i = 0; i < got.size(); ++i) {
            CHECK_EQ(got[i].price, want[i].price);
            CHECK_EQ(got[i].qty, want[i].qty);
            CHECK_EQ(got[i].orders, want[i].orders);
        }
    }
}

TEST(itch, symbol_filter_skips_other_books) {
    std::vector<std::uint8_t> buf;
    Writer w(buf);
    w.stock_directory(1, 0, "AAA");
    w.stock_directory(2, 0, "BBB");
    w.add(1, 1, 10, Side::Buy, 100, "AAA", 100'000);
    w.add(2, 2, 11, Side::Buy, 100, "BBB", 200'000);
    w.executed(2, 3, 11, 50, 1);
    BookBuilder builder("BBB");
    for_each_frame(buf.data(), buf.size(), [&](const std::uint8_t* m, std::size_t len) { builder.on_message(m, len); });
    CHECK(builder.find("AAA") == nullptr);
    CHECK(builder.find("BBB") != nullptr);
    CHECK_EQ(builder.find("BBB")->book.order_qty(11), 50);
    CHECK_EQ(builder.unknown_refs(), 0u);
}
