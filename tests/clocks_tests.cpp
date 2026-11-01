// Tests of stx_auxlib's clocks (include/stx_auxlib/clocks.hpp).

#include "stx_auxlib/clocks.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

void require(const bool condition, const char* const message) {
    if (condition) return;
    std::fprintf(stderr, "stx_auxlib clocks: %s\n", message);
    std::abort();
}

void monotonic_never_decreases() {
    stx_auxlib::MonotonicClock::time_point previous = stx_auxlib::MonotonicClock::now();
    for (int read = 0; read < 100000; ++read) {
        const stx_auxlib::MonotonicClock::time_point next = stx_auxlib::MonotonicClock::now();
        require(next >= previous, "the monotonic clock went backwards");
        previous = next;
    }
}

// The hosted clocks read the standard clocks; they must agree closely.
void agrees_with_host() {
    const std::chrono::steady_clock::time_point host_steady = std::chrono::steady_clock::now();
    const stx_auxlib::MonotonicClock::time_point steady = stx_auxlib::MonotonicClock::now();
    const std::chrono::nanoseconds steady_gap =
        steady.time_since_epoch() -
        std::chrono::duration_cast<std::chrono::nanoseconds>(host_steady.time_since_epoch());
    require(steady_gap >= std::chrono::nanoseconds(0) && steady_gap < std::chrono::seconds(1),
            "the monotonic clock disagrees with steady_clock");

    const std::chrono::system_clock::time_point host_wall = std::chrono::system_clock::now();
    const stx_auxlib::WallClock::time_point wall = stx_auxlib::WallClock::now();
    const std::chrono::nanoseconds wall_gap =
        wall.time_since_epoch() -
        std::chrono::duration_cast<std::chrono::nanoseconds>(host_wall.time_since_epoch());
    require(wall_gap > -std::chrono::seconds(1) && wall_gap < std::chrono::seconds(1),
            "the wall clock disagrees with system_clock");
}

// Unix time: after 2026-01-01T00:00:00Z (1767225600 s) and before
// 2100-01-01T00:00:00Z (4102444800 s).
void wall_clock_is_unix_time() {
    const std::int64_t seconds = std::chrono::duration_cast<std::chrono::seconds>(
                                     stx_auxlib::WallClock::now().time_since_epoch())
                                     .count();
    require(seconds > 1767225600 && seconds < 4102444800, "the wall clock is not Unix time");
}

void clock_traits() {
    static_assert(stx_auxlib::MonotonicClock::is_steady, "MonotonicClock must be steady");
    static_assert(!stx_auxlib::WallClock::is_steady, "WallClock must not claim to be steady");
    static_assert(sizeof(stx_auxlib::MonotonicClock::rep) == 8, "clocks count 64-bit ticks");
}

} // namespace

int main() {
    clock_traits();
    monotonic_never_decreases();
    agrees_with_host();
    wall_clock_is_unix_time();
    std::puts("stx_auxlib clocks tests passed");
    return 0;
}
