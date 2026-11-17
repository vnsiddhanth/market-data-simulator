// SPDX-License-Identifier: MIT
//
// MarketEvent: the unit of work that flows through the pipeline, gets
// persisted to Redis and is replayed by the backtester.
//
// It is a 40-byte trivially-copyable POD so that
//   * the SPSC ring can move it with a plain memcpy,
//   * the Redis chunk codec can serialise it with a fixed little-endian layout,
//   * the Python tooling can decode it with struct.unpack("<qQqqIBBH").
#pragma once

#include <cstdint>
#include <string_view>
#include <type_traits>

#include "mds/common/types.hpp"

namespace mds {

enum class EventType : std::uint8_t {
    Add = 0,     // new resting limit order (may cross and trade first)
    Cancel = 1,  // remove a resting order
    Modify = 2,  // change resting quantity (down keeps priority, up loses it)
    Market = 3,  // aggressive order with no limit price
};

[[nodiscard]] constexpr std::string_view to_string(EventType t) noexcept {
    switch (t) {
        case EventType::Add: return "ADD";
        case EventType::Cancel: return "CANCEL";
        case EventType::Modify: return "MODIFY";
        case EventType::Market: return "MARKET";
    }
    return "?";
}

struct MarketEvent {
    Timestamp ts{0};       // exchange timestamp, ns
    OrderId id{0};         // order the event refers to
    Price price{0};        // limit price (ignored for Market/Cancel)
    Qty qty{0};            // size (new size for Modify)
    SymbolId symbol{0};
    EventType type{EventType::Add};
    Side side{Side::Buy};
    std::uint16_t flags{0};  // reserved; keeps the struct padding explicit

    [[nodiscard]] static constexpr MarketEvent add(Timestamp ts, OrderId id, Side s, Price px,
                                                   Qty q, SymbolId sym = 0) noexcept {
        return MarketEvent{ts, id, px, q, sym, EventType::Add, s, 0};
    }
    [[nodiscard]] static constexpr MarketEvent cancel(Timestamp ts, OrderId id,
                                                      SymbolId sym = 0) noexcept {
        return MarketEvent{ts, id, 0, 0, sym, EventType::Cancel, Side::Buy, 0};
    }
    [[nodiscard]] static constexpr MarketEvent modify(Timestamp ts, OrderId id, Qty new_qty,
                                                      SymbolId sym = 0) noexcept {
        return MarketEvent{ts, id, 0, new_qty, sym, EventType::Modify, Side::Buy, 0};
    }
    [[nodiscard]] static constexpr MarketEvent market(Timestamp ts, OrderId id, Side s, Qty q,
                                                      SymbolId sym = 0) noexcept {
        return MarketEvent{ts, id, 0, q, sym, EventType::Market, s, 0};
    }
};

static_assert(std::is_trivially_copyable_v<MarketEvent>, "must be memcpy-able");
static_assert(std::is_standard_layout_v<MarketEvent>, "must have a stable layout");
static_assert(sizeof(MarketEvent) == 40, "wire format (and the Python codec) assume 40 bytes");

}  // namespace mds
