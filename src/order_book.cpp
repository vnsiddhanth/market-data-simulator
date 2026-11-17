// SPDX-License-Identifier: MIT
#include "mds/orderbook/order_book.hpp"

#include <algorithm>
#include <utility>

namespace mds {

namespace {

// Compact a level once tombstones outnumber live entries (and there are enough
// of them for the O(n) rebuild to be worth it). Amortised over the cancels
// that produced the tombstones this is O(1) per cancel.
constexpr std::uint32_t kCompactMinTombstones = 64;

}  // namespace

OrderBook::OrderBook(SymbolId symbol, std::size_t expected_orders) : symbol_(symbol) {
    // Keep the load factor low: lookups dominate (every cancel and every fill)
    // and a rehash in the middle of a burst is a multi-millisecond stall.
    // max_load_factor must be set before reserve() so the bucket count accounts for it.
    orders_.max_load_factor(0.5f);
    orders_.reserve(expected_orders);
}

// ---------------------------------------------------------------------------
// Matching
// ---------------------------------------------------------------------------

template <class Levels>
Qty OrderBook::match(Levels& opposite, Side aggressor, OrderId taker, Price limit, Qty qty,
                     Timestamp ts, std::vector<Fill>& fills) {
    Qty filled = 0;
    while (qty > 0 && !opposite.empty()) {
        auto level_it = opposite.begin();
        const Price level_px = level_it->first;

        // Price check: a buy may lift asks <= limit, a sell may hit bids >= limit.
        const bool crosses = aggressor == Side::Buy ? level_px <= limit : level_px >= limit;
        if (!crosses) break;

        PriceLevel& level = level_it->second;
        while (qty > 0 && !level.queue.empty()) {
            const QueueEntry head = level.queue.front();
            auto it = orders_.find(head.id);
            if (it == orders_.end() || it->second.seq != head.seq) {
                // Tombstone left behind by a cancel / priority-losing modify.
                level.queue.pop_front();
                --level.tombstones;
                continue;
            }
            Order& maker = it->second;
            const Qty traded = std::min(qty, maker.qty);
            maker.qty -= traded;
            level.total_qty -= traded;
            qty -= traded;
            filled += traded;

            fills.push_back(Fill{maker.id, taker, level_px, traded, aggressor, ts});
            ++stats_.fills;
            stats_.volume += traded;

            if (maker.qty == 0) {
                level.queue.pop_front();
                --level.live;
                orders_.erase(it);
            }
        }
        if (level.live == 0) {
            opposite.erase(level_it);  // drops any trailing tombstones with it
        }
    }
    return filled;
}

template <class Levels>
void OrderBook::rest(Levels& levels, Order&& order) {
    order.seq = next_seq_++;
    PriceLevel& level = levels[order.price];
    level.queue.push_back(QueueEntry{order.id, order.seq});
    level.total_qty += order.qty;
    ++level.live;
    const OrderId id = order.id;
    orders_.emplace(id, std::move(order));
}

template <class Levels>
void OrderBook::remove_from_level(Levels& levels, const Order& order) {
    auto level_it = levels.find(order.price);
    if (level_it == levels.end()) return;  // unreachable if invariants hold
    PriceLevel& level = level_it->second;
    level.total_qty -= order.qty;
    --level.live;
    ++level.tombstones;
    if (level.live == 0) {
        levels.erase(level_it);
    } else {
        maybe_compact(level, orders_, stats_);
    }
}

void OrderBook::maybe_compact(PriceLevel& level, const std::unordered_map<OrderId, Order>& live,
                              BookStats& stats) {
    if (level.tombstones < kCompactMinTombstones || level.tombstones <= level.live) return;
    std::deque<QueueEntry> compacted;
    for (const QueueEntry& e : level.queue) {
        auto it = live.find(e.id);
        if (it != live.end() && it->second.seq == e.seq) compacted.push_back(e);
    }
    level.queue.swap(compacted);
    level.tombstones = 0;
    ++stats.compactions;
}

// ---------------------------------------------------------------------------
// Order entry
// ---------------------------------------------------------------------------

BookResult OrderBook::add_limit(OrderId id, Side side, Price price, Qty qty, Timestamp ts,
                                std::vector<Fill>& fills) {
    last_ts_ = ts;
    if (qty <= 0) {
        ++stats_.rejects;
        return BookResult::InvalidQty;
    }
    if (price <= 0) {
        ++stats_.rejects;
        return BookResult::InvalidPrice;
    }
    if (orders_.find(id) != orders_.end()) {
        ++stats_.rejects;
        return BookResult::DuplicateOrder;
    }
    ++stats_.adds;

    Qty remaining = qty;
    if (side == Side::Buy) {
        remaining -= match(asks_, side, id, price, remaining, ts, fills);
        if (remaining > 0) rest(bids_, Order{id, price, remaining, side, ts, 0});
    } else {
        remaining -= match(bids_, side, id, price, remaining, ts, fills);
        if (remaining > 0) rest(asks_, Order{id, price, remaining, side, ts, 0});
    }
    return BookResult::Ok;
}

Qty OrderBook::execute_market(OrderId id, Side side, Qty qty, Timestamp ts,
                              std::vector<Fill>& fills) {
    last_ts_ = ts;
    if (qty <= 0) {
        ++stats_.rejects;
        return 0;
    }
    ++stats_.market_orders;
    return side == Side::Buy ? match(asks_, side, id, kMaxPrice, qty, ts, fills)
                             : match(bids_, side, id, kMinPrice, qty, ts, fills);
}

BookResult OrderBook::cancel(OrderId id) {
    auto it = orders_.find(id);
    if (it == orders_.end()) {
        ++stats_.rejects;
        return BookResult::UnknownOrder;
    }
    const Order order = it->second;  // copy: the level fix-up below needs it
    orders_.erase(it);               // erase first so compaction sees it as dead
    if (order.side == Side::Buy) {
        remove_from_level(bids_, order);
    } else {
        remove_from_level(asks_, order);
    }
    ++stats_.cancels;
    return BookResult::Ok;
}

BookResult OrderBook::modify(OrderId id, Qty new_qty, Timestamp ts) {
    last_ts_ = ts;
    auto it = orders_.find(id);
    if (it == orders_.end()) {
        ++stats_.rejects;
        return BookResult::UnknownOrder;
    }
    if (new_qty <= 0) return cancel(id);

    Order& order = it->second;
    ++stats_.modifies;
    if (new_qty == order.qty) return BookResult::Ok;

    if (new_qty < order.qty) {
        // Size-down: in place, keeps queue position.
        const Qty delta = order.qty - new_qty;
        order.qty = new_qty;
        if (order.side == Side::Buy) {
            bids_.find(order.price)->second.total_qty -= delta;
        } else {
            asks_.find(order.price)->second.total_qty -= delta;
        }
        return BookResult::Ok;
    }

    // Size-up: loses priority. Tombstone the old entry, re-queue at the back.
    const Order old = order;  // copy before erase invalidates the reference
    orders_.erase(it);
    Order replacement = old;
    replacement.qty = new_qty;
    replacement.ts = ts;
    if (old.side == Side::Buy) {
        remove_from_level(bids_, old);
        rest(bids_, std::move(replacement));
    } else {
        remove_from_level(asks_, old);
        rest(asks_, std::move(replacement));
    }
    return BookResult::Ok;
}

BookResult OrderBook::apply(const MarketEvent& ev, std::vector<Fill>& fills) {
    switch (ev.type) {
        case EventType::Add: return add_limit(ev.id, ev.side, ev.price, ev.qty, ev.ts, fills);
        case EventType::Cancel: last_ts_ = ev.ts; return cancel(ev.id);
        case EventType::Modify: return modify(ev.id, ev.qty, ev.ts);
        case EventType::Market:
            execute_market(ev.id, ev.side, ev.qty, ev.ts, fills);
            return BookResult::Ok;
    }
    return BookResult::InvalidQty;
}

// ---------------------------------------------------------------------------
// Market data
// ---------------------------------------------------------------------------

std::optional<Price> OrderBook::best_bid() const noexcept {
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->first;
}

std::optional<Price> OrderBook::best_ask() const noexcept {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first;
}

std::optional<Price> OrderBook::mid_price_x2() const noexcept {
    if (bids_.empty() || asks_.empty()) return std::nullopt;
    return bids_.begin()->first + asks_.begin()->first;
}

std::optional<double> OrderBook::mid_price() const noexcept {
    auto m2 = mid_price_x2();
    if (!m2) return std::nullopt;
    return static_cast<double>(*m2) / (2.0 * static_cast<double>(kPriceScale));
}

std::optional<Price> OrderBook::spread() const noexcept {
    if (bids_.empty() || asks_.empty()) return std::nullopt;
    return asks_.begin()->first - bids_.begin()->first;
}

Qty OrderBook::volume_at(Side side, Price price) const noexcept {
    if (side == Side::Buy) {
        auto it = bids_.find(price);
        return it == bids_.end() ? 0 : it->second.total_qty;
    }
    auto it = asks_.find(price);
    return it == asks_.end() ? 0 : it->second.total_qty;
}

Qty OrderBook::best_bid_qty() const noexcept {
    return bids_.empty() ? 0 : bids_.begin()->second.total_qty;
}

Qty OrderBook::best_ask_qty() const noexcept {
    return asks_.empty() ? 0 : asks_.begin()->second.total_qty;
}

void OrderBook::depth_into(Side side, std::size_t levels, std::vector<LevelView>& out) const {
    out.clear();
    auto collect = [&](const auto& map) {
        for (auto it = map.begin(); it != map.end() && out.size() < levels; ++it) {
            out.push_back(LevelView{it->first, it->second.total_qty, it->second.live});
        }
    };
    if (side == Side::Buy) {
        collect(bids_);
    } else {
        collect(asks_);
    }
}

std::vector<LevelView> OrderBook::depth(Side side, std::size_t levels) const {
    std::vector<LevelView> out;
    out.reserve(levels);
    depth_into(side, levels, out);
    return out;
}

double OrderBook::imbalance(std::size_t levels) const noexcept {
    auto sum = [levels](const auto& map) {
        Qty total = 0;
        std::size_t n = 0;
        for (auto it = map.begin(); it != map.end() && n < levels; ++it, ++n) {
            total += it->second.total_qty;
        }
        return total;
    };
    const auto b = static_cast<double>(sum(bids_));
    const auto a = static_cast<double>(sum(asks_));
    return (a + b) > 0.0 ? (b - a) / (b + a) : 0.0;
}

std::optional<double> OrderBook::sweep_vwap(Side side, Qty qty) const noexcept {
    if (qty <= 0) return std::nullopt;
    auto walk = [qty](const auto& map) -> std::optional<double> {
        Qty remaining = qty;
        double notional = 0.0;
        for (const auto& [px, level] : map) {
            const Qty take = std::min(remaining, level.total_qty);
            notional += static_cast<double>(take) * static_cast<double>(px);
            remaining -= take;
            if (remaining == 0) break;
        }
        if (remaining > 0) return std::nullopt;  // not enough liquidity
        return notional / static_cast<double>(qty) / static_cast<double>(kPriceScale);
    };
    return side == Side::Buy ? walk(bids_) : walk(asks_);
}

const Order* OrderBook::find(OrderId id) const noexcept {
    auto it = orders_.find(id);
    return it == orders_.end() ? nullptr : &it->second;
}

void OrderBook::clear() {
    bids_.clear();
    asks_.clear();
    orders_.clear();
    stats_ = BookStats{};
    last_ts_ = 0;
}

bool OrderBook::validate() const {
    std::size_t reachable = 0;
    auto check_side = [&](const auto& map, Side side) {
        for (const auto& [px, level] : map) {
            if (level.live == 0) return false;  // empty levels must be erased
            Qty qty = 0;
            std::uint32_t live = 0;
            for (const QueueEntry& e : level.queue) {
                auto it = orders_.find(e.id);
                if (it == orders_.end() || it->second.seq != e.seq) continue;
                const Order& o = it->second;
                if (o.price != px || o.side != side || o.qty <= 0) return false;
                qty += o.qty;
                ++live;
            }
            if (qty != level.total_qty || live != level.live) return false;
            if (level.queue.size() != static_cast<std::size_t>(level.live) + level.tombstones) {
                return false;
            }
            reachable += live;
        }
        return true;
    };
    if (!check_side(bids_, Side::Buy) || !check_side(asks_, Side::Sell)) return false;
    if (reachable != orders_.size()) return false;
    if (!bids_.empty() && !asks_.empty() && bids_.begin()->first >= asks_.begin()->first) {
        return false;  // crossed book
    }
    return true;
}

}  // namespace mds
