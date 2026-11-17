// SPDX-License-Identifier: MIT
//
// Core vocabulary types shared by every component.
//
// Prices are fixed-point integers (ticks), never floating point. A floating
// price key in std::map would make level lookup depend on rounding noise
// (0.1 + 0.2 != 0.3), so all arithmetic on the hot path is integral and
// conversion to double happens only at the edges (I/O, reporting).
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

namespace mds {

using Price = std::int64_t;      // fixed-point, 1 unit == 1 / kPriceScale
using Qty = std::int64_t;        // shares / contracts
using OrderId = std::uint64_t;   // unique per order for the lifetime of a book
using Timestamp = std::int64_t;  // nanoseconds since Unix epoch
using SymbolId = std::uint32_t;

inline constexpr std::int64_t kPriceScale = 10'000;  // 4 implied decimals
inline constexpr Price kNoPrice = std::numeric_limits<Price>::min();
inline constexpr Price kMaxPrice = std::numeric_limits<Price>::max();
inline constexpr Price kMinPrice = std::numeric_limits<Price>::min() + 1;

// One cache line on x86-64 and most AArch64 parts. We avoid
// std::hardware_destructive_interference_size because GCC warns that its value
// is ABI-unstable across -mtune settings.
inline constexpr std::size_t kCacheLine = 64;

inline constexpr Timestamp kNanosPerMicro = 1'000;
inline constexpr Timestamp kNanosPerMilli = 1'000'000;
inline constexpr Timestamp kNanosPerSecond = 1'000'000'000;

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

[[nodiscard]] constexpr Side opposite(Side s) noexcept {
    return s == Side::Buy ? Side::Sell : Side::Buy;
}

[[nodiscard]] constexpr std::string_view to_string(Side s) noexcept {
    return s == Side::Buy ? "BUY" : "SELL";
}

[[nodiscard]] inline double to_double(Price p) noexcept {
    return static_cast<double>(p) / static_cast<double>(kPriceScale);
}

[[nodiscard]] inline Price to_price(double px) noexcept {
    return static_cast<Price>(std::llround(px * static_cast<double>(kPriceScale)));
}

}  // namespace mds
