// SPDX-License-Identifier: MIT
#include "mds/orderbook/flat_order_book.hpp"

#include <algorithm>
#include <stdexcept>

namespace mds {

FlatOrderBook::FlatOrderBook(FlatBookConfig cfg, SymbolId symbol) : cfg_(cfg), symbol_(symbol) {
    if (cfg_.tick <= 0) throw std::invalid_argument("tick must be > 0");
    if (cfg_.window_ticks < 64) cfg_.window_ticks = 64;
    bids_.levels.assign(cfg_.window_ticks, Level{});
    asks_.levels.assign(cfg_.window_ticks, Level{});
    bids_.nonempty.resize(cfg_.window_ticks);
    asks_.nonempty.resize(cfg_.window_ticks);
    pool_.reserve(cfg_.expected_orders);
    index_.reserve(cfg_.expected_orders);
}

void FlatOrderBook::clear() {
    std::fill(bids_.levels.begin(), bids_.levels.end(), Level{});
    std::fill(asks_.levels.begin(), asks_.levels.end(), Level{});
    bids_.nonempty.clear_all();
    asks_.nonempty.clear_all();
    best_bid_ = best_ask_ = -1;
    bid_levels_ = ask_levels_ = 0;
    pool_.clear();
    free_head_ = kNil;
    index_.clear();
    based_ = false;
    last_ts_ = 0;
    recenters_ = 0;
    stats_ = BookStats{};
}

// ---------------------------------------------------------------------------
// Ladder geometry
// ---------------------------------------------------------------------------

std::int64_t FlatOrderBook::level_index(Price price, bool may_recenter) {
    if (price <= 0 || price % cfg_.tick != 0) return -1;
    const auto window = static_cast<std::int64_t>(cfg_.window_ticks);
    if (!based_) {
        if (!may_recenter) return -1;
        base_ = (price / cfg_.tick - window / 2) * cfg_.tick;
        based_ = true;
    }
    std::int64_t off = (price - base_) / cfg_.tick;
    if (off >= 0 && off < window) return off;
    if (!may_recenter || !recenter(price)) return -1;
    off = (price - base_) / cfg_.tick;
    return (off >= 0 && off < window) ? off : -1;
}

bool FlatOrderBook::recenter(Price around) {
    const auto window = static_cast<std::int64_t>(cfg_.window_ticks);
    Price lo = around, hi = around;
    if (empty()) {
        base_ = (around / cfg_.tick - window / 2) * cfg_.tick;
        ++recenters_;
        return true;
    }
    auto extend = [&](const Ladder& l) {
        const std::int64_t first = l.nonempty.next_at_or_after(0);
        if (first < 0) return;
        const std::int64_t last = l.nonempty.prev_at_or_before(cfg_.window_ticks - 1);
        lo = std::min(lo, level_price(static_cast<std::uint32_t>(first)));
        hi = std::max(hi, level_price(static_cast<std::uint32_t>(last)));
    };
    extend(bids_);
    extend(asks_);
    if ((hi - lo) / cfg_.tick >= window) return false;  // cannot fit

    const Price center = lo + ((hi - lo) / cfg_.tick / 2) * cfg_.tick;
    Price new_base = (center / cfg_.tick - window / 2) * cfg_.tick;
    if (lo < new_base) new_base = lo;
    if (hi >= new_base + window * cfg_.tick) new_base = hi - (window - 1) * cfg_.tick;
    const std::int64_t shift = (base_ - new_base) / cfg_.tick;  // old index + shift = new index

    auto move = [&](Ladder& l, std::int64_t& best) {
        std::vector<Level> fresh(cfg_.window_ticks);
        ScanBitset bits(cfg_.window_ticks);
        for (std::int64_t i = l.nonempty.next_at_or_after(0); i >= 0;
             i = l.nonempty.next_at_or_after(static_cast<std::size_t>(i) + 1)) {
            const auto ni = static_cast<std::uint32_t>(i + shift);
            fresh[ni] = l.levels[static_cast<std::size_t>(i)];
            bits.set(ni);
            for (std::uint32_t n = fresh[ni].head; n != kNil; n = pool_[n].next) pool_[n].level = ni;
        }
        l.levels.swap(fresh);
        l.nonempty = std::move(bits);
        if (best >= 0) best += shift;
    };
    move(bids_, best_bid_);
    move(asks_, best_ask_);
    base_ = new_base;
    ++recenters_;
    return true;
}

// ---------------------------------------------------------------------------
// Pool and intrusive lists
// ---------------------------------------------------------------------------

std::uint32_t FlatOrderBook::alloc_node() {
    if (free_head_ != kNil) {
        const std::uint32_t n = free_head_;
        free_head_ = pool_[n].next;
        return n;
    }
    pool_.push_back(Node{});
    return static_cast<std::uint32_t>(pool_.size() - 1);
}

void FlatOrderBook::free_node(std::uint32_t n) noexcept {
    pool_[n].next = free_head_;
    free_head_ = n;
}

void FlatOrderBook::link_back(Ladder& ladder, std::uint32_t level, std::uint32_t n) {
    Level& L = ladder.levels[level];
    Node& node = pool_[n];
    node.level = level;
    node.prev = L.tail;
    node.next = kNil;
    if (L.tail != kNil) {
        pool_[L.tail].next = n;
    } else {
        L.head = n;
    }
    L.tail = n;
    L.qty += node.qty;
    if (++L.count == 1) {
        ladder.nonempty.set(level);
        ++(&ladder == &bids_ ? bid_levels_ : ask_levels_);
    }
}

void FlatOrderBook::unlink(Ladder& ladder, std::uint32_t n) {
    Node& node = pool_[n];
    Level& L = ladder.levels[node.level];
    if (node.prev != kNil) {
        pool_[node.prev].next = node.next;
    } else {
        L.head = node.next;
    }
    if (node.next != kNil) {
        pool_[node.next].prev = node.prev;
    } else {
        L.tail = node.prev;
    }
    L.qty -= node.qty;
    --L.count;
}

void FlatOrderBook::on_level_emptied(Side side, std::uint32_t level) {
    if (side == Side::Buy) {
        bids_.nonempty.reset(level);
        --bid_levels_;
        if (best_bid_ == static_cast<std::int64_t>(level)) {
            best_bid_ = level == 0 ? -1 : bids_.nonempty.prev_at_or_before(level - 1);
        }
    } else {
        asks_.nonempty.reset(level);
        --ask_levels_;
        if (best_ask_ == static_cast<std::int64_t>(level)) {
            best_ask_ = asks_.nonempty.next_at_or_after(static_cast<std::size_t>(level) + 1);
        }
    }
}

void FlatOrderBook::remove_order(std::uint32_t n) {
    const Node node = pool_[n];
    Ladder& ladder = node.side == Side::Buy ? bids_ : asks_;
    unlink(ladder, n);
    if (ladder.levels[node.level].count == 0) on_level_emptied(node.side, node.level);
    index_.erase(node.id);
    free_node(n);
}

void FlatOrderBook::rest(OrderId id, Side side, std::uint32_t level, Qty qty, Timestamp ts) {
    const std::uint32_t n = alloc_node();
    Node& node = pool_[n];
    node.id = id;
    node.qty = qty;
    node.ts = ts;
    node.side = side;
    if (side == Side::Buy) {
        link_back(bids_, level, n);
        if (best_bid_ < static_cast<std::int64_t>(level)) best_bid_ = level;
    } else {
        link_back(asks_, level, n);
        if (best_ask_ < 0 || best_ask_ > static_cast<std::int64_t>(level)) best_ask_ = level;
    }
    index_.insert(id, n);
}

// ---------------------------------------------------------------------------
// Matching
// ---------------------------------------------------------------------------

Qty FlatOrderBook::match(Side aggressor, OrderId taker, Price limit, Qty qty, Timestamp ts,
                         std::vector<Fill>& fills) {
    Qty filled = 0;
    Ladder& opp = aggressor == Side::Buy ? asks_ : bids_;
    while (qty > 0) {
        const std::int64_t best = aggressor == Side::Buy ? best_ask_ : best_bid_;
        if (best < 0) break;
        const auto lvl = static_cast<std::uint32_t>(best);
        const Price px = level_price(lvl);
        if (aggressor == Side::Buy ? px > limit : px < limit) break;
        Level& L = opp.levels[lvl];
        while (qty > 0 && L.head != kNil) {
            const std::uint32_t n = L.head;
            Node& maker = pool_[n];
            const Qty traded = std::min(qty, maker.qty);
            maker.qty -= traded;
            L.qty -= traded;
            qty -= traded;
            filled += traded;
            fills.push_back(Fill{maker.id, taker, px, traded, aggressor, ts});
            ++stats_.fills;
            stats_.volume += traded;
            if (maker.qty == 0) remove_order(n);  // may move best_* when the level empties
        }
    }
    return filled;
}

// ---------------------------------------------------------------------------
// Order entry
// ---------------------------------------------------------------------------

BookResult FlatOrderBook::add_limit(OrderId id, Side side, Price price, Qty qty, Timestamp ts,
                                    std::vector<Fill>& fills) {
    last_ts_ = ts;
    if (qty <= 0) {
        ++stats_.rejects;
        return BookResult::InvalidQty;
    }
    if (contains(id)) {
        ++stats_.rejects;
        return BookResult::DuplicateOrder;
    }
    const std::int64_t level = level_index(price, /*may_recenter=*/true);
    if (level < 0) {
        ++stats_.rejects;
        return BookResult::InvalidPrice;
    }
    ++stats_.adds;
    const Qty remaining = qty - match(side, id, price, qty, ts, fills);
    if (remaining > 0) rest(id, side, static_cast<std::uint32_t>(level), remaining, ts);
    return BookResult::Ok;
}

Qty FlatOrderBook::execute_market(OrderId id, Side side, Qty qty, Timestamp ts,
                                  std::vector<Fill>& fills) {
    last_ts_ = ts;
    if (qty <= 0) {
        ++stats_.rejects;
        return 0;
    }
    ++stats_.market_orders;
    return match(side, id, side == Side::Buy ? kMaxPrice : kMinPrice, qty, ts, fills);
}

BookResult FlatOrderBook::cancel(OrderId id) {
    const std::uint32_t n = index_.find(id);
    if (n == OrderIndex::kNotFound) {
        ++stats_.rejects;
        return BookResult::UnknownOrder;
    }
    remove_order(n);
    ++stats_.cancels;
    return BookResult::Ok;
}

BookResult FlatOrderBook::modify(OrderId id, Qty new_qty, Timestamp ts) {
    last_ts_ = ts;
    const std::uint32_t n = index_.find(id);
    if (n == OrderIndex::kNotFound) {
        ++stats_.rejects;
        return BookResult::UnknownOrder;
    }
    if (new_qty <= 0) return cancel(id);
    ++stats_.modifies;
    Node& node = pool_[n];
    if (new_qty == node.qty) return BookResult::Ok;
    Ladder& ladder = node.side == Side::Buy ? bids_ : asks_;
    Level& L = ladder.levels[node.level];
    if (new_qty < node.qty || L.count == 1) {
        // Size-down keeps priority; a lone order keeps its (only) place anyway.
        L.qty += new_qty - node.qty;
        node.qty = new_qty;
        if (new_qty > 0 && L.count == 1) node.ts = ts;
        return BookResult::Ok;
    }
    // Size-up loses priority: move to the back of the same level (count stays >= 1).
    const std::uint32_t level = node.level;
    unlink(ladder, n);
    node.qty = new_qty;
    node.ts = ts;
    link_back(ladder, level, n);
    return BookResult::Ok;
}

BookResult FlatOrderBook::apply(const MarketEvent& ev, std::vector<Fill>& fills) {
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
// Exchange-feed operations
// ---------------------------------------------------------------------------

BookResult FlatOrderBook::execute(OrderId id, Qty qty, Timestamp ts, std::vector<Fill>& fills) {
    last_ts_ = ts;
    const std::uint32_t n = index_.find(id);
    if (n == OrderIndex::kNotFound || qty <= 0) {
        ++stats_.rejects;
        return BookResult::UnknownOrder;
    }
    Node& node = pool_[n];
    Ladder& ladder = node.side == Side::Buy ? bids_ : asks_;
    const Qty q = std::min(qty, node.qty);
    node.qty -= q;
    ladder.levels[node.level].qty -= q;
    fills.push_back(Fill{node.id, 0, level_price(node.level), q, opposite(node.side), ts});
    ++stats_.fills;
    stats_.volume += q;
    if (node.qty == 0) remove_order(n);
    return BookResult::Ok;
}

BookResult FlatOrderBook::reduce(OrderId id, Qty qty) {
    const std::uint32_t n = index_.find(id);
    if (n == OrderIndex::kNotFound || qty <= 0) {
        ++stats_.rejects;
        return BookResult::UnknownOrder;
    }
    Node& node = pool_[n];
    Ladder& ladder = node.side == Side::Buy ? bids_ : asks_;
    const Qty q = std::min(qty, node.qty);
    node.qty -= q;
    ladder.levels[node.level].qty -= q;
    ++stats_.modifies;
    if (node.qty == 0) remove_order(n);
    return BookResult::Ok;
}

BookResult FlatOrderBook::replace(OrderId old_id, OrderId new_id, Price price, Qty qty, Timestamp ts) {
    last_ts_ = ts;
    const std::uint32_t n = index_.find(old_id);
    if (n == OrderIndex::kNotFound) {
        ++stats_.rejects;
        return BookResult::UnknownOrder;
    }
    const Side side = pool_[n].side;
    remove_order(n);
    if (qty <= 0) return BookResult::Ok;
    if (contains(new_id)) {
        ++stats_.rejects;
        return BookResult::DuplicateOrder;
    }
    const std::int64_t level = level_index(price, true);
    if (level < 0) {
        ++stats_.rejects;
        return BookResult::InvalidPrice;
    }
    rest(new_id, side, static_cast<std::uint32_t>(level), qty, ts);
    ++stats_.modifies;
    return BookResult::Ok;
}

// ---------------------------------------------------------------------------
// Market data
// ---------------------------------------------------------------------------

std::optional<Price> FlatOrderBook::best_bid() const noexcept {
    if (best_bid_ < 0) return std::nullopt;
    return level_price(static_cast<std::uint32_t>(best_bid_));
}
std::optional<Price> FlatOrderBook::best_ask() const noexcept {
    if (best_ask_ < 0) return std::nullopt;
    return level_price(static_cast<std::uint32_t>(best_ask_));
}
std::optional<double> FlatOrderBook::mid_price() const noexcept {
    if (best_bid_ < 0 || best_ask_ < 0) return std::nullopt;
    return static_cast<double>(*best_bid() + *best_ask()) / (2.0 * static_cast<double>(kPriceScale));
}
std::optional<Price> FlatOrderBook::spread() const noexcept {
    if (best_bid_ < 0 || best_ask_ < 0) return std::nullopt;
    return *best_ask() - *best_bid();
}
Qty FlatOrderBook::best_bid_qty() const noexcept {
    return best_bid_ < 0 ? 0 : bids_.levels[static_cast<std::size_t>(best_bid_)].qty;
}
Qty FlatOrderBook::best_ask_qty() const noexcept {
    return best_ask_ < 0 ? 0 : asks_.levels[static_cast<std::size_t>(best_ask_)].qty;
}
Qty FlatOrderBook::volume_at(Side side, Price price) const noexcept {
    if (!based_ || price % cfg_.tick != 0) return 0;
    const std::int64_t off = (price - base_) / cfg_.tick;
    if (off < 0 || off >= static_cast<std::int64_t>(cfg_.window_ticks)) return 0;
    return (side == Side::Buy ? bids_ : asks_).levels[static_cast<std::size_t>(off)].qty;
}
Qty FlatOrderBook::order_qty(OrderId id) const noexcept {
    const std::uint32_t n = index_.find(id);
    return n == OrderIndex::kNotFound ? 0 : pool_[n].qty;
}

std::optional<Price> FlatOrderBook::order_price(OrderId id) const noexcept {
    const std::uint32_t n = index_.find(id);
    if (n == OrderIndex::kNotFound) return std::nullopt;
    return level_price(pool_[n].level);
}

void FlatOrderBook::depth_into(Side side, std::size_t levels, std::vector<LevelView>& out) const {
    out.clear();
    if (side == Side::Buy) {
        for (std::int64_t i = best_bid_; i >= 0 && out.size() < levels;
             i = i == 0 ? -1 : bids_.nonempty.prev_at_or_before(static_cast<std::size_t>(i) - 1)) {
            const Level& L = bids_.levels[static_cast<std::size_t>(i)];
            out.push_back(LevelView{level_price(static_cast<std::uint32_t>(i)), L.qty, L.count});
        }
    } else {
        for (std::int64_t i = best_ask_; i >= 0 && out.size() < levels;
             i = asks_.nonempty.next_at_or_after(static_cast<std::size_t>(i) + 1)) {
            const Level& L = asks_.levels[static_cast<std::size_t>(i)];
            out.push_back(LevelView{level_price(static_cast<std::uint32_t>(i)), L.qty, L.count});
        }
    }
}

std::vector<LevelView> FlatOrderBook::depth(Side side, std::size_t levels) const {
    std::vector<LevelView> out;
    out.reserve(levels);
    depth_into(side, levels, out);
    return out;
}

double FlatOrderBook::imbalance(std::size_t levels) const noexcept {
    Qty b = 0, a = 0;
    std::size_t n = 0;
    for (std::int64_t i = best_bid_; i >= 0 && n < levels; ++n,
         i = i == 0 ? -1 : bids_.nonempty.prev_at_or_before(static_cast<std::size_t>(i) - 1)) {
        b += bids_.levels[static_cast<std::size_t>(i)].qty;
    }
    n = 0;
    for (std::int64_t i = best_ask_; i >= 0 && n < levels; ++n,
         i = asks_.nonempty.next_at_or_after(static_cast<std::size_t>(i) + 1)) {
        a += asks_.levels[static_cast<std::size_t>(i)].qty;
    }
    return (a + b) > 0 ? static_cast<double>(b - a) / static_cast<double>(b + a) : 0.0;
}

bool FlatOrderBook::validate() const {
    std::size_t orders = 0;
    auto check = [&](const Ladder& l, Side side, std::size_t expect_levels, std::int64_t best) {
        std::size_t levels = 0;
        std::int64_t first = -1, last = -1;
        for (std::size_t i = 0; i < l.levels.size(); ++i) {
            const Level& L = l.levels[i];
            if ((L.count > 0) != l.nonempty.test(i)) return false;
            if (L.count == 0) {
                if (L.head != kNil || L.tail != kNil || L.qty != 0) return false;
                continue;
            }
            ++levels;
            if (first < 0) first = static_cast<std::int64_t>(i);
            last = static_cast<std::int64_t>(i);
            Qty qty = 0;
            std::uint32_t count = 0, prev = kNil;
            for (std::uint32_t n = L.head; n != kNil; n = pool_[n].next) {
                const Node& node = pool_[n];
                if (node.prev != prev || node.level != i || node.side != side || node.qty <= 0) return false;
                if (index_.find(node.id) != n) return false;
                qty += node.qty;
                ++count;
                prev = n;
            }
            if (prev != L.tail || qty != L.qty || count != L.count) return false;
            orders += count;
        }
        if (levels != expect_levels) return false;
        const std::int64_t want = side == Side::Buy ? last : first;
        return want == best;
    };
    if (!check(bids_, Side::Buy, bid_levels_, best_bid_)) return false;
    if (!check(asks_, Side::Sell, ask_levels_, best_ask_)) return false;
    if (orders != index_.size()) return false;
    if (best_bid_ >= 0 && best_ask_ >= 0 && best_bid_ >= best_ask_) return false;
    return true;
}

}  // namespace mds
