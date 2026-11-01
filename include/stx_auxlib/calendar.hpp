#pragma once

// Calendar time: civil dates and times in the proleptic Gregorian calendar,
// the host's local time zone, and ISO 8601 text, replacing gmtime,
// localtime, mktime, timegm, strftime and std::put_time.
//
// The calendar arithmetic is owned and host-free (Hinnant's civil-day
// algorithms), so it gives the same answer on every host and on UEFI. Only
// the local time zone comes from the host: local_time reports unsupported
// where there is none, and callers then show UTC.
//
// Range: WallClock counts signed 64-bit nanoseconds from 1970, so civil
// times from 1677-09-21T00:12:43.145224192Z to
// 2262-04-11T23:47:16.854775807Z convert; others are refused. Leap seconds
// do not exist here (Unix time): second 60 is refused.

#include "stx_auxlib/capabilities.hpp"
#include "stx_auxlib/clocks.hpp"
#include "stx_auxlib/text.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace stx_auxlib {

struct CivilTime final {
    std::int32_t year{1970};
    // 1 to 12.
    std::uint8_t month{1};
    // 1 to the month's length.
    std::uint8_t day{1};
    std::uint8_t hour{0};
    std::uint8_t minute{0};
    std::uint8_t second{0};
    std::uint32_t nanosecond{0};
};

// ISO 8601 numbering: Monday is 1, Sunday 7.
enum class Weekday : std::uint8_t {
    monday = 1,
    tuesday = 2,
    wednesday = 3,
    thursday = 4,
    friday = 5,
    saturday = 6,
    sunday = 7,
};

[[nodiscard]] bool is_leap_year(std::int32_t year) noexcept;
// 0 for a month outside 1 to 12.
[[nodiscard]] std::uint8_t days_in_month(std::int32_t year, std::uint8_t month) noexcept;
// True when every field is in range (the date exists, hour < 24,
// minute < 60, second < 60, nanosecond < 10^9); says nothing about whether
// it fits WallClock.
[[nodiscard]] bool is_valid_civil_time(const CivilTime& time) noexcept;
[[nodiscard]] Weekday weekday_of(const CivilTime& time) noexcept;
// 1 for January 1.
[[nodiscard]] std::uint16_t day_of_year(const CivilTime& time) noexcept;

// The UTC civil time of a wall-clock instant.
[[nodiscard]] CivilTime civil_from_wall(WallClock::time_point instant) noexcept;
// The instant of a UTC civil time; empty when the time is invalid or
// outside WallClock's range.
[[nodiscard]] std::optional<WallClock::time_point> wall_from_civil(const CivilTime& time) noexcept;

struct LocalTime final {
    CivilTime civil{};
    // Seconds east of UTC (local = UTC + offset), daylight saving included.
    std::int32_t offset_seconds{0};
};

// The instant in the host's local time zone (its TZ setting or system
// zone). unsupported where the host has no time zone.
[[nodiscard]] CapabilityValue<LocalTime> local_time(WallClock::time_point instant);

enum class ZoneDesignator : std::uint8_t {
    // No zone ("2026-10-31 21:30:05"), as "%Y-%m-%d %H:%M:%S" prints.
    none = 0,
    // "Z" for offset zero, "+02:00" otherwise.
    designated = 1,
};

struct Iso8601Format final {
    // The character between date and time: 'T' (ISO 8601) or ' ' (RFC 3339
    // allows it, and it reads more easily).
    char separator{'T'};
    // Digits of the second's fraction, 0 to 9.
    std::uint8_t fraction_digits{0};
    ZoneDesignator zone{ZoneDesignator::designated};
};

// Appends "YYYY-MM-DD<separator>HH:MM:SS[.fff][zone]". Years outside 0 to
// 9999 are written with a sign and at least four digits ("-0042",
// "+12345"), as ISO 8601's expanded form does.
void append_iso8601(std::string& output, const CivilTime& time, std::int32_t offset_seconds,
                    const Iso8601Format& format = {});

// Parses "YYYY-MM-DDTHH:MM:SS[.fraction](Z|+HH:MM|-HH:MM)", with 'T', 't'
// or ' ' between date and time and up to nine fraction digits, as an
// instant. A zone is required, since text without one names no instant.
[[nodiscard]] text::Parsed<WallClock::time_point> parse_iso8601(std::string_view value) noexcept;

} // namespace stx_auxlib
