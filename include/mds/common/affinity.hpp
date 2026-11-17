// SPDX-License-Identifier: MIT
//
// Best-effort thread-to-core pinning. Pinning the producer and consumer to
// distinct physical cores keeps their L1/L2 warm and makes SPSC benchmark
// numbers reproducible. Failure is non-fatal: the call simply returns false.
#pragma once

#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace mds {

inline bool pin_current_thread(int cpu) noexcept {
    if (cpu < 0) return false;
#if defined(_WIN32)
    if (cpu >= static_cast<int>(sizeof(DWORD_PTR) * 8)) return false;
    return SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR{1} << cpu) != 0;
#elif defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#else
    (void)cpu;
    return false;  // macOS has no hard affinity API
#endif
}

}  // namespace mds
