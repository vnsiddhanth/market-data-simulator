// SPDX-License-Identifier: MIT
//
// Bit-scan helpers and a bitset that finds the next/previous set bit in a
// handful of instructions (TZCNT/LZCNT on x86, CLZ/RBIT on ARM). The flat
// order book uses it to locate the next non-empty price level after the best
// level empties, without walking empty levels one by one.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace mds {

// Precondition for both: x != 0.
inline int ctz64(std::uint64_t x) noexcept {
#if defined(_MSC_VER)
    unsigned long i;
    _BitScanForward64(&i, x);
    return static_cast<int>(i);
#else
    return __builtin_ctzll(x);
#endif
}

inline int msb64(std::uint64_t x) noexcept {  // index of the highest set bit
#if defined(_MSC_VER)
    unsigned long i;
    _BitScanReverse64(&i, x);
    return static_cast<int>(i);
#else
    return 63 - __builtin_clzll(x);
#endif
}

class ScanBitset {
public:
    explicit ScanBitset(std::size_t bits = 0) { resize(bits); }

    void resize(std::size_t bits) {
        bits_ = bits;
        words_.assign((bits + 63) / 64, 0);
    }
    void clear_all() { std::fill(words_.begin(), words_.end(), 0); }
    [[nodiscard]] std::size_t size() const noexcept { return bits_; }

    void set(std::size_t i) noexcept { words_[i >> 6] |= (std::uint64_t{1} << (i & 63)); }
    void reset(std::size_t i) noexcept { words_[i >> 6] &= ~(std::uint64_t{1} << (i & 63)); }
    [[nodiscard]] bool test(std::size_t i) const noexcept {
        return (words_[i >> 6] >> (i & 63)) & 1u;
    }

    // Lowest set index >= i, or -1.
    [[nodiscard]] std::int64_t next_at_or_after(std::size_t i) const noexcept {
        if (i >= bits_) return -1;
        std::size_t w = i >> 6;
        std::uint64_t word = words_[w] & (~std::uint64_t{0} << (i & 63));
        while (true) {
            if (word) return static_cast<std::int64_t>((w << 6) + static_cast<std::size_t>(ctz64(word)));
            if (++w >= words_.size()) return -1;
            word = words_[w];
        }
    }

    // Highest set index <= i, or -1.
    [[nodiscard]] std::int64_t prev_at_or_before(std::size_t i) const noexcept {
        if (bits_ == 0) return -1;
        if (i >= bits_) i = bits_ - 1;
        std::size_t w = i >> 6;
        const unsigned shift = 63u - static_cast<unsigned>(i & 63);
        std::uint64_t word = words_[w] & (~std::uint64_t{0} >> shift);
        while (true) {
            if (word) return static_cast<std::int64_t>((w << 6) + static_cast<std::size_t>(msb64(word)));
            if (w == 0) return -1;
            word = words_[--w];
        }
    }

private:
    std::size_t bits_{0};
    std::vector<std::uint64_t> words_;
};

}  // namespace mds
