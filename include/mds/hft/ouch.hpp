// SPDX-License-Identifier: MIT
//
// Nasdaq OUCH 4.2 order-entry messages (client -> exchange), binary,
// big-endian. Only the two messages a quoting strategy needs:
//
//   Enter Order 'O' (49 bytes)            Cancel Order 'X' (19 bytes)
//   off len field                         off len field
//    0   1  type 'O'                       0   1  type 'X'
//    1  14  order token (alpha)            1  14  order token
//   15   1  side 'B'/'S'                  15   4  shares (new size; 0 = cancel all)
//   16   4  shares
//   20   8  stock (alpha, space padded)
//   28   4  price (4 implied decimals)
//   32   4  time in force (0 = IOC, 99999 = day)
//   36   4  firm
//   40   1  display ('Y' visible)
//   41   1  capacity ('P' principal)
//   42   1  intermarket sweep ('N')
//   43   4  minimum quantity
//   47   1  cross type ('N')
//   48   1  customer type ('R')
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "mds/common/types.hpp"
#include "mds/itch/itch.hpp"  // big-endian helpers

namespace mds::hft::ouch {

inline constexpr std::size_t kLenEnterOrder = 49;
inline constexpr std::size_t kLenCancelOrder = 19;
inline constexpr std::uint32_t kTifIoc = 0;
inline constexpr std::uint32_t kTifDay = 99'999;

inline void put_alpha(std::uint8_t* p, std::size_t n, std::string_view s) {
    std::memset(p, ' ', n);
    std::memcpy(p, s.data(), s.size() < n ? s.size() : n);
}

// Order tokens must be unique per session: a fixed prefix + base-36 counter.
inline void make_token(std::uint8_t* p, std::uint64_t seq) {
    static constexpr char kDigits[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    std::memset(p, '0', 14);
    p[0] = 'M';
    p[1] = 'D';
    p[2] = 'S';
    for (int i = 13; i >= 3 && seq; --i, seq /= 36) p[i] = static_cast<std::uint8_t>(kDigits[seq % 36]);
}

inline std::size_t enter_order(std::uint8_t* p, std::uint64_t token_seq, Side side, std::uint32_t shares,
                               std::string_view stock, std::uint32_t price, std::uint32_t tif) {
    p[0] = 'O';
    make_token(p + 1, token_seq);
    p[15] = side == Side::Buy ? 'B' : 'S';
    itch::put32(p + 16, shares);
    put_alpha(p + 20, 8, stock);
    itch::put32(p + 28, price);
    itch::put32(p + 32, tif);
    put_alpha(p + 36, 4, "MDSF");
    p[40] = 'Y';
    p[41] = 'P';
    p[42] = 'N';
    itch::put32(p + 43, 0);
    p[47] = 'N';
    p[48] = 'R';
    return kLenEnterOrder;
}

inline std::size_t cancel_order(std::uint8_t* p, std::uint64_t token_seq, std::uint32_t shares) {
    p[0] = 'X';
    make_token(p + 1, token_seq);
    itch::put32(p + 15, shares);
    return kLenCancelOrder;
}

}  // namespace mds::hft::ouch
