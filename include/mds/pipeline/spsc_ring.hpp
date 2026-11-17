// SPDX-License-Identifier: MIT
//
// Bounded, wait-free single-producer / single-consumer ring buffer.
//
// Design
// ------
// * Capacity is a power of two; head/tail are free-running 64-bit counters and
//   the slot index is `counter & mask`. A 64-bit counter at 1e9 msgs/s wraps
//   after ~585 years, so "full" is simply `tail - head == capacity` and no slot
//   has to be sacrificed to tell full from empty.
//
// * Only the producer writes `tail_`; only the consumer writes `head_`. Each
//   side therefore needs exactly ONE release-store per publish and ONE acquire-
//   load of the other side's counter - no read-modify-write (CAS / fetch_add),
//   no lock prefix, no fences beyond what x86 TSO gives for free.
//
// * Memory ordering:
//     producer: write slot  ->  tail_.store(t + 1, release)
//     consumer: tail_.load(acquire)  ->  read slot  ->  head_.store(h + 1, release)
//     producer: head_.load(acquire) before overwriting a slot
//   The release/acquire pair on tail_ makes the slot contents visible to the
//   consumer; the pair on head_ guarantees the consumer has finished reading a
//   slot before the producer reuses it.
//
// * Index caching (Rigtorp / Vyukov trick): the producer keeps a private copy
//   of the last head_ it observed and only re-reads the shared head_ when the
//   cached value says the ring is full (and symmetrically for the consumer).
//   In steady state each side touches the other side's cache line roughly once
//   per `capacity` messages instead of once per message, which removes most of
//   the cross-core coherence traffic.
//
// * Layout: producer-owned and consumer-owned fields live on separate cache
//   lines (plus a padding line against adjacent-line prefetch), so the two
//   threads never false-share.
//
// * Batch APIs (`try_push_n`, `try_pop_n`) publish many slots with a single
//   release-store, amortising the remaining synchronisation further.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "mds/common/types.hpp"

namespace mds {

template <class T>
class SpscRing {
    static_assert(std::is_nothrow_move_assignable_v<T>, "slots are move-assigned in place");
    static_assert(std::is_default_constructible_v<T>, "slots are pre-constructed");

public:
    explicit SpscRing(std::size_t capacity)
        : capacity_(round_up_pow2(capacity)),
          mask_(capacity_ - 1),
          slots_(std::make_unique<T[]>(capacity_)) {
        if (capacity == 0) throw std::invalid_argument("SpscRing capacity must be > 0");
    }

    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    // ---- producer side (exactly one thread) --------------------------------

    template <class U>
    [[nodiscard]] bool try_push(U&& value) noexcept(std::is_nothrow_assignable_v<T&, U&&>) {
        const std::uint64_t tail = prod_.tail.load(std::memory_order_relaxed);
        if (tail - prod_.cached_head == capacity_) {
            prod_.cached_head = cons_.head.load(std::memory_order_acquire);
            if (tail - prod_.cached_head == capacity_) return false;  // genuinely full
        }
        slots_[tail & mask_] = std::forward<U>(value);
        prod_.tail.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Pushes up to `n` items from `src`; returns how many were enqueued.
    std::size_t try_push_n(const T* src, std::size_t n) noexcept {
        const std::uint64_t tail = prod_.tail.load(std::memory_order_relaxed);
        std::size_t free_slots = capacity_ - static_cast<std::size_t>(tail - prod_.cached_head);
        if (free_slots < n) {
            prod_.cached_head = cons_.head.load(std::memory_order_acquire);
            free_slots = capacity_ - static_cast<std::size_t>(tail - prod_.cached_head);
        }
        const std::size_t count = n < free_slots ? n : free_slots;
        for (std::size_t i = 0; i < count; ++i) slots_[(tail + i) & mask_] = src[i];
        if (count) prod_.tail.store(tail + count, std::memory_order_release);
        return count;
    }

    // ---- consumer side (exactly one thread) --------------------------------

    [[nodiscard]] bool try_pop(T& out) noexcept {
        const std::uint64_t head = cons_.head.load(std::memory_order_relaxed);
        if (head == cons_.cached_tail) {
            cons_.cached_tail = prod_.tail.load(std::memory_order_acquire);
            if (head == cons_.cached_tail) return false;  // genuinely empty
        }
        out = std::move(slots_[head & mask_]);
        cons_.head.store(head + 1, std::memory_order_release);
        return true;
    }

    // Pops up to `max` items into `dst`; returns how many were dequeued.
    std::size_t try_pop_n(T* dst, std::size_t max) noexcept {
        const std::uint64_t head = cons_.head.load(std::memory_order_relaxed);
        std::size_t avail = static_cast<std::size_t>(cons_.cached_tail - head);
        if (avail < max) {
            cons_.cached_tail = prod_.tail.load(std::memory_order_acquire);
            avail = static_cast<std::size_t>(cons_.cached_tail - head);
        }
        const std::size_t count = max < avail ? max : avail;
        for (std::size_t i = 0; i < count; ++i) dst[i] = std::move(slots_[(head + i) & mask_]);
        if (count) cons_.head.store(head + count, std::memory_order_release);
        return count;
    }

    // Zero-copy peek: pointer to the oldest element or nullptr. Must be
    // followed by pop_front() once the caller is done with the element.
    [[nodiscard]] T* front() noexcept {
        const std::uint64_t head = cons_.head.load(std::memory_order_relaxed);
        if (head == cons_.cached_tail) {
            cons_.cached_tail = prod_.tail.load(std::memory_order_acquire);
            if (head == cons_.cached_tail) return nullptr;
        }
        return &slots_[head & mask_];
    }
    void pop_front() noexcept {
        const std::uint64_t head = cons_.head.load(std::memory_order_relaxed);
        cons_.head.store(head + 1, std::memory_order_release);
    }

    // ---- either side (approximate under concurrency) -----------------------

    [[nodiscard]] std::size_t size_approx() const noexcept {
        const std::uint64_t tail = prod_.tail.load(std::memory_order_acquire);
        const std::uint64_t head = cons_.head.load(std::memory_order_acquire);
        return static_cast<std::size_t>(tail - head);
    }
    [[nodiscard]] bool empty_approx() const noexcept { return size_approx() == 0; }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    [[nodiscard]] static constexpr const char* name() noexcept { return "spsc-lockfree"; }

private:
    static std::size_t round_up_pow2(std::size_t v) noexcept {
        std::size_t p = 1;
        while (p < v) p <<= 1;
        return p;
    }

    // Read-only after construction: shared freely by both threads.
    const std::size_t capacity_;
    const std::size_t mask_;
    const std::unique_ptr<T[]> slots_;

    struct alignas(kCacheLine) ProducerState {
        std::atomic<std::uint64_t> tail{0};
        std::uint64_t cached_head{0};  // producer-private snapshot of head
    };
    struct alignas(kCacheLine) ConsumerState {
        std::atomic<std::uint64_t> head{0};
        std::uint64_t cached_tail{0};  // consumer-private snapshot of tail
    };

    // Extra line between the two hot lines defeats the L2 adjacent-line
    // (spatial) prefetcher on Intel parts, which pulls cache lines in 128-byte
    // pairs and would otherwise re-introduce false sharing.
    alignas(kCacheLine) char pad0_[kCacheLine]{};
    ProducerState prod_;
    alignas(kCacheLine) char pad1_[kCacheLine]{};
    ConsumerState cons_;
    alignas(kCacheLine) char pad2_[kCacheLine]{};

    static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                  "SPSC ring requires lock-free 64-bit atomics");
};

}  // namespace mds
