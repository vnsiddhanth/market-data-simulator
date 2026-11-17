// SPDX-License-Identifier: MIT
#include <cstdio>
#include <cstring>
#include <string>

#include "mds/feed/synthetic_feed.hpp"
#include "mds/storage/resp.hpp"
#include "mds/storage/tick_store.hpp"
#include "test_framework.hpp"

using namespace mds;

// ---- synthetic feed ----------------------------------------------------------

TEST(feed, deterministic_for_same_seed) {
    SyntheticFeedConfig c;
    c.seed = 77;
    const auto a = SyntheticFeed(c).generate(5000);
    const auto b = SyntheticFeed(c).generate(5000);
    CHECK_EQ(a.size(), b.size());
    CHECK(std::memcmp(a.data(), b.data(), a.size() * sizeof(MarketEvent)) == 0);
}

TEST(feed, timestamps_monotonic_and_mix_reasonable) {
    SyntheticFeedConfig c;
    SyntheticFeed feed(c);
    const auto ev = feed.generate(50'000);
    std::size_t counts[4] = {0, 0, 0, 0};
    for (std::size_t i = 0; i < ev.size(); ++i) {
        if (i) CHECK(ev[i].ts >= ev[i - 1].ts);
        ++counts[static_cast<int>(ev[i].type)];
        CHECK(ev[i].qty >= 0);
    }
    CHECK(counts[0] > counts[1]);  // adds dominate
    CHECK(counts[1] > 0 && counts[2] > 0 && counts[3] > 0);
    CHECK(feed.exchange_book().order_count() >= c.min_resting_orders / 2);
    CHECK_EQ(feed.exchange_book().stats().rejects, 0u);  // never cancels a dead order
}

TEST(feed, max_events_bounds_stream) {
    SyntheticFeedConfig c;
    c.max_events = 10;
    SyntheticFeed feed(c);
    MarketEvent e;
    int n = 0;
    while (feed.next(e)) ++n;
    CHECK_EQ(n, 10);
}

TEST(feed, csv_and_binary_roundtrip) {
    SyntheticFeedConfig c;
    c.seed = 9;
    const auto events = SyntheticFeed(c).generate(2000);
    const std::string csv = "mds_test_roundtrip.csv";
    const std::string bin = "mds_test_roundtrip.bin";
    write_csv(csv, events);
    write_binary(bin, events);
    const auto from_csv = read_csv(csv);
    const auto from_bin = read_binary(bin);
    std::remove(csv.c_str());
    std::remove(bin.c_str());
    CHECK_EQ(from_csv.size(), events.size());
    CHECK_EQ(from_bin.size(), events.size());
    CHECK(std::memcmp(from_bin.data(), events.data(), events.size() * sizeof(MarketEvent)) == 0);
    for (std::size_t i = 0; i < events.size(); ++i) {
        CHECK_EQ(from_csv[i].ts, events[i].ts);
        CHECK_EQ(from_csv[i].id, events[i].id);
        CHECK(from_csv[i].type == events[i].type);
        CHECK_EQ(from_csv[i].qty, events[i].qty);
        if (events[i].type == EventType::Add) {
            CHECK_EQ(from_csv[i].price, events[i].price);
            CHECK(from_csv[i].side == events[i].side);
        }
    }
}

// ---- RESP codec ---------------------------------------------------------------

TEST(resp, encode_command_is_binary_safe) {
    const std::string bin("a\0b", 3);
    CHECK_EQ(encode_command({"SET", "k", bin}), std::string("*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$3\r\na\0b\r\n", 29));
}

TEST(resp, parses_all_reply_types) {
    RespParser p;
    p.feed("+OK\r\n-ERR boom\r\n:42\r\n$5\r\nhello\r\n$-1\r\n*2\r\n:1\r\n$1\r\nx\r\n*-1\r\n");
    auto r = p.next();
    CHECK(r && r->type == RedisReply::Type::Status && r->str == "OK");
    r = p.next();
    CHECK(r && r->is_error() && r->str == "ERR boom");
    r = p.next();
    CHECK(r && r->type == RedisReply::Type::Integer && r->integer == 42);
    r = p.next();
    CHECK(r && r->type == RedisReply::Type::Bulk && r->str == "hello");
    r = p.next();
    CHECK(r && r->is_nil());
    r = p.next();
    CHECK(r && r->is_array() && r->elements.size() == 2 && r->elements[1].str == "x");
    r = p.next();
    CHECK(r && r->is_nil());
    CHECK(!p.next());
}

TEST(resp, incremental_feed_byte_by_byte) {
    const std::string wire = "*2\r\n*2\r\n:1700000000000\r\n+100.5\r\n$4\r\n\r\n\r\n\r\n";
    RespParser p;
    for (std::size_t i = 0; i + 1 < wire.size(); ++i) {
        p.feed(wire.data() + i, 1);
        CHECK(!p.next());  // incomplete until the very last byte
    }
    p.feed(wire.data() + wire.size() - 1, 1);
    auto r = p.next();
    CHECK(r && r->is_array());
    CHECK_EQ(r->elements[0].elements[0].integer, 1700000000000LL);
    CHECK_EQ(r->elements[0].elements[1].str, std::string("100.5"));
    CHECK_EQ(r->elements[1].str, std::string("\r\n\r\n"));  // CRLF inside bulk payload
}

TEST(resp, rejects_garbage) {
    RespParser p;
    p.feed("?what\r\n");
    bool threw = false;
    try {
        (void)p.next();
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}

// ---- tick store helpers (no server needed) -------------------------------------

TEST(tick_store, client_side_aggregation) {
    const std::vector<Sample> raw = {{0, 1}, {400, 3}, {999, 2}, {1000, 10}, {2500, 7}, {2600, 9}};
    auto avg = TickStore::aggregate(raw, Aggregation::Avg, 1000);
    CHECK_EQ(avg.size(), 3u);
    CHECK_EQ(avg[0].first, 0);
    CHECK_NEAR(avg[0].second, 2.0, 1e-12);
    CHECK_EQ(avg[2].first, 2000);
    CHECK_NEAR(avg[2].second, 8.0, 1e-12);
    CHECK_NEAR(TickStore::aggregate(raw, Aggregation::Max, 1000)[0].second, 3.0, 1e-12);
    CHECK_NEAR(TickStore::aggregate(raw, Aggregation::Min, 1000)[0].second, 1.0, 1e-12);
    CHECK_NEAR(TickStore::aggregate(raw, Aggregation::First, 1000)[2].second, 7.0, 1e-12);
    CHECK_NEAR(TickStore::aggregate(raw, Aggregation::Last, 1000)[2].second, 9.0, 1e-12);
    CHECK_NEAR(TickStore::aggregate(raw, Aggregation::Sum, 1000)[1].second, 10.0, 1e-12);
    CHECK_NEAR(TickStore::aggregate(raw, Aggregation::Count, 1000)[0].second, 3.0, 1e-12);
    CHECK_EQ(TickStore::aggregate(raw, Aggregation::None, 1000).size(), raw.size());
}

TEST(tick_store, parse_endpoint) {
    std::string host;
    std::uint16_t port = 0;
    RedisClient::parse_endpoint("10.0.0.5:6380", host, port);
    CHECK_EQ(host, std::string("10.0.0.5"));
    CHECK_EQ(port, 6380);
    RedisClient::parse_endpoint("localhost", host, port);
    CHECK_EQ(port, 6379);
}
