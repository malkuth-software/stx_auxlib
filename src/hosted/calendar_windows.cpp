// The local time zone on Windows (include/stx_auxlib/calendar.hpp):
// FileTimeToSystemTime for UTC, then SystemTimeToTzSpecificLocalTimeEx with
// the current dynamic time zone. FILETIME counts 100 ns ticks from
// 1601-01-01, so earlier instants are refused (invalid_request). The
// offset is the local reading minus the UTC one.

#include "stx_auxlib/calendar.hpp"

#include "../calendar_detail.hpp"

#include <cstdint>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace stx_auxlib {

namespace {

// 100 ns ticks from 1601-01-01 to 1970-01-01.
constexpr std::int64_t unix_epoch_ticks = 116444736000000000LL;

CivilTime civil_of(const SYSTEMTIME& time, const std::uint32_t nanosecond) noexcept {
    CivilTime result{};
    result.year = time.wYear;
    result.month = static_cast<std::uint8_t>(time.wMonth);
    result.day = static_cast<std::uint8_t>(time.wDay);
    result.hour = static_cast<std::uint8_t>(time.wHour);
    result.minute = static_cast<std::uint8_t>(time.wMinute);
    result.second = static_cast<std::uint8_t>(time.wSecond);
    result.nanosecond = nanosecond;
    return result;
}

} // namespace

CapabilityValue<LocalTime> local_time(const WallClock::time_point instant) {
    const calendar_detail::WallSplit split = calendar_detail::split_wall(instant);
    // Whole seconds only: the nanoseconds are carried over unchanged.
    if (split.seconds < -unix_epoch_ticks / 10000000)
        return CapabilityValue<LocalTime>::failure(CapabilityStatus::invalid_request);
    const std::int64_t ticks = split.seconds * 10000000 + unix_epoch_ticks;
    FILETIME file_time{};
    file_time.dwLowDateTime = static_cast<DWORD>(static_cast<std::uint64_t>(ticks) & 0xffffffffULL);
    file_time.dwHighDateTime = static_cast<DWORD>(static_cast<std::uint64_t>(ticks) >> 32);
    SYSTEMTIME utc{};
    SYSTEMTIME local{};
    if (!::FileTimeToSystemTime(&file_time, &utc) ||
        !::SystemTimeToTzSpecificLocalTimeEx(nullptr, &utc, &local))
        return CapabilityValue<LocalTime>::failure(CapabilityStatus::io_error);
    LocalTime result{};
    result.civil = civil_of(local, split.nanosecond);
    const std::int64_t local_seconds = calendar_detail::seconds_from_civil(result.civil);
    result.offset_seconds = static_cast<std::int32_t>(local_seconds - split.seconds);
    return CapabilityValue<LocalTime>::success(result);
}

} // namespace stx_auxlib
