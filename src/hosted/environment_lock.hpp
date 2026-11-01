#pragma once

// The lock every environment access holds (environment.cpp), shared with
// code that makes the host read the environment itself: localtime_r and
// tzset read TZ, and must not race with set_variable.
//
// A spin lock rather than a Mutex: every holder runs a few hundred
// nanoseconds of host calls at most and contention is rare, so an atomic
// exchange to take it and a store to release it cost a fraction of a
// mutex. A waiter yields its time slice between attempts, so a holder that
// is descheduled is not starved by spinning waiters.

#include "stx_auxlib/threads.hpp"

#include <atomic>
#include <cstdint>

namespace stx_auxlib::environment_detail {

class SpinLock final {
public:
    SpinLock() noexcept = default;
    SpinLock(const SpinLock&) = delete;
    SpinLock& operator=(const SpinLock&) = delete;

    void lock() noexcept {
        while (held_.exchange(true, std::memory_order_acquire)) {
            while (held_.load(std::memory_order_relaxed)) yield_thread();
        }
    }
    void unlock() noexcept { held_.store(false, std::memory_order_release); }

private:
    std::atomic<bool> held_{false};
};

// Holds a SpinLock for its own lifetime.
class SpinGuard final {
public:
    explicit SpinGuard(SpinLock& lock) noexcept : lock_(lock) { lock_.lock(); }
    ~SpinGuard() { lock_.unlock(); }
    SpinGuard(const SpinGuard&) = delete;
    SpinGuard& operator=(const SpinGuard&) = delete;

private:
    SpinLock& lock_;
};

// Created on first use and never destroyed.
[[nodiscard]] SpinLock& environment_lock() noexcept;

// Incremented, under environment_lock, by every successful set_variable and
// remove_variable, so readers of the host's environment (tzset) can tell
// whether it changed. Read under the same lock. Wraps after 2^64 changes,
// which only risks one missed tzset.
[[nodiscard]] std::uint64_t& environment_generation() noexcept;

} // namespace stx_auxlib::environment_detail
