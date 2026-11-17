// SPDX-License-Identifier: MIT
//
// Time-stamp-counter clock: reads the CPU cycle counter (RDTSC, ~7 ns, no
// system call, no vDSO) and converts cycles to nanoseconds with a one-off
// calibration against steady_clock. This is how latency-critical systems
// timestamp hot paths: steady_clock costs ~20-25 ns per read, which would
// dominate sub-microsecond measurements.
//
// Valid on CPUs with an invariant TSC (every x86-64 server/desktop CPU of the
// last decade) - the counter ticks at a constant rate on all cores. On other
// architectures it falls back to steady_clock transparently.
#pragma once

#include <chrono>
#include <cstdint>
#include <thread>

#if defined(__x86_64__) || defined(_M_X64)
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
#define MDS_HAS_RDTSC 1
#endif

namespace mds {

class TscClock {
public:
    // Raw counter value (cycles, or ns on the fallback path).
    static inline std::uint64_t now() noexcept {
#if defined(MDS_HAS_RDTSC)
        return __rdtsc();
#else
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                              std::chrono::steady_clock::now().time_since_epoch())
                                              .count());
#endif
    }

    // Measures cycles per nanosecond over `ms` milliseconds. Call once at start-up.
    static void calibrate(int ms = 50) {
#if defined(MDS_HAS_RDTSC)
        const auto w0 = std::chrono::steady_clock::now();
        const std::uint64_t c0 = now();
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        const std::uint64_t c1 = now();
        const auto w1 = std::chrono::steady_clock::now();
        const double ns = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(w1 - w0).count());
        ns_per_cycle() = ns / static_cast<double>(c1 - c0);
#else
        (void)ms;
        ns_per_cycle() = 1.0;
#endif
    }

    static double to_ns(std::uint64_t cycles) noexcept {
        return static_cast<double>(cycles) * ns_per_cycle();
    }
    static double ghz() noexcept { return 1.0 / ns_per_cycle(); }
    static bool hardware() noexcept {
#if defined(MDS_HAS_RDTSC)
        return true;
#else
        return false;
#endif
    }

private:
    static double& ns_per_cycle() noexcept {
        static double v = 1.0;
        return v;
    }
};

}  // namespace mds
