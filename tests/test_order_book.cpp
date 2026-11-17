// SPDX-License-Identifier: MIT
#include <algorithm>
#include <map>
#include <random>
#include <vector>

#include "mds/orderbook/order_book.hpp"
#include "test_framework.hpp"

using namespace mds;

namespace {

constexpr Price px(double p) { return static_cast<Price>(p * kPriceScale + 0.5); }

}  // namespace

TEST(order_book, empty_book) {
    OrderBook book;
    CHECK(!book.best_bid());
    CHECK(!book.best_ask());
    CHECK(!book.mid_price());
    CHECK(!book.spread());
    CHECK_EQ(book.order_count(), 0u);
    CHECK_NEAR(book.imbalance(), 0.0, 1e-12);
    CHECK(book.validate());
}

TEST(order_book, resting_orders_set_top_of_book) {
    OrderBook book;
    std::vector<Fill> f;
    CHECK(book.add_limit(1, Side::Buy, px(99.98), 100, 1, f) == BookResult::Ok);
    CHECK(book.add_limit(2, Side::Buy, px(99.99), 200, 2, f) == BookResult::Ok);
    CHECK(book.add_limit(3, Side::Sell, px(100.01), 300, 3, f) == BookResult::Ok);
    CHECK(book.add_limit(4, Side::Sell, px(100.02), 400, 4, f) == BookResult::Ok);
    CHECK(f.empty());
    CHECK_EQ(*book.best_bid(), px(99.99));
    CHECK_EQ(*book.best_ask(), px(100.01));
    CHECK_EQ(*book.spread(), px(0.02));
    CHECK_NEAR(*book.mid_price(), 100.0, 1e-9);
    CHECK_EQ(book.best_bid_qty(), 200);
    CHECK_EQ(book.level_count(Side::Buy), 2u);
    CHECK(book.validate());
}

TEST(order_book, aggregates_same_price_level) {
    OrderBook book;
    std::vector<Fill> f;
    book.add_limit(1, Side::Buy, px(100), 100, 1, f);
    book.add_limit(2, Side::Buy, px(100), 250, 2, f);
    book.add_limit(3, Side::Buy, px(100), 50, 3, f);
    const auto d = book.depth(Side::Buy, 5);
    CHECK_EQ(d.size(), 1u);
    CHECK_EQ(d[0].qty, 400);
    CHECK_EQ(d[0].orders, 3u);
}

TEST(order_book, fifo_time_priority_within_level) {
    OrderBook book;
    std::vector<Fill> f;
    book.add_limit(1, Side::Sell, px(100), 100, 1, f);
    book.add_limit(2, Side::Sell, px(100), 100, 2, f);
    book.add_limit(3, Side::Sell, px(100), 100, 3, f);
    book.execute_market(10, Side::Buy, 150, 4, f);
    CHECK_EQ(f.size(), 2u);
    CHECK_EQ(f[0].maker_id, 1u);
    CHECK_EQ(f[0].qty, 100);
    CHECK_EQ(f[1].maker_id, 2u);
    CHECK_EQ(f[1].qty, 50);
    CHECK_EQ(book.find(2)->qty, 50);
    CHECK(book.find(1) == nullptr);
    CHECK(book.validate());
}

TEST(order_book, price_priority_and_sweep) {
    OrderBook book;
    std::vector<Fill> f;
    book.add_limit(1, Side::Sell, px(100.02), 100, 1, f);
    book.add_limit(2, Side::Sell, px(100.01), 100, 2, f);
    book.add_limit(3, Side::Sell, px(100.03), 100, 3, f);
    // Marketable limit buy for 250 @ 100.02: takes 100.01 then 100.02, rests 50.
    book.add_limit(9, Side::Buy, px(100.02), 250, 4, f);
    CHECK_EQ(f.size(), 2u);
    CHECK_EQ(f[0].price, px(100.01));
    CHECK_EQ(f[1].price, px(100.02));
    CHECK_EQ(*book.best_bid(), px(100.02));
    CHECK_EQ(book.best_bid_qty(), 50);
    CHECK_EQ(*book.best_ask(), px(100.03));
    CHECK(book.validate());
}

TEST(order_book, trades_at_maker_price) {
    OrderBook book;
    std::vector<Fill> f;
    book.add_limit(1, Side::Buy, px(99.50), 100, 1, f);
    book.add_limit(2, Side::Sell, px(99.00), 100, 2, f);  // aggressive sell
    CHECK_EQ(f.size(), 1u);
    CHECK_EQ(f[0].price, px(99.50));  // price improvement for the taker
    CHECK(f[0].aggressor == Side::Sell);
    CHECK(book.empty());
}

TEST(order_book, market_order_partial_liquidity) {
    OrderBook book;
    std::vector<Fill> f;
    book.add_limit(1, Side::Buy, px(100), 100, 1, f);
    const Qty filled = book.execute_market(2, Side::Sell, 500, 2, f);
    CHECK_EQ(filled, 100);
    CHECK(book.empty());  // remainder of a market order never rests
}

TEST(order_book, cancel_updates_aggregates_and_skips_tombstone) {
    OrderBook book;
    std::vector<Fill> f;
    book.add_limit(1, Side::Sell, px(100), 100, 1, f);
    book.add_limit(2, Side::Sell, px(100), 200, 2, f);
    book.add_limit(3, Side::Sell, px(100), 300, 3, f);
    CHECK(book.cancel(2) == BookResult::Ok);
    CHECK_EQ(book.volume_at(Side::Sell, px(100)), 400);
    CHECK(book.cancel(2) == BookResult::UnknownOrder);
    book.execute_market(9, Side::Buy, 150, 4, f);
    CHECK_EQ(f.size(), 2u);
    CHECK_EQ(f[0].maker_id, 1u);
    CHECK_EQ(f[1].maker_id, 3u);  // cancelled #2 skipped
    CHECK(book.validate());
}

TEST(order_book, cancelling_last_order_removes_level) {
    OrderBook book;
    std::vector<Fill> f;
    book.add_limit(1, Side::Buy, px(100), 100, 1, f);
    book.add_limit(2, Side::Buy, px(99), 100, 2, f);
    book.cancel(1);
    CHECK_EQ(*book.best_bid(), px(99));
    CHECK_EQ(book.level_count(Side::Buy), 1u);
}

TEST(order_book, modify_down_keeps_priority_up_loses_it) {
    OrderBook book;
    std::vector<Fill> f;
    book.add_limit(1, Side::Sell, px(100), 300, 1, f);
    book.add_limit(2, Side::Sell, px(100), 100, 2, f);
    book.modify(1, 200, 3);  // down: still first
    book.execute_market(9, Side::Buy, 50, 4, f);
    CHECK_EQ(f.back().maker_id, 1u);
    f.clear();
    book.modify(1, 500, 5);  // up: goes behind #2
    book.execute_market(10, Side::Buy, 50, 6, f);
    CHECK_EQ(f.back().maker_id, 2u);
    CHECK_EQ(book.volume_at(Side::Sell, px(100)), 550);
    CHECK(book.validate());
}

TEST(order_book, rejects_bad_input) {
    OrderBook book;
    std::vector<Fill> f;
    CHECK(book.add_limit(1, Side::Buy, px(100), 0, 1, f) == BookResult::InvalidQty);
    CHECK(book.add_limit(1, Side::Buy, 0, 10, 1, f) == BookResult::InvalidPrice);
    CHECK(book.add_limit(1, Side::Buy, px(100), 10, 1, f) == BookResult::Ok);
    CHECK(book.add_limit(1, Side::Buy, px(100), 10, 1, f) == BookResult::DuplicateOrder);
    CHECK(book.modify(77, 10, 1) == BookResult::UnknownOrder);
    CHECK_EQ(book.stats().rejects, 4u);
}

TEST(order_book, imbalance_and_sweep_vwap) {
    OrderBook book;
    std::vector<Fill> f;
    book.add_limit(1, Side::Buy, px(100), 300, 1, f);
    book.add_limit(2, Side::Sell, px(101), 100, 2, f);
    book.add_limit(3, Side::Sell, px(102), 100, 3, f);
    CHECK_NEAR(book.imbalance(1), 0.5, 1e-12);  // (300-100)/(400)
    CHECK_NEAR(*book.sweep_vwap(Side::Sell, 200), 101.5, 1e-9);
    CHECK(!book.sweep_vwap(Side::Sell, 1000));  // not enough liquidity
}

TEST(order_book, compaction_bounds_tombstones) {
    OrderBook book;
    std::vector<Fill> f;
    for (OrderId id = 1; id <= 1000; ++id) book.add_limit(id, Side::Buy, px(100), 1, id, f);
    for (OrderId id = 1; id <= 990; ++id) book.cancel(id);
    CHECK(book.stats().compactions > 0);
    CHECK_EQ(book.volume_at(Side::Buy, px(100)), 10);
    CHECK(book.validate());
}

// ---------------------------------------------------------------------------
// Differential test: compare against an obviously-correct O(n) reference.
// ---------------------------------------------------------------------------
namespace {

struct RefOrder {
    OrderId id;
    Side side;
    Price price;
    Qty qty;
    std::uint64_t seq;
};

class ReferenceBook {
public:
    void add(OrderId id, Side side, Price price, Qty qty, std::vector<Fill>& fills, Timestamp ts) {
        qty -= match(id, side, price, qty, fills, ts);
        if (qty > 0) orders_.push_back({id, side, price, qty, seq_++});
    }
    void market(OrderId id, Side side, Qty qty, std::vector<Fill>& fills, Timestamp ts) {
        match(id, side, side == Side::Buy ? kMaxPrice : kMinPrice, qty, fills, ts);
    }
    void cancel(OrderId id) {
        orders_.erase(std::remove_if(orders_.begin(), orders_.end(),
                                     [id](const RefOrder& o) { return o.id == id; }),
                      orders_.end());
    }
    void modify(OrderId id, Qty q) {
        for (auto& o : orders_) {
            if (o.id != id) continue;
            if (q <= 0) return cancel(id);
            if (q > o.qty) o.seq = seq_++;  // loses priority
            o.qty = q;
            return;
        }
    }
    std::map<Price, Qty> levels(Side s) const {
        std::map<Price, Qty> m;
        for (const auto& o : orders_) {
            if (o.side == s) m[o.price] += o.qty;
        }
        return m;
    }
    bool has(OrderId id) const {
        return std::any_of(orders_.begin(), orders_.end(), [id](const RefOrder& o) { return o.id == id; });
    }

private:
    Qty match(OrderId taker, Side side, Price limit, Qty qty, std::vector<Fill>& fills, Timestamp ts) {
        Qty filled = 0;
        while (qty > 0) {
            RefOrder* best = nullptr;
            for (auto& o : orders_) {
                if (o.side == side) continue;
                const bool ok = side == Side::Buy ? o.price <= limit : o.price >= limit;
                if (!ok) continue;
                if (!best) {
                    best = &o;
                    continue;
                }
                const bool better = side == Side::Buy ? o.price < best->price : o.price > best->price;
                if (better || (o.price == best->price && o.seq < best->seq)) best = &o;
            }
            if (!best) break;
            const Qty t = std::min(qty, best->qty);
            fills.push_back({best->id, taker, best->price, t, side, ts});
            best->qty -= t;
            qty -= t;
            filled += t;
            if (best->qty == 0) cancel(best->id);
        }
        return filled;
    }
    std::vector<RefOrder> orders_;
    std::uint64_t seq_{0};
};

}  // namespace

TEST(order_book, differential_vs_reference_model) {
    std::mt19937_64 rng(2024);
    OrderBook book;
    ReferenceBook ref;
    std::vector<OrderId> ids;
    OrderId next = 1;
    std::vector<Fill> f1, f2;

    for (int step = 0; step < 20'000; ++step) {
        f1.clear();
        f2.clear();
        const auto ts = static_cast<Timestamp>(step);
        const int op = static_cast<int>(rng() % 100);
        const Side side = (rng() & 1) ? Side::Buy : Side::Sell;
        const Price price = px(100.0) + static_cast<Price>(rng() % 21) * 100 - 1000;
        const Qty qty = static_cast<Qty>(1 + rng() % 10) * 10;

        if (op < 55 || ids.empty()) {
            book.add_limit(next, side, price, qty, ts, f1);
            ref.add(next, side, price, qty, f2, ts);
            ids.push_back(next++);
        } else if (op < 80) {
            const OrderId id = ids[rng() % ids.size()];
            book.cancel(id);
            ref.cancel(id);
        } else if (op < 90) {
            const OrderId id = ids[rng() % ids.size()];
            const Qty q = static_cast<Qty>(rng() % 12) * 10;
            book.modify(id, q, ts);
            ref.modify(id, q);
        } else {
            book.execute_market(next, side, qty, ts, f1);
            ref.market(next, side, qty, f2, ts);
            ++next;
        }

        CHECK_EQ(f1.size(), f2.size());
        for (std::size_t i = 0; i < f1.size(); ++i) {
            CHECK_EQ(f1[i].maker_id, f2[i].maker_id);
            CHECK_EQ(f1[i].price, f2[i].price);
            CHECK_EQ(f1[i].qty, f2[i].qty);
        }
        if (step % 97 == 0) {
            for (Side s : {Side::Buy, Side::Sell}) {
                const auto expect = ref.levels(s);
                const auto got = book.depth(s, 1000);
                CHECK_EQ(got.size(), expect.size());
                for (const auto& lv : got) CHECK_EQ(lv.qty, expect.at(lv.price));
            }
            CHECK(book.validate());
        }
    }
    CHECK(book.validate());
}
