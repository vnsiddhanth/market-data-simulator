// SPDX-License-Identifier: MIT
#include <random>
#include <vector>

#include "mds/common/bits.hpp"
#include "mds/common/flat_hash.hpp"
#include "mds/feed/synthetic_feed.hpp"
#include "mds/orderbook/flat_order_book.hpp"
#include "mds/orderbook/order_book.hpp"
#include "test_framework.hpp"

using namespace mds;

namespace {
constexpr Price px(double p) { return static_cast<Price>(p * kPriceScale + 0.5); }

bool same_depth(const OrderBook& a, const FlatOrderBook& b) {
    if (a.order_count() != b.order_count()) return false;
    for (Side s : {Side::Buy, Side::Sell}) {
        const auto da = a.depth(s, 100000), db = b.depth(s, 100000);
        if (da.size() != db.size()) return false;
        for (std::size_t i = 0; i < da.size(); ++i) {
            if (da[i].price != db[i].price || da[i].qty != db[i].qty || da[i].orders != db[i].orders) return false;
        }
    }
    return true;
}
}  // namespace

TEST(bits, scan_bitset_next_prev) {
    ScanBitset b(300);
    CHECK_EQ(b.next_at_or_after(0), -1);
    b.set(5);
    b.set(64);
    b.set(299);
    CHECK_EQ(b.next_at_or_after(0), 5);
    CHECK_EQ(b.next_at_or_after(6), 64);
    CHECK_EQ(b.next_at_or_after(65), 299);
    CHECK_EQ(b.prev_at_or_before(298), 64);
    CHECK_EQ(b.prev_at_or_before(63), 5);
    CHECK_EQ(b.prev_at_or_before(4), -1);
    b.reset(64);
    CHECK_EQ(b.next_at_or_after(6), 299);
}

TEST(flat_hash, insert_find_erase_backward_shift) {
    OrderIndex idx(8);
    std::mt19937_64 rng(1);
    std::vector<std::uint64_t> keys;
    for (std::uint32_t i = 0; i < 5000; ++i) {
        keys.push_back(rng());
        CHECK(idx.insert(keys.back(), i));
    }
    CHECK(!idx.insert(keys[10], 99));  // duplicate
    for (std::uint32_t i = 0; i < 5000; i += 2) CHECK(idx.erase(keys[i]));
    for (std::uint32_t i = 0; i < 5000; ++i) {
        CHECK_EQ(idx.find(keys[i]), i % 2 ? i : OrderIndex::kNotFound);
    }
    CHECK_EQ(idx.size(), 2500u);
}

TEST(flat_book, basic_fifo_and_sweep) {
    FlatOrderBook b;
    std::vector<Fill> f;
    b.add_limit(1, Side::Sell, px(100.02), 100, 1, f);
    b.add_limit(2, Side::Sell, px(100.01), 100, 2, f);
    b.add_limit(3, Side::Sell, px(100.01), 100, 3, f);
    b.add_limit(9, Side::Buy, px(100.02), 250, 4, f);
    CHECK_EQ(f.size(), 3u);
    CHECK_EQ(f[0].maker_id, 2u);
    CHECK_EQ(f[1].maker_id, 3u);
    CHECK_EQ(f[2].maker_id, 1u);
    CHECK_EQ(f[2].qty, 50);
    CHECK_EQ(*b.best_ask(), px(100.02));
    CHECK(!b.best_bid());
    CHECK(b.validate());
}

TEST(flat_book, exchange_feed_operations) {
    FlatOrderBook b;
    std::vector<Fill> f;
    b.add_limit(1, Side::Buy, px(50.00), 300, 1, f);
    b.add_limit(2, Side::Buy, px(50.00), 200, 2, f);
    CHECK(b.execute(1, 100, 3, f) == BookResult::Ok);  // ITCH 'E'
    CHECK_EQ(f.back().price, px(50.00));
    CHECK(f.back().aggressor == Side::Sell);
    CHECK(b.reduce(2, 50) == BookResult::Ok);          // ITCH 'X'
    CHECK_EQ(b.volume_at(Side::Buy, px(50.00)), 350);
    CHECK(b.replace(1, 7, px(50.05), 400, 4) == BookResult::Ok);  // ITCH 'U'
    CHECK(!b.contains(1));
    CHECK_EQ(*b.best_bid(), px(50.05));
    CHECK(b.execute(7, 400, 5, f) == BookResult::Ok);
    CHECK_EQ(*b.best_bid(), px(50.00));                 // bitmap found the next level
    CHECK(b.validate());
}

TEST(flat_book, recenters_when_price_leaves_window) {
    FlatBookConfig cfg;
    cfg.window_ticks = 256;  // tiny ladder: $2.56 at 1c ticks
    FlatOrderBook b(cfg);
    std::vector<Fill> f;
    b.add_limit(1, Side::Buy, px(100.00), 100, 1, f);
    b.add_limit(2, Side::Sell, px(100.50), 100, 2, f);
    CHECK(b.add_limit(3, Side::Sell, px(101.90), 100, 3, f) == BookResult::Ok);  // forces recentre
    CHECK(b.recenters() >= 1);
    CHECK_EQ(*b.best_bid(), px(100.00));
    CHECK_EQ(*b.best_ask(), px(100.50));
    CHECK(b.add_limit(4, Side::Sell, px(200.00), 100, 4, f) == BookResult::InvalidPrice);  // can't fit
    CHECK(b.validate());
}

TEST(flat_book, differential_vs_map_book_random_ops) {
    std::mt19937_64 rng(77);
    FlatBookConfig cfg;
    cfg.window_ticks = 512;  // small so jumps exercise re-centring
    FlatOrderBook flat(cfg);
    OrderBook map;
    std::vector<OrderId> ids;
    OrderId next = 1;
    std::vector<Fill> f1, f2;
    Price center = px(100.0);
    for (int step = 0; step < 50'000; ++step) {
        f1.clear();
        f2.clear();
        if (step % 5000 == 4999 && map.empty()) center += 300 * 100;  // drift the market
        const auto ts = static_cast<Timestamp>(step);
        const int op = static_cast<int>(rng() % 100);
        const Side side = (rng() & 1) ? Side::Buy : Side::Sell;
        const Price price = center + (static_cast<Price>(rng() % 41) - 20) * 100;
        const Qty qty = static_cast<Qty>(1 + rng() % 10) * 10;
        BookResult r1, r2;
        if (op < 55 || ids.empty()) {
            r1 = flat.add_limit(next, side, price, qty, ts, f1);
            r2 = map.add_limit(next, side, price, qty, ts, f2);
            ids.push_back(next++);
        } else if (op < 80) {
            const OrderId id = ids[rng() % ids.size()];
            r1 = flat.cancel(id);
            r2 = map.cancel(id);
        } else if (op < 90) {
            const OrderId id = ids[rng() % ids.size()];
            const Qty q = static_cast<Qty>(rng() % 12) * 10;
            r1 = flat.modify(id, q, ts);
            r2 = map.modify(id, q, ts);
        } else {
            flat.execute_market(next, side, qty, ts, f1);
            map.execute_market(next, side, qty, ts, f2);
            ++next;
            r1 = r2 = BookResult::Ok;
        }
        CHECK(r1 == r2);
        CHECK_EQ(f1.size(), f2.size());
        for (std::size_t i = 0; i < f1.size(); ++i) {
            CHECK_EQ(f1[i].maker_id, f2[i].maker_id);
            CHECK_EQ(f1[i].price, f2[i].price);
            CHECK_EQ(f1[i].qty, f2[i].qty);
        }
        if (step % 251 == 0) {
            CHECK(same_depth(map, flat));
            CHECK(flat.validate());
        }
    }
    CHECK(same_depth(map, flat));
    CHECK(flat.validate());
}

TEST(flat_book, matches_map_book_on_synthetic_feed) {
    SyntheticFeedConfig fc;
    fc.seed = 31;
    const auto events = SyntheticFeed(fc).generate(300'000);
    FlatOrderBook flat;
    OrderBook map;
    std::vector<Fill> f1, f2;
    std::uint64_t fills = 0;
    for (const auto& ev : events) {
        f1.clear();
        f2.clear();
        flat.apply(ev, f1);
        map.apply(ev, f2);
        CHECK_EQ(f1.size(), f2.size());
        fills += f1.size();
    }
    CHECK(fills > 1000);
    CHECK(same_depth(map, flat));
    CHECK(flat.validate());
    CHECK_EQ(flat.stats().rejects, 0u);
}
