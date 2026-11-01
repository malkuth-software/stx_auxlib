// The host-free calendar (include/stx_auxlib/calendar.hpp): civil days
// after Howard Hinnant's "chrono-Compatible Low-Level Date Algorithms",
// ISO 8601 text, and the conversion helpers the host local-time backends
// share (calendar_detail.hpp).

#include "stx_auxlib/calendar.hpp"

#include "calendar_detail.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace stx_auxlib {

namespace {

constexpr std::int64_t nanoseconds_per_second = 1000000000;
constexpr std::int64_t seconds_per_day = 86400;

// Floor division and its non-negative remainder.
struct Division final {
    std::int64_t quotient{0};
    std::int64_t remainder{0};
};

Division floor_divide(const std::int64_t value, const std::int64_t divisor) noexcept {
    std::int64_t quotient = value / divisor;
    std::int64_t remainder = value % divisor;
    if (remainder < 0) {
        --quotient;
        remainder += divisor;
    }
    return Division{quotient, remainder};
}

// Writes `value` as exactly `digits` decimal digits, zero-padded.
void append_digits(std::string& output, std::uint64_t value, const std::size_t digits) {
    std::array<char, 20> buffer{};
    for (std::size_t index = digits; index > 0; --index) {
        buffer[index - 1] = static_cast<char>('0' + value % 10);
        value /= 10;
    }
    output.append(buffer.data(), digits);
}

std::size_t decimal_length(std::uint64_t value) noexcept {
    std::size_t length = 1;
    while (value >= 10) {
        value /= 10;
        ++length;
    }
    return length;
}

bool is_digit(const char character) noexcept {
    const bool result = character >= '0' && character <= '9';
    return result;
}

// The number in `text` at [first, first + count), all digits; -1 otherwise.
std::int64_t digits_at(const std::string_view text, const std::size_t first, const std::size_t count) noexcept {
    if (first + count > text.size()) return -1;
    std::int64_t value = 0;
    for (std::size_t index = first; index < first + count; ++index) {
        if (!is_digit(text[index])) return -1;
        value = value * 10 + (text[index] - '0');
    }
    return value;
}

} // namespace

std::int64_t calendar_detail::days_from_civil(const std::int64_t year, const std::uint32_t month,
                                              const std::uint32_t day) noexcept {
    const std::int64_t shifted_year = month <= 2 ? year - 1 : year;
    const std::int64_t era = (shifted_year >= 0 ? shifted_year : shifted_year - 399) / 400;
    const std::int64_t year_of_era = shifted_year - era * 400;
    const std::int64_t shifted_month = month > 2 ? month - 3 : month + 9;
    const std::int64_t day_of_year = (153 * shifted_month + 2) / 5 + day - 1;
    const std::int64_t day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    const std::int64_t result = era * 146097 + day_of_era - 719468;
    return result;
}

calendar_detail::CivilDay calendar_detail::civil_from_days(const std::int64_t days) noexcept {
    const std::int64_t shifted = days + 719468;
    const std::int64_t era = (shifted >= 0 ? shifted : shifted - 146096) / 146097;
    const std::int64_t day_of_era = shifted - era * 146097;
    const std::int64_t year_of_era =
        (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
    const std::int64_t day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    const std::int64_t shifted_month = (5 * day_of_year + 2) / 153;
    const std::int64_t day = day_of_year - (153 * shifted_month + 2) / 5 + 1;
    const std::int64_t month = shifted_month < 10 ? shifted_month + 3 : shifted_month - 9;
    CivilDay result{};
    result.year = year_of_era + era * 400 + (month <= 2 ? 1 : 0);
    result.month = static_cast<std::uint32_t>(month);
    result.day = static_cast<std::uint32_t>(day);
    return result;
}

std::int64_t calendar_detail::seconds_from_civil(const CivilTime& time) noexcept {
    const std::int64_t days = days_from_civil(time.year, time.month, time.day);
    const std::int64_t result = days * seconds_per_day + time.hour * 3600 + time.minute * 60 + time.second;
    return result;
}

CivilTime calendar_detail::civil_from_seconds(const std::int64_t seconds, const std::uint32_t nanosecond) noexcept {
    const Division day = floor_divide(seconds, seconds_per_day);
    const CivilDay date = civil_from_days(day.quotient);
    assert(date.year >= std::numeric_limits<std::int32_t>::min() &&
           date.year <= std::numeric_limits<std::int32_t>::max());
    CivilTime result{};
    result.year = static_cast<std::int32_t>(date.year);
    result.month = static_cast<std::uint8_t>(date.month);
    result.day = static_cast<std::uint8_t>(date.day);
    result.hour = static_cast<std::uint8_t>(day.remainder / 3600);
    result.minute = static_cast<std::uint8_t>(day.remainder % 3600 / 60);
    result.second = static_cast<std::uint8_t>(day.remainder % 60);
    result.nanosecond = nanosecond;
    return result;
}

calendar_detail::WallSplit calendar_detail::split_wall(const WallClock::time_point instant) noexcept {
    const Division split = floor_divide(instant.time_since_epoch().count(), nanoseconds_per_second);
    WallSplit result{};
    result.seconds = split.quotient;
    result.nanosecond = static_cast<std::uint32_t>(split.remainder);
    return result;
}

// seconds * 10^9 + nanosecond, refusing results outside int64. The bounds:
// INT64_MAX is 9223372036 s + 854775807 ns, and INT64_MIN is
// -9223372037 s + 145224192 ns.
std::optional<WallClock::time_point> calendar_detail::wall_from_seconds(const std::int64_t seconds,
                                                                        const std::uint32_t nanosecond) noexcept {
    constexpr std::int64_t largest_seconds = 9223372036;
    constexpr std::int64_t smallest_seconds = -9223372037;
    if (seconds > largest_seconds || seconds < smallest_seconds) return std::nullopt;
    if (seconds == largest_seconds && nanosecond > 854775807U) return std::nullopt;
    if (seconds == smallest_seconds && nanosecond < 145224192U) return std::nullopt;
    // For negative seconds, (seconds + 1) * 10^9 - (10^9 - nanosecond) keeps
    // every intermediate in range at the lower bound, where seconds * 10^9
    // alone would overflow.
    std::int64_t count = 0;
    if (seconds >= 0) {
        count = seconds * nanoseconds_per_second + static_cast<std::int64_t>(nanosecond);
    } else {
        count = (seconds + 1) * nanoseconds_per_second -
                (nanoseconds_per_second - static_cast<std::int64_t>(nanosecond));
    }
    const WallClock::time_point result{WallClock::duration(count)};
    return result;
}

bool is_leap_year(const std::int32_t year) noexcept {
    const bool result = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return result;
}

std::uint8_t days_in_month(const std::int32_t year, const std::uint8_t month) noexcept {
    constexpr std::array<std::uint8_t, 12> lengths = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 0;
    if (month == 2 && is_leap_year(year)) return 29;
    return lengths[month - 1];
}

bool is_valid_civil_time(const CivilTime& time) noexcept {
    const bool result = time.day >= 1 && time.day <= days_in_month(time.year, time.month) &&
                        time.hour < 24 && time.minute < 60 && time.second < 60 &&
                        time.nanosecond < 1000000000U;
    return result;
}

Weekday weekday_of(const CivilTime& time) noexcept {
    // 1970-01-01 was a Thursday (ISO 4).
    const std::int64_t days = calendar_detail::days_from_civil(time.year, time.month, time.day);
    const Division week = floor_divide(days + 3, 7);
    const Weekday result = static_cast<Weekday>(week.remainder + 1);
    return result;
}

std::uint16_t day_of_year(const CivilTime& time) noexcept {
    const std::int64_t days = calendar_detail::days_from_civil(time.year, time.month, time.day);
    const std::int64_t first = calendar_detail::days_from_civil(time.year, 1, 1);
    const std::uint16_t result = static_cast<std::uint16_t>(days - first + 1);
    return result;
}

CivilTime civil_from_wall(const WallClock::time_point instant) noexcept {
    const calendar_detail::WallSplit split = calendar_detail::split_wall(instant);
    const CivilTime result = calendar_detail::civil_from_seconds(split.seconds, split.nanosecond);
    return result;
}

std::optional<WallClock::time_point> wall_from_civil(const CivilTime& time) noexcept {
    if (!is_valid_civil_time(time)) return std::nullopt;
    const std::int64_t seconds = calendar_detail::seconds_from_civil(time);
    const std::optional<WallClock::time_point> result =
        calendar_detail::wall_from_seconds(seconds, time.nanosecond);
    return result;
}

void append_iso8601(std::string& output, const CivilTime& time, const std::int32_t offset_seconds,
                    const Iso8601Format& format) {
    if (time.year >= 0 && time.year <= 9999) {
        append_digits(output, static_cast<std::uint64_t>(time.year), 4);
    } else {
        const std::uint64_t magnitude = static_cast<std::uint64_t>(std::llabs(time.year));
        output.push_back(time.year < 0 ? '-' : '+');
        const std::size_t length = decimal_length(magnitude);
        append_digits(output, magnitude, length < 4 ? 4 : length);
    }
    output.push_back('-');
    append_digits(output, time.month, 2);
    output.push_back('-');
    append_digits(output, time.day, 2);
    output.push_back(format.separator);
    append_digits(output, time.hour, 2);
    output.push_back(':');
    append_digits(output, time.minute, 2);
    output.push_back(':');
    append_digits(output, time.second, 2);
    const std::size_t digits = format.fraction_digits > 9 ? 9 : format.fraction_digits;
    if (digits > 0) {
        std::uint64_t scale = 1;
        for (std::size_t index = digits; index < 9; ++index) scale *= 10;
        output.push_back('.');
        append_digits(output, time.nanosecond / scale, digits);
    }
    if (format.zone == ZoneDesignator::none) return;
    if (offset_seconds == 0) {
        output.push_back('Z');
        return;
    }
    // ISO 8601 offsets are whole minutes; any seconds are dropped.
    const std::int64_t magnitude = std::llabs(static_cast<std::int64_t>(offset_seconds));
    output.push_back(offset_seconds < 0 ? '-' : '+');
    append_digits(output, static_cast<std::uint64_t>(magnitude / 3600), 2);
    output.push_back(':');
    append_digits(output, static_cast<std::uint64_t>(magnitude % 3600 / 60), 2);
}

// Fixed positions: YYYY-MM-DDTHH:MM:SS at 0..18, then an optional fraction,
// then the zone.
text::Parsed<WallClock::time_point> parse_iso8601(const std::string_view value) noexcept {
    using Result = text::Parsed<WallClock::time_point>;
    if (value.empty()) return Result::failure(text::ParseFailure::empty);
    const std::int64_t year = digits_at(value, 0, 4);
    const std::int64_t month = digits_at(value, 5, 2);
    const std::int64_t day = digits_at(value, 8, 2);
    const std::int64_t hour = digits_at(value, 11, 2);
    const std::int64_t minute = digits_at(value, 14, 2);
    const std::int64_t second = digits_at(value, 17, 2);
    if (year < 0 || month < 0 || day < 0 || hour < 0 || minute < 0 || second < 0)
        return Result::failure(text::ParseFailure::invalid);
    const char separator = value[10];
    if (value[4] != '-' || value[7] != '-' || value[13] != ':' || value[16] != ':' ||
        (separator != 'T' && separator != 't' && separator != ' '))
        return Result::failure(text::ParseFailure::invalid);
    std::size_t position = 19;
    std::uint32_t nanosecond = 0;
    if (position < value.size() && value[position] == '.') {
        ++position;
        std::size_t digits = 0;
        while (position < value.size() && is_digit(value[position])) {
            if (digits == 9) return Result::failure(text::ParseFailure::invalid);
            nanosecond = nanosecond * 10 + static_cast<std::uint32_t>(value[position] - '0');
            ++digits;
            ++position;
        }
        if (digits == 0) return Result::failure(text::ParseFailure::invalid);
        for (std::size_t index = digits; index < 9; ++index) nanosecond *= 10;
    }
    if (position >= value.size()) return Result::failure(text::ParseFailure::invalid);
    std::int64_t offset = 0;
    const char zone = value[position];
    if (zone == 'Z' || zone == 'z') {
        ++position;
    } else if (zone == '+' || zone == '-') {
        const std::int64_t offset_hours = digits_at(value, position + 1, 2);
        const std::int64_t offset_minutes = digits_at(value, position + 4, 2);
        if (offset_hours < 0 || offset_minutes < 0 || position + 3 >= value.size() ||
            value[position + 3] != ':' || offset_hours > 23 || offset_minutes > 59)
            return Result::failure(text::ParseFailure::invalid);
        offset = offset_hours * 3600 + offset_minutes * 60;
        if (zone == '-') offset = -offset;
        position += 6;
    } else {
        return Result::failure(text::ParseFailure::invalid);
    }
    if (position != value.size()) return Result::failure(text::ParseFailure::trailing_text);

    CivilTime time{};
    time.year = static_cast<std::int32_t>(year);
    time.month = static_cast<std::uint8_t>(month);
    time.day = static_cast<std::uint8_t>(day);
    time.hour = static_cast<std::uint8_t>(hour);
    time.minute = static_cast<std::uint8_t>(minute);
    time.second = static_cast<std::uint8_t>(second);
    time.nanosecond = nanosecond;
    if (!is_valid_civil_time(time)) return Result::failure(text::ParseFailure::invalid);
    // The text's local reading minus its offset is the instant.
    const std::int64_t seconds = calendar_detail::seconds_from_civil(time) - offset;
    const std::optional<WallClock::time_point> instant = calendar_detail::wall_from_seconds(seconds, nanosecond);
    if (!instant) return Result::failure(text::ParseFailure::out_of_range);
    return Result::success(*instant);
}

} // namespace stx_auxlib
