// SPDX-License-Identifier: MIT
//
// Nasdaq TotalView-ITCH 5.0 - binary market-data protocol.
//
// ITCH is the order-by-order feed Nasdaq publishes: every displayed order
// add, execution, cancel, delete and replace, for every symbol, as compact
// big-endian binary messages. This module provides
//
//   * a zero-copy parser (decode straight from the receive buffer, no
//     allocation, dispatch by the 1-byte message type),
//   * an encoder (used to produce synthetic ITCH and to test the parser),
//   * BinaryFILE framing: each message preceded by a 2-byte big-endian length,
//     the format of Nasdaq's published sample files.
//
// Message layouts follow the Nasdaq TotalView-ITCH 5.0 specification. All
// messages share an 11-byte header:
//
//   offset 0  Message Type        1  alpha
//          1  Stock Locate        2  uint16   (per-day symbol index)
//          3  Tracking Number     2  uint16
//          5  Timestamp           6  uint48   ns since midnight
//
// Prices are 4-byte unsigned integers with 4 implied decimals, the same fixed
// point as mds::Price, so no conversion is needed.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "mds/common/types.hpp"

namespace mds::itch {

// ---- big-endian helpers ----------------------------------------------------------
inline std::uint16_t be16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}
inline std::uint32_t be32(const std::uint8_t* p) noexcept {
    return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) | p[3];
}
inline std::uint64_t be48(const std::uint8_t* p) noexcept {
    return (std::uint64_t{be16(p)} << 32) | be32(p + 2);
}
inline std::uint64_t be64(const std::uint8_t* p) noexcept {
    return (std::uint64_t{be32(p)} << 32) | be32(p + 4);
}
inline void put16(std::uint8_t* p, std::uint16_t v) noexcept {
    p[0] = static_cast<std::uint8_t>(v >> 8);
    p[1] = static_cast<std::uint8_t>(v);
}
inline void put32(std::uint8_t* p, std::uint32_t v) noexcept {
    put16(p, static_cast<std::uint16_t>(v >> 16));
    put16(p + 2, static_cast<std::uint16_t>(v));
}
inline void put48(std::uint8_t* p, std::uint64_t v) noexcept {
    put16(p, static_cast<std::uint16_t>(v >> 32));
    put32(p + 2, static_cast<std::uint32_t>(v));
}
inline void put64(std::uint8_t* p, std::uint64_t v) noexcept {
    put32(p, static_cast<std::uint32_t>(v >> 32));
    put32(p + 4, static_cast<std::uint32_t>(v));
}

// ---- message lengths (excluding the 2-byte BinaryFILE length prefix) ------------
inline constexpr std::size_t kLenSystemEvent = 12;    // 'S'
inline constexpr std::size_t kLenStockDirectory = 39; // 'R'
inline constexpr std::size_t kLenAdd = 36;            // 'A'
inline constexpr std::size_t kLenAddMpid = 40;        // 'F'
inline constexpr std::size_t kLenExecuted = 31;       // 'E'
inline constexpr std::size_t kLenExecutedPrice = 36;  // 'C'
inline constexpr std::size_t kLenCancel = 23;         // 'X'
inline constexpr std::size_t kLenDelete = 19;         // 'D'
inline constexpr std::size_t kLenReplace = 35;        // 'U'
inline constexpr std::size_t kLenTrade = 44;          // 'P'

// ---- decoded messages -------------------------------------------------------------
struct Header {
    char type;
    std::uint16_t locate;
    std::uint16_t tracking;
    std::uint64_t ts;  // ns since midnight
};
struct SystemEvent { Header h; char code; };
struct StockDirectory { Header h; std::string_view stock; };  // 8 chars, space padded
struct AddOrder { Header h; std::uint64_t ref; Side side; std::uint32_t shares; std::string_view stock; std::uint32_t price; };
struct OrderExecuted { Header h; std::uint64_t ref; std::uint32_t shares; std::uint64_t match; };
struct OrderExecutedPrice { Header h; std::uint64_t ref; std::uint32_t shares; std::uint64_t match; bool printable; std::uint32_t price; };
struct OrderCancel { Header h; std::uint64_t ref; std::uint32_t shares; };
struct OrderDelete { Header h; std::uint64_t ref; };
struct OrderReplace { Header h; std::uint64_t orig_ref; std::uint64_t new_ref; std::uint32_t shares; std::uint32_t price; };
struct Trade { Header h; std::uint64_t ref; Side side; std::uint32_t shares; std::string_view stock; std::uint32_t price; std::uint64_t match; };

inline std::string_view trim_stock(const std::uint8_t* p) noexcept {
    std::size_t n = 8;
    while (n > 0 && p[n - 1] == ' ') --n;
    return std::string_view(reinterpret_cast<const char*>(p), n);
}

inline Header header(const std::uint8_t* p) noexcept {
    return Header{static_cast<char>(p[0]), be16(p + 1), be16(p + 3), be48(p + 5)};
}

enum class ParseResult : std::uint8_t { Handled, Ignored, Truncated };

// Decodes one message and calls the matching handler method. `H` provides
// on_system, on_directory, on_add, on_executed, on_executed_price,
// on_cancel, on_delete, on_replace and on_trade. Unknown / uninteresting
// message types are reported as Ignored (their length is known from framing).
template <class H>
inline ParseResult parse(const std::uint8_t* p, std::size_t len, H& h) {
    if (len < 11) return ParseResult::Truncated;
    const Header hd = header(p);
    switch (hd.type) {
        case 'A':
        case 'F':
            if (len < (hd.type == 'A' ? kLenAdd : kLenAddMpid)) return ParseResult::Truncated;
            h.on_add(AddOrder{hd, be64(p + 11), p[19] == 'S' ? Side::Sell : Side::Buy, be32(p + 20),
                              trim_stock(p + 24), be32(p + 32)});
            return ParseResult::Handled;
        case 'E':
            if (len < kLenExecuted) return ParseResult::Truncated;
            h.on_executed(OrderExecuted{hd, be64(p + 11), be32(p + 19), be64(p + 23)});
            return ParseResult::Handled;
        case 'C':
            if (len < kLenExecutedPrice) return ParseResult::Truncated;
            h.on_executed_price(OrderExecutedPrice{hd, be64(p + 11), be32(p + 19), be64(p + 23), p[31] == 'Y', be32(p + 32)});
            return ParseResult::Handled;
        case 'X':
            if (len < kLenCancel) return ParseResult::Truncated;
            h.on_cancel(OrderCancel{hd, be64(p + 11), be32(p + 19)});
            return ParseResult::Handled;
        case 'D':
            if (len < kLenDelete) return ParseResult::Truncated;
            h.on_delete(OrderDelete{hd, be64(p + 11)});
            return ParseResult::Handled;
        case 'U':
            if (len < kLenReplace) return ParseResult::Truncated;
            h.on_replace(OrderReplace{hd, be64(p + 11), be64(p + 19), be32(p + 27), be32(p + 31)});
            return ParseResult::Handled;
        case 'P':
            if (len < kLenTrade) return ParseResult::Truncated;
            h.on_trade(Trade{hd, be64(p + 11), p[19] == 'S' ? Side::Sell : Side::Buy, be32(p + 20),
                             trim_stock(p + 24), be32(p + 32), be64(p + 36)});
            return ParseResult::Handled;
        case 'R':
            if (len < kLenStockDirectory) return ParseResult::Truncated;
            h.on_directory(StockDirectory{hd, trim_stock(p + 11)});
            return ParseResult::Handled;
        case 'S':
            if (len < kLenSystemEvent) return ParseResult::Truncated;
            h.on_system(SystemEvent{hd, static_cast<char>(p[11])});
            return ParseResult::Handled;
        default:
            return ParseResult::Ignored;
    }
}

// ---- encoder ------------------------------------------------------------------------
// Appends BinaryFILE-framed messages (2-byte length + body) to a byte buffer.
class Writer {
public:
    explicit Writer(std::vector<std::uint8_t>& out) : out_(out) {}

    void system_event(std::uint16_t locate, std::uint64_t ts, char code) {
        auto* p = begin('S', locate, ts, kLenSystemEvent);
        p[11] = static_cast<std::uint8_t>(code);
    }
    void stock_directory(std::uint16_t locate, std::uint64_t ts, std::string_view stock) {
        auto* p = begin('R', locate, ts, kLenStockDirectory);
        put_stock(p + 11, stock);
        p[19] = 'Q';                 // market category: NASDAQ Global Select
        p[20] = 'N';                 // financial status: normal
        put32(p + 21, 100);          // round lot size
        p[25] = 'N';                 // round lots only
        std::memset(p + 26, ' ', kLenStockDirectory - 26);
    }
    void add(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref, Side side, std::uint32_t shares,
             std::string_view stock, std::uint32_t price) {
        auto* p = begin('A', locate, ts, kLenAdd);
        put64(p + 11, ref);
        p[19] = side == Side::Buy ? 'B' : 'S';
        put32(p + 20, shares);
        put_stock(p + 24, stock);
        put32(p + 32, price);
    }
    void executed(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref, std::uint32_t shares, std::uint64_t match) {
        auto* p = begin('E', locate, ts, kLenExecuted);
        put64(p + 11, ref);
        put32(p + 19, shares);
        put64(p + 23, match);
    }
    void cancel(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref, std::uint32_t shares) {
        auto* p = begin('X', locate, ts, kLenCancel);
        put64(p + 11, ref);
        put32(p + 19, shares);
    }
    void del(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref) {
        auto* p = begin('D', locate, ts, kLenDelete);
        put64(p + 11, ref);
    }
    void replace(std::uint16_t locate, std::uint64_t ts, std::uint64_t orig, std::uint64_t fresh,
                 std::uint32_t shares, std::uint32_t price) {
        auto* p = begin('U', locate, ts, kLenReplace);
        put64(p + 11, orig);
        put64(p + 19, fresh);
        put32(p + 27, shares);
        put32(p + 31, price);
    }

    [[nodiscard]] std::uint64_t messages() const noexcept { return messages_; }

private:
    std::uint8_t* begin(char type, std::uint16_t locate, std::uint64_t ts, std::size_t len) {
        const std::size_t at = out_.size();
        out_.resize(at + 2 + len);
        std::uint8_t* frame = out_.data() + at;
        put16(frame, static_cast<std::uint16_t>(len));
        std::uint8_t* p = frame + 2;
        p[0] = static_cast<std::uint8_t>(type);
        put16(p + 1, locate);
        put16(p + 3, static_cast<std::uint16_t>(tracking_++));
        put48(p + 5, ts);
        ++messages_;
        return p;
    }
    static void put_stock(std::uint8_t* p, std::string_view s) {
        std::memset(p, ' ', 8);
        std::memcpy(p, s.data(), s.size() < 8 ? s.size() : 8);
    }

    std::vector<std::uint8_t>& out_;
    std::uint32_t tracking_{0};
    std::uint64_t messages_{0};
};

// Iterates BinaryFILE framing over an in-memory buffer:
//   for_each_frame(buf, n, [](const uint8_t* msg, size_t len) { ... });
// Returns the number of bytes consumed (a trailing partial frame is left).
template <class F>
inline std::size_t for_each_frame(const std::uint8_t* buf, std::size_t n, F&& f) {
    std::size_t pos = 0;
    while (pos + 2 <= n) {
        const std::size_t len = be16(buf + pos);
        if (pos + 2 + len > n) break;
        f(buf + pos + 2, len);
        pos += 2 + len;
    }
    return pos;
}

}  // namespace mds::itch
