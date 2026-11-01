// Tests of stx_auxlib's calendar (include/stx_auxlib/calendar.hpp). The
// civil-day algorithms are cross-checked against std::chrono's
// year_month_day, which the standard fixes exactly, over every day
// WallClock can hold.

#include "../src/checked.hpp"

#include "stx_auxlib/calendar.hpp"
#include "stx_auxlib/environment.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace {

void require(const bool condition, const char* const message) {
    if (condition) return;
    std::fprintf(stderr, "stx_auxlib calendar: %s\n", message);
    std::abort();
}

stx_auxlib::CivilTime civil(const std::int32_t year, const unsigned month, const unsigned day,
                            const unsigned hour, const unsigned minute, const unsigned second,
                            const std::uint32_t nanosecond) {
    stx_auxlib::CivilTime result{};
    result.year = year;
    result.month = static_cast<std::uint8_t>(month);
    result.day = static_cast<std::uint8_t>(day);
    result.hour = static_cast<std::uint8_t>(hour);
    result.minute = static_cast<std::uint8_t>(minute);
    result.second = static_cast<std::uint8_t>(second);
    result.nanosecond = nanosecond;
    return result;
}

bool same(const stx_auxlib::CivilTime& left, const stx_auxlib::CivilTime& right) {
    const bool result = left.year == right.year && left.month == right.month && left.day == right.day &&
                        left.hour == right.hour && left.minute == right.minute &&
                        left.second == right.second && left.nanosecond == right.nanosecond;
    return result;
}

// A 64-bit linear congruential step (Knuth's MMIX constants), modulo 2^64.
STX_AUXLIB_MODULAR std::uint64_t next_state(const std::uint64_t state) {
    const std::uint64_t result = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return result;
}

stx_auxlib::WallClock::time_point at(const std::int64_t nanoseconds) {
    const stx_auxlib::WallClock::time_point result{stx_auxlib::WallClock::duration(nanoseconds)};
    return result;
}

void against_standard_days() {
    // WallClock spans about +-106,751 days around 1970.
    for (std::int64_t day = -106750; day <= 106750; ++day) {
        const std::chrono::sys_days standard_day{std::chrono::days(day)};
        const std::chrono::year_month_day expected(standard_day);
        const stx_auxlib::CivilTime noon = stx_auxlib::civil_from_wall(at((day * 86400 + 43200) * 1000000000LL));
        require(noon.year == static_cast<int>(expected.year()) &&
                    noon.month == static_cast<unsigned>(expected.month()) &&
                    noon.day == static_cast<unsigned>(expected.day()) && noon.hour == 12,
                "a civil date differs from std::chrono");
        const std::chrono::weekday standard_weekday(standard_day);
        require(static_cast<unsigned>(stx_auxlib::weekday_of(noon)) == standard_weekday.iso_encoding(),
                "a weekday differs from std::chrono");
        const std::optional<stx_auxlib::WallClock::time_point> back = stx_auxlib::wall_from_civil(noon);
        require(back && (*back).time_since_epoch().count() == (day * 86400 + 43200) * 1000000000LL,
                "a civil date did not round-trip");
    }
}

void calendar_rules() {
    require(stx_auxlib::is_leap_year(2000) && stx_auxlib::is_leap_year(2024) &&
                !stx_auxlib::is_leap_year(1900) && !stx_auxlib::is_leap_year(2026),
            "leap years are wrong");
    require(stx_auxlib::days_in_month(2024, 2) == 29 && stx_auxlib::days_in_month(2026, 2) == 28 &&
                stx_auxlib::days_in_month(2026, 4) == 30 && stx_auxlib::days_in_month(2026, 13) == 0,
            "month lengths are wrong");
    require(stx_auxlib::weekday_of(civil(2026, 10, 31, 0, 0, 0, 0)) == stx_auxlib::Weekday::saturday,
            "2026-10-31 is not a Saturday");
    require(stx_auxlib::weekday_of(civil(1970, 1, 1, 0, 0, 0, 0)) == stx_auxlib::Weekday::thursday,
            "1970-01-01 is not a Thursday");
    require(stx_auxlib::day_of_year(civil(2024, 12, 31, 0, 0, 0, 0)) == 366, "day_of_year is wrong");
    require(stx_auxlib::day_of_year(civil(2026, 1, 1, 0, 0, 0, 0)) == 1, "January 1 is not day 1");
    require(!stx_auxlib::is_valid_civil_time(civil(2026, 2, 29, 0, 0, 0, 0)), "2026-02-29 is valid");
    require(!stx_auxlib::is_valid_civil_time(civil(2026, 1, 1, 24, 0, 0, 0)), "hour 24 is valid");
    require(!stx_auxlib::is_valid_civil_time(civil(2026, 1, 1, 0, 0, 60, 0)), "second 60 is valid");
    require(!stx_auxlib::is_valid_civil_time(civil(2026, 1, 1, 0, 0, 0, 1000000000)),
            "a full second of nanoseconds is valid");
}

void instants() {
    require(same(stx_auxlib::civil_from_wall(at(-500000000)), civil(1969, 12, 31, 23, 59, 59, 500000000)),
            "half a second before the epoch is wrong");
    const stx_auxlib::CivilTime latest = civil(2262, 4, 11, 23, 47, 16, 854775807);
    const stx_auxlib::CivilTime earliest = civil(1677, 9, 21, 0, 12, 43, 145224192);
    require(same(stx_auxlib::civil_from_wall(at(std::numeric_limits<std::int64_t>::max())), latest),
            "the latest instant is wrong");
    require(same(stx_auxlib::civil_from_wall(at(std::numeric_limits<std::int64_t>::min())), earliest),
            "the earliest instant is wrong");
    require(stx_auxlib::wall_from_civil(latest).has_value() && stx_auxlib::wall_from_civil(earliest).has_value(),
            "the range limits were refused");
    stx_auxlib::CivilTime past_latest = latest;
    past_latest.nanosecond += 1;
    stx_auxlib::CivilTime before_earliest = earliest;
    before_earliest.nanosecond -= 1;
    require(!stx_auxlib::wall_from_civil(past_latest) && !stx_auxlib::wall_from_civil(before_earliest),
            "an instant outside the range was accepted");
    require(!stx_auxlib::wall_from_civil(civil(2026, 2, 29, 0, 0, 0, 0)), "an invalid date converted");

    std::uint64_t state = 1;
    for (int sample = 0; sample < 100000; ++sample) {
        state = next_state(state);
        const std::int64_t nanoseconds = static_cast<std::int64_t>(state);
        const std::optional<stx_auxlib::WallClock::time_point> back =
            stx_auxlib::wall_from_civil(stx_auxlib::civil_from_wall(at(nanoseconds)));
        require(back && (*back).time_since_epoch().count() == nanoseconds, "an instant did not round-trip");
    }
}

std::string iso(const stx_auxlib::CivilTime& time, const std::int32_t offset,
                const stx_auxlib::Iso8601Format& format) {
    std::string result{};
    stx_auxlib::append_iso8601(result, time, offset, format);
    return result;
}

void iso8601_text() {
    const stx_auxlib::CivilTime time = civil(2026, 10, 31, 21, 30, 5, 123456789);
    require(iso(time, 0, {}) == "2026-10-31T21:30:05Z", "the default form is wrong");
    require(iso(time, 0, {.separator = ' ', .zone = stx_auxlib::ZoneDesignator::none}) ==
                "2026-10-31 21:30:05",
            "the put_time form is wrong");
    require(iso(time, 7200, {.fraction_digits = 3}) == "2026-10-31T21:30:05.123+02:00",
            "a fraction with an offset is wrong");
    require(iso(time, -19800, {.fraction_digits = 9}) == "2026-10-31T21:30:05.123456789-05:30",
            "nine digits with a negative offset is wrong");
    require(iso(civil(-42, 1, 2, 3, 4, 5, 0), 0, {}) == "-0042-01-02T03:04:05Z", "a negative year is wrong");
    require(iso(civil(12345, 6, 7, 8, 9, 10, 0), 0, {}) == "+12345-06-07T08:09:10Z", "a long year is wrong");
}

std::int64_t parsed_nanoseconds(const std::string_view text) {
    const stx_auxlib::text::Parsed<stx_auxlib::WallClock::time_point> parsed = stx_auxlib::parse_iso8601(text);
    require(parsed.succeeded(), "a valid ISO 8601 text was refused");
    const std::int64_t result = (*parsed.value()).time_since_epoch().count();
    return result;
}

void expect_refused(const std::string_view text, const stx_auxlib::text::ParseFailure failure) {
    const stx_auxlib::text::Parsed<stx_auxlib::WallClock::time_point> parsed = stx_auxlib::parse_iso8601(text);
    require(!parsed.succeeded() && *parsed.failure() == failure, "an invalid ISO 8601 text was accepted");
}

void iso8601_parsing() {
    const std::int64_t base = parsed_nanoseconds("2026-10-31T21:30:05Z");
    require(base == 1793482205LL * 1000000000LL, "the parsed instant is wrong");
    require(parsed_nanoseconds("2026-10-31t21:30:05z") == base, "lower-case t and z were refused");
    require(parsed_nanoseconds("2026-10-31 23:30:05+02:00") == base, "an offset was misapplied");
    require(parsed_nanoseconds("2026-10-31T16:00:05-05:30") == base, "a negative offset was misapplied");
    require(parsed_nanoseconds("2026-10-31T21:30:05.5Z") == base + 500000000, "a fraction was misread");
    require(parsed_nanoseconds("2026-10-31T21:30:05.123456789Z") == base + 123456789,
            "nine fraction digits were misread");
    require(parsed_nanoseconds("1969-12-31T23:59:59.5Z") == -500000000, "a pre-epoch instant is wrong");

    expect_refused("", stx_auxlib::text::ParseFailure::empty);
    expect_refused("2026-10-31T21:30:05", stx_auxlib::text::ParseFailure::invalid);
    expect_refused("2026-13-01T00:00:00Z", stx_auxlib::text::ParseFailure::invalid);
    expect_refused("2026-02-30T00:00:00Z", stx_auxlib::text::ParseFailure::invalid);
    expect_refused("2026-10-31T24:00:00Z", stx_auxlib::text::ParseFailure::invalid);
    expect_refused("2026-10-31T21:30:05.1234567890Z", stx_auxlib::text::ParseFailure::invalid);
    expect_refused("2026-10-31T21:30:05.Z", stx_auxlib::text::ParseFailure::invalid);
    expect_refused("2026-10-31T21:30:05+2:00", stx_auxlib::text::ParseFailure::invalid);
    expect_refused("2026-10-31X21:30:05Z", stx_auxlib::text::ParseFailure::invalid);
    expect_refused("2026-10-31T21:30:05Z extra", stx_auxlib::text::ParseFailure::trailing_text);
    expect_refused("2263-01-01T00:00:00Z", stx_auxlib::text::ParseFailure::out_of_range);

    // Formatting then parsing gives the instant back.
    std::uint64_t state = 7;
    for (int sample = 0; sample < 10000; ++sample) {
        state = next_state(state);
        const std::int64_t nanoseconds = static_cast<std::int64_t>(state >> 2) - (1LL << 61);
        const std::string text = iso(stx_auxlib::civil_from_wall(at(nanoseconds)), 0, {.fraction_digits = 9});
        require(parsed_nanoseconds(text) == nanoseconds, "formatted text did not parse back");
    }
}

#if !defined(_WIN32)
// POSIX TZ rules fix the zone without depending on the host's zone files.
void local_time_rules() {
    std::string saved{};
    const bool had_zone = stx_auxlib::read_variable("TZ", 256, saved) == stx_auxlib::EnvironmentStatus::ok;
    const stx_auxlib::WallClock::time_point winter = at(parsed_nanoseconds("2026-01-15T12:00:00.25Z"));
    const stx_auxlib::WallClock::time_point summer = at(parsed_nanoseconds("2026-07-15T12:00:00Z"));

    require(stx_auxlib::set_variable("TZ", "UTC0") == stx_auxlib::EnvironmentStatus::ok, "TZ was not set");
    const stx_auxlib::CapabilityValue<stx_auxlib::LocalTime> utc = stx_auxlib::local_time(winter);
    require(utc.succeeded() && (*utc.value()).offset_seconds == 0 &&
                same((*utc.value()).civil, stx_auxlib::civil_from_wall(winter)),
            "UTC local time is wrong");

    require(stx_auxlib::set_variable("TZ", "EST5EDT,M3.2.0,M11.1.0") == stx_auxlib::EnvironmentStatus::ok,
            "TZ was not set");
    const stx_auxlib::CapabilityValue<stx_auxlib::LocalTime> eastern_winter = stx_auxlib::local_time(winter);
    require(eastern_winter.succeeded() && (*eastern_winter.value()).offset_seconds == -18000 &&
                (*eastern_winter.value()).civil.hour == 7 &&
                (*eastern_winter.value()).civil.nanosecond == 250000000,
            "standard time is wrong");
    const stx_auxlib::CapabilityValue<stx_auxlib::LocalTime> eastern_summer = stx_auxlib::local_time(summer);
    require(eastern_summer.succeeded() && (*eastern_summer.value()).offset_seconds == -14400 &&
                (*eastern_summer.value()).civil.hour == 8,
            "daylight saving time is wrong");

    require(stx_auxlib::set_variable("TZ", "<+0530>-5:30") == stx_auxlib::EnvironmentStatus::ok,
            "TZ was not set");
    const stx_auxlib::CapabilityValue<stx_auxlib::LocalTime> india = stx_auxlib::local_time(winter);
    require(india.succeeded() && (*india.value()).offset_seconds == 19800 &&
                (*india.value()).civil.hour == 17 && (*india.value()).civil.minute == 30,
            "a half-hour offset is wrong");
    std::string text{};
    stx_auxlib::append_iso8601(text, (*india.value()).civil, (*india.value()).offset_seconds);
    require(text == "2026-01-15T17:30:00+05:30", "a local time's text is wrong");
    require(parsed_nanoseconds(text) == winter.time_since_epoch().count() - 250000000,
            "a local time's text names another instant");

    if (had_zone) {
        (void)stx_auxlib::set_variable("TZ", saved);
    } else {
        (void)stx_auxlib::remove_variable("TZ");
    }
}
#endif

} // namespace

int main() {
    against_standard_days();
    calendar_rules();
    instants();
    iso8601_text();
    iso8601_parsing();
#if !defined(_WIN32)
    local_time_rules();
#endif
    std::puts("stx_auxlib calendar tests passed");
    return 0;
}
