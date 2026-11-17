// SPDX-License-Identifier: MIT
//
// Open-addressing hash map from 64-bit order ids to 32-bit pool indices.
//
// Why not std::unordered_map: every insert allocates a node, lookups chase a
// pointer into a separate allocation, and a rehash stalls for milliseconds.
// Here keys and values live in one flat array (12 bytes per slot, contiguous),
// lookups are a hash + a short linear probe in the same cache lines, and
// deletion uses backward-shift (no tombstones), so probe sequences never
// degrade over a long trading day. Capacity is reserved up front; growth
// doubles the table and only happens if the reservation was too small.
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace mds {

class OrderIndex {
public:
    static constexpr std::uint64_t kEmptyKey = std::numeric_limits<std::uint64_t>::max();
    static constexpr std::uint32_t kNotFound = std::numeric_limits<std::uint32_t>::max();

    explicit OrderIndex(std::size_t expected = 1 << 16) { reserve(expected); }

    void reserve(std::size_t expected) {
        std::size_t cap = 16;
        while (cap < expected * 2) cap <<= 1;  // max load factor 0.5
        if (cap > slots_.size()) rehash(cap);
    }

    [[nodiscard]] std::uint32_t find(std::uint64_t key) const noexcept {
        std::size_t i = hash(key) & mask_;
        while (true) {
            const Slot& s = slots_[i];
            if (s.key == key) return s.value;
            if (s.key == kEmptyKey) return kNotFound;
            i = (i + 1) & mask_;
        }
    }

    // Returns false if the key already exists (value unchanged).
    bool insert(std::uint64_t key, std::uint32_t value) {
        if ((size_ + 1) * 2 > slots_.size()) rehash(slots_.size() * 2);
        std::size_t i = hash(key) & mask_;
        while (slots_[i].key != kEmptyKey) {
            if (slots_[i].key == key) return false;
            i = (i + 1) & mask_;
        }
        slots_[i] = Slot{key, value};
        ++size_;
        return true;
    }

    bool erase(std::uint64_t key) noexcept {
        std::size_t i = hash(key) & mask_;
        while (slots_[i].key != key) {
            if (slots_[i].key == kEmptyKey) return false;
            i = (i + 1) & mask_;
        }
        // Backward-shift deletion: pull later entries of the probe chain back
        // into the hole so lookups never need tombstones.
        std::size_t hole = i;
        std::size_t j = i;
        while (true) {
            j = (j + 1) & mask_;
            if (slots_[j].key == kEmptyKey) break;
            const std::size_t home = hash(slots_[j].key) & mask_;
            // Move j into the hole unless its home lies cyclically in (hole, j].
            const bool between = hole <= j ? (home > hole && home <= j) : (home > hole || home <= j);
            if (!between) {
                slots_[hole] = slots_[j];
                hole = j;
            }
        }
        slots_[hole].key = kEmptyKey;
        --size_;
        return true;
    }

    void clear() noexcept {
        for (auto& s : slots_) s.key = kEmptyKey;
        size_ = 0;
    }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }

private:
    struct Slot {
        std::uint64_t key{kEmptyKey};
        std::uint32_t value{0};
    };

    static std::uint64_t hash(std::uint64_t x) noexcept {  // splitmix64 finaliser
        x ^= x >> 30;
        x *= 0xbf58476d1ce4e5b9ULL;
        x ^= x >> 27;
        x *= 0x94d049bb133111ebULL;
        x ^= x >> 31;
        return x;
    }

    void rehash(std::size_t new_cap) {
        std::vector<Slot> old;
        old.swap(slots_);
        slots_.assign(new_cap, Slot{});
        mask_ = new_cap - 1;
        size_ = 0;
        for (const Slot& s : old) {
            if (s.key != kEmptyKey) insert(s.key, s.value);
        }
    }

    std::vector<Slot> slots_;
    std::size_t mask_{0};
    std::size_t size_{0};
};

}  // namespace mds
