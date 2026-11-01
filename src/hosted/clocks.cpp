// The hosted clocks: std::chrono::steady_clock and std::chrono::system_clock.
// Since C++20, system_clock's epoch is the Unix epoch on every host, so
// converting its duration to nanoseconds is all WallClock needs.

#include "stx_auxlib/clocks.hpp"

#include <chrono>

namespace stx_auxlib {

MonotonicClock::time_point MonotonicClock::now() noexcept {
    const std::chrono::steady_clock::time_point host = std::chrono::steady_clock::now();
    const MonotonicClock::duration since_epoch =
        std::chrono::duration_cast<MonotonicClock::duration>(host.time_since_epoch());
    const MonotonicClock::time_point result(since_epoch);
    return result;
}

WallClock::time_point WallClock::now() noexcept {
    const std::chrono::system_clock::time_point host = std::chrono::system_clock::now();
    const WallClock::duration since_epoch =
        std::chrono::duration_cast<WallClock::duration>(host.time_since_epoch());
    const WallClock::time_point result(since_epoch);
    return result;
}

} // namespace stx_auxlib
