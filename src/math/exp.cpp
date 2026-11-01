// exp (declared, with its contracts, in stx_auxlib/math.hpp). The core is
// in exp_core.hpp.

#include "stx_auxlib/math.hpp"

#include "exp_core.hpp"

#include <limits>

namespace stx_auxlib::math {

// e^x for a float (contract in stx_auxlib/math.hpp): computed in double and
// rounded once to float, so float overflow and underflow round correctly.
float exp(const float value) noexcept {
    const double x = detail::widen(value);
    if (!detail::exp_fast_path(x)) {
        // Beyond |x| = 708 every float result is infinity or zero.
        if (x != x) return value + value;
        if (x > 0.0) return std::numeric_limits<float>::infinity();
        return 0.0f;
    }
    const double result = detail::exp_cheap(x);
    return static_cast<float>(result);
}

namespace detail {
namespace {

// The inputs exp_fast_path rejects: NaN, infinities, overflow, and results
// that are subnormal or zero.
double exp_outside(const double x) noexcept {
    if (x != x) return x + x;
    if (x > exp_overflow_limit) return std::numeric_limits<double>::infinity();
    if (x < exp_underflow_limit) return 0.0;
    double head = 0.0;
    double tail = 0.0;
    std::int64_t exponent = 0;
    exp_core(x, 0.0, head, tail, exponent);
    const double result = scale_wide(head, tail, exponent);
    return result;
}

} // namespace
} // namespace detail

// e^x for a double (contract in stx_auxlib/math.hpp).
double exp(const double x) noexcept {
    if (!detail::exp_fast_path(x)) return detail::exp_outside(x);
    double head = 0.0;
    double tail = 0.0;
    std::int64_t exponent = 0;
    detail::exp_core(x, 0.0, head, tail, exponent);
    const double y = head + tail;
    const double result = y * detail::power_of_two(exponent);
    return result;
}

} // namespace stx_auxlib::math
