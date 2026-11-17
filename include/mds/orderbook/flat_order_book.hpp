// SPDX-License-Identifier: MIT
//
// FlatOrderBook - an HFT-style limit order book.
//
// Same price-time-priority semantics as OrderBook, built the way latency-
// sensitive trading systems build books:
//
//   * Price ladder: a flat array of levels indexed by (price - base) / tick.
//     Finding a level is one subtraction and one shift - no tree walk, no
//     pointer chasing, no allocation for a new price.
//   * Best bid/ask: cached indices. When the best level empties, a bitmap of
//     non-empty levels finds the next one with TZCNT/LZCNT, 64 levels per
//     instruction, instead of scanning empty levels.
//   * Orders: a pre-allocated pool of fixed-size nodes linked into per-level
//     FIFO lists by 32-bit indices (intrusive doubly-linked list). Cancels
//     unlink in O(1) exactly - no tombstones, no compaction pauses.
//   * Id lookup: open-addressing hash (OrderIndex) - flat, no per-order heap
//     node.
//   * Zero allocations on the hot path once capacity is reserved.
//
// The ladder covers `window` ticks around a base price. If an order arrives
// outside the window the ladder is re-centred (O(levels + orders), rare); a
// price that cannot fit even after re-centring is rejected.
//
// Also exposes the passive-side operations an exchange data feed (ITCH)
// describes directly: execute(), reduce() and replace() of a resting order.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "mds/common/bits.hpp"
#include "mds/common/event.hpp"
#include "mds/common/flat_hash.hpp"
#include "mds/common/types.hpp"
#include "mds/orderbook/order_book.hpp"  // Fill, LevelView, BookResult, BookStats

namespace mds {

struct FlatBookConfig {
    Price tick{100};                   // price units per tick ($0.01 at 4 implied decimals)
    std::uint32_t window_ticks{1u << 15};  // ladder size (32768 ticks = $327.68 at 1c)
    std::size_t expected_orders{1u << 16};
};

class FlatOrderBook {
public:
    explicit FlatOrderBook(FlatBookConfig cfg = {}, SymbolId symbol = 0);

    // ---- order entry (same contract as OrderBook) -------------------------------
    BookResult add_limit(OrderId id, Side side, Price price, Qty qty, Timestamp ts,
                         std::vector<Fill>& fills);
    Qty execute_market(OrderId id, Side side, Qty qty, Timestamp ts, std::vector<Fill>& fills);
    BookResult cancel(OrderId id);
    BookResult modify(OrderId id, Qty new_qty, Timestamp ts);
    BookResult apply(const MarketEvent& ev, std::vector<Fill>& fills);

    // ---- exchange-feed (passive side) operations ---------------------------------
    // A resting order traded `qty` shares (ITCH 'E'/'C'). Emits the fill.
    BookResult execute(OrderId id, Qty qty, Timestamp ts, std::vector<Fill>& fills);
    // A resting order lost `qty` shares without trading (ITCH 'X').
    BookResult reduce(OrderId id, Qty qty);
    // Cancel-replace with a new id, price and size; loses priority (ITCH 'U').
    BookResult replace(OrderId old_id, OrderId new_id, Price price, Qty qty, Timestamp ts);

    // ---- market data -------------------------------------------------------------
    [[nodiscard]] std::optional<Price> best_bid() const noexcept;
    [[nodiscard]] std::optional<Price> best_ask() const noexcept;
    [[nodiscard]] std::optional<double> mid_price() const noexcept;
    [[nodiscard]] std::optional<Price> spread() const noexcept;
    [[nodiscard]] Qty volume_at(Side side, Price price) const noexcept;
    [[nodiscard]] Qty best_bid_qty() const noexcept;
    [[nodiscard]] Qty best_ask_qty() const noexcept;
    void depth_into(Side side, std::size_t levels, std::vector<LevelView>& out) const;
    [[nodiscard]] std::vector<LevelView> depth(Side side, std::size_t levels) const;
    [[nodiscard]] double imbalance(std::size_t levels = 5) const noexcept;

    [[nodiscard]] std::size_t order_count() const noexcept { return index_.size(); }
    [[nodiscard]] std::size_t level_count(Side side) const noexcept {
        return side == Side::Buy ? bid_levels_ : ask_levels_;
    }
    [[nodiscard]] bool empty() const noexcept { return index_.size() == 0; }
    [[nodiscard]] bool contains(OrderId id) const noexcept {
        return index_.find(id) != OrderIndex::kNotFound;
    }
    [[nodiscard]] Qty order_qty(OrderId id) const noexcept;
    [[nodiscard]] std::optional<Price> order_price(OrderId id) const noexcept;
    [[nodiscard]] const BookStats& stats() const noexcept { return stats_; }
    [[nodiscard]] std::uint64_t recenters() const noexcept { return recenters_; }
    [[nodiscard]] Timestamp last_update() const noexcept { return last_ts_; }
    [[nodiscard]] Price tick() const noexcept { return cfg_.tick; }

    void clear();
    [[nodiscard]] bool validate() const;

private:
    static constexpr std::uint32_t kNil = 0xFFFFFFFFu;

    struct Node {  // 48 bytes; pool-allocated
        OrderId id;
        Qty qty;
        Timestamp ts;
        std::uint32_t prev;
        std::uint32_t next;
        std::uint32_t level;  // ladder index
        Side side;
    };
    struct Level {  // 24 bytes
        Qty qty{0};
        std::uint32_t count{0};
        std::uint32_t head{kNil};
        std::uint32_t tail{kNil};
    };
    struct Ladder {
        std::vector<Level> levels;
        ScanBitset nonempty;
    };

    // Index of `price` in the ladder, re-centring if needed; -1 if impossible.
    std::int64_t level_index(Price price, bool may_recenter);
    [[nodiscard]] Price level_price(std::uint32_t idx) const noexcept {
        return base_ + static_cast<Price>(idx) * cfg_.tick;
    }
    bool recenter(Price around);

    std::uint32_t alloc_node();
    void free_node(std::uint32_t n) noexcept;
    void link_back(Ladder& ladder, std::uint32_t level, std::uint32_t n);
    void unlink(Ladder& ladder, std::uint32_t n);
    void remove_order(std::uint32_t n);  // unlink + free + index erase + best update
    void on_level_emptied(Side side, std::uint32_t level);
    void rest(OrderId id, Side side, std::uint32_t level, Qty qty, Timestamp ts);

    Qty match(Side aggressor, OrderId taker, Price limit, Qty qty, Timestamp ts,
              std::vector<Fill>& fills);

    FlatBookConfig cfg_;
    SymbolId symbol_;
    Price base_{0};
    bool based_{false};
    Ladder bids_;
    Ladder asks_;
    std::int64_t best_bid_{-1};  // ladder index, -1 == empty
    std::int64_t best_ask_{-1};
    std::size_t bid_levels_{0};
    std::size_t ask_levels_{0};

    std::vector<Node> pool_;
    std::uint32_t free_head_{kNil};
    OrderIndex index_;

    Timestamp last_ts_{0};
    std::uint64_t recenters_{0};
    BookStats stats_;
};

}  // namespace mds
