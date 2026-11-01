// The local time zone on POSIX hosts (include/stx_auxlib/calendar.hpp):
// tzset and localtime_r, under the environment lock because both read TZ.
// localtime_r need not reread TZ, so tzset runs on the first call and after
// every change made through set_variable or remove_variable (a TZ changed
// with the host's setenv directly is not seen). The
// offset is the local reading minus the UTC one, so no non-standard field
// (tm_gmtoff) is needed.

#include "stx_auxlib/calendar.hpp"

#include "../calendar_detail.hpp"
#include "environment_lock.hpp"

#include "stx_auxlib/threads.hpp"

#include <cstdint>
#include <ctime>
#include <limits>

#include <time.h>

namespace stx_auxlib {

namespace {

// The environment generation tzset last saw; guarded by the environment
// lock.
bool zone_loaded = false;
std::uint64_t zone_generation = 0;

} // namespace

CapabilityValue<LocalTime> local_time(const WallClock::time_point instant) {
    const calendar_detail::WallSplit split = calendar_detail::split_wall(instant);
    static_assert(sizeof(std::time_t) >= 8, "time_t must hold 64-bit seconds");
    const std::time_t seconds = static_cast<std::time_t>(split.seconds);
    std::tm local{};
    {
        const environment_detail::SpinGuard lock(environment_detail::environment_lock());
        const std::uint64_t generation = environment_detail::environment_generation();
        if (!zone_loaded || generation != zone_generation) {
            ::tzset();
            zone_loaded = true;
            zone_generation = generation;
        }
        if (::localtime_r(&seconds, &local) == nullptr)
            return CapabilityValue<LocalTime>::failure(CapabilityStatus::io_error);
    }
    LocalTime result{};
    result.civil.year = static_cast<std::int32_t>(local.tm_year + 1900);
    result.civil.month = static_cast<std::uint8_t>(local.tm_mon + 1);
    result.civil.day = static_cast<std::uint8_t>(local.tm_mday);
    result.civil.hour = static_cast<std::uint8_t>(local.tm_hour);
    result.civil.minute = static_cast<std::uint8_t>(local.tm_min);
    // A leap-second-aware zone may report second 60; Unix time has none.
    result.civil.second = static_cast<std::uint8_t>(local.tm_sec > 59 ? 59 : local.tm_sec);
    result.civil.nanosecond = split.nanosecond;
    const std::int64_t local_seconds = calendar_detail::seconds_from_civil(result.civil);
    result.offset_seconds = static_cast<std::int32_t>(local_seconds - split.seconds);
    return CapabilityValue<LocalTime>::success(result);
}

} // namespace stx_auxlib
