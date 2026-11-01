#pragma once

// Private calendar conversions shared by calendar.cpp and the host local
// time backends (calendar_posix.cpp, calendar_windows.cpp).

#include "stx_auxlib/calendar.hpp"
#include "stx_auxlib/clocks.hpp"

#include <cstdint>
#include <optional>

namespace stx_auxlib::calendar_detail {

struct CivilDay final {
    std::int64_t year{1970};
    std::uint32_t month{1};
    std::uint32_t day{1};
};

// Days from 1970-01-01 to a proleptic Gregorian date, and back.
[[nodiscard]] std::int64_t days_from_civil(std::int64_t year, std::uint32_t month,
                                           std::uint32_t day) noexcept;
[[nodiscard]] CivilDay civil_from_days(std::int64_t days) noexcept;

// Whole seconds from 1970-01-01T00:00:00 of a valid civil time, ignoring
// its nanosecond, and back. civil_from_seconds requires seconds whose year
// fits int32 (|seconds| below about 6.7e16); WallClock's whole range, about
// +-9.2e9 seconds, does.
[[nodiscard]] std::int64_t seconds_from_civil(const CivilTime& time) noexcept;
[[nodiscard]] CivilTime civil_from_seconds(std::int64_t seconds, std::uint32_t nanosecond) noexcept;

// A wall-clock instant as floor seconds and the nanoseconds past them.
struct WallSplit final {
    std::int64_t seconds{0};
    std::uint32_t nanosecond{0};
};
[[nodiscard]] WallSplit split_wall(WallClock::time_point instant) noexcept;
// The instant, or empty when it does not fit WallClock.
[[nodiscard]] std::optional<WallClock::time_point> wall_from_seconds(std::int64_t seconds,
                                                                     std::uint32_t nanosecond) noexcept;

} // namespace stx_auxlib::calendar_detail
