// SPDX-License-Identifier: MIT
//
// Baseline bounded queue protected by a std::mutex.
//
// It deliberately has the same interface and the same storage as SpscRing
// (power-of-two circular buffer, non-blocking try_push / try_pop) so the
// benchmark isolates the cost of the synchronisation strategy rather than
// comparing a ring buffer against, say, a std::deque that allocates.
//
// What it pays per operation compared with the lock-free ring:
//   * lock():  an atomic RMW (lock cmpxchg) on the mutex word
//   * unlock(): another atomic RMW / store + possible futex wake
//   * the mutex word, head and tail all share one hot cache line that
//     ping-pongs between producer and consumer cores on EVERY operation
//   * under contention the kernel may park a thread (futex / SRWLock), which
//     costs microseconds rather than nanoseconds
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace mds {

template <class T>
class MutexQueue {
public:
    explicit MutexQueue(std::size_t capacity)
        : capacity_(round_up_pow2(capacity)),
          mask_(capacity_ - 1),
          slots_(std::make_unique<T[]>(capacity_)) {
        if (capacity == 0) throw std::invalid_argument("MutexQueue capacity must be > 0");
    }

    template <class U>
    [[nodiscard]] bool try_push(U&& value) {
        std::lock_guard<std::mutex> lock(mu_);
        if (tail_ - head_ == capacity_) return false;
        slots_[tail_ & mask_] = std::forward<U>(value);
        ++tail_;
        return true;
    }

    std::size_t try_push_n(const T* src, std::size_t n) {
        std::lock_guard<std::mutex> lock(mu_);
        const std::size_t free_slots = capacity_ - static_cast<std::size_t>(tail_ - head_);
        const std::size_t count = n < free_slots ? n : free_slots;
        for (std::size_t i = 0; i < count; ++i) slots_[(tail_ + i) & mask_] = src[i];
        tail_ += count;
        return count;
    }

    [[nodiscard]] bool try_pop(T& out) {
        std::lock_guard<std::mutex> lock(mu_);
        if (head_ == tail_) return false;
        out = std::move(slots_[head_ & mask_]);
        ++head_;
        return true;
    }

    std::size_t try_pop_n(T* dst, std::size_t max) {
        std::lock_guard<std::mutex> lock(mu_);
        const std::size_t avail = static_cast<std::size_t>(tail_ - head_);
        const std::size_t count = max < avail ? max : avail;
        for (std::size_t i = 0; i < count; ++i) dst[i] = std::move(slots_[(head_ + i) & mask_]);
        head_ += count;
        return count;
    }

    [[nodiscard]] std::size_t size_approx() const {
        std::lock_guard<std::mutex> lock(mu_);
        return static_cast<std::size_t>(tail_ - head_);
    }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] static constexpr const char* name() noexcept { return "mutex"; }

private:
    static std::size_t round_up_pow2(std::size_t v) noexcept {
        std::size_t p = 1;
        while (p < v) p <<= 1;
        return p;
    }

    const std::size_t capacity_;
    const std::size_t mask_;
    const std::unique_ptr<T[]> slots_;
    mutable std::mutex mu_;
    std::uint64_t head_{0};
    std::uint64_t tail_{0};
};

}  // namespace mds
