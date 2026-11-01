#pragma once

// The program's two clocks. Both meet the standard Clock requirements, so
// std::chrono durations, time points and arithmetic work with them; only
// reading the current time is sealed here.
//
// MonotonicClock never goes backwards and is unaffected by changes to the
// wall clock; its epoch is unspecified (typically boot), so its time points
// are meaningful only relative to one another within one run. It replaces
// std::chrono::steady_clock.
//
// WallClock is Coordinated Universal Time as nanoseconds since
// 1970-01-01T00:00:00Z, ignoring leap seconds (Unix time). It can jump when
// the system time is set, so measure intervals with MonotonicClock. It
// replaces std::chrono::system_clock.
//
// Both count signed 64-bit nanoseconds, which spans about 292 years either
// side of the epoch. The host's clocks may tick more coarsely; now() reports
// whatever the host provides, converted without rounding.

#include <chrono>
#include <cstdint>
#include <ratio>

namespace stx_auxlib {

struct MonotonicClock final {
    using rep = std::int64_t;
    using period = std::nano;
    using duration = std::chrono::duration<rep, period>;
    using time_point = std::chrono::time_point<MonotonicClock, duration>;
    static constexpr bool is_steady = true;

    [[nodiscard]] static time_point now() noexcept;
};

struct WallClock final {
    using rep = std::int64_t;
    using period = std::nano;
    using duration = std::chrono::duration<rep, period>;
    using time_point = std::chrono::time_point<WallClock, duration>;
    static constexpr bool is_steady = false;

    [[nodiscard]] static time_point now() noexcept;
};

} // namespace stx_auxlib
