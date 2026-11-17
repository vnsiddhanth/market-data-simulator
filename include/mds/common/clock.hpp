// SPDX-License-Identifier: MIT
//
// Timing helpers: a monotonic nanosecond clock for latency measurement and a
// CPU "pause" hint for spin loops.
#pragma once

#include <chrono>
#include <cstdint>
#include <thread>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#define MDS_HAS_MM_PAUSE 1
#endif

namespace mds {

// Monotonic clock in nanoseconds. steady_clock is backed by
// clock_gettime(CLOCK_MONOTONIC) / QueryPerformanceCounter, ~20 ns per call,
// which is good enough for per-message latency sampling. A raw RDTSC clock
// would be cheaper but requires invariant-TSC calibration; see
// docs/TECHNICAL_NOTES.md.
[[nodiscard]] inline std::int64_t mono_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Hint to the CPU that we are in a spin-wait loop. On x86 PAUSE reduces power
// and avoids a memory-order-violation pipeline flush when the awaited cache
// line finally changes; on SMT cores it also yields execution resources to the
// sibling hyper-thread.
inline void cpu_relax() noexcept {
#if defined(MDS_HAS_MM_PAUSE)
    _mm_pause();
#elif defined(__aarch64__) || defined(__arm__)
    asm volatile("yield" ::: "memory");
#else
    std::this_thread::yield();
#endif
}

// Bounded exponential back-off used by the pipeline's wait loops: spin with
// PAUSE first (lowest wake-up latency), then fall back to yielding the time
// slice so an oversubscribed machine does not live-lock.
class Backoff {
public:
    void pause() noexcept {
        if (spins_ < kSpinLimit) {
            for (std::uint32_t i = 0; i < (1u << spins_); ++i) cpu_relax();
            ++spins_;
        } else {
            std::this_thread::yield();
        }
    }
    void reset() noexcept { spins_ = 0; }

private:
    static constexpr std::uint32_t kSpinLimit = 6;  // up to 64 PAUSEs per round
    std::uint32_t spins_{0};
};

}  // namespace mds
