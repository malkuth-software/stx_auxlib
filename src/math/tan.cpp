// tan (declared, with its contracts, in stx_auxlib/math.hpp).
//
// tan x = sin x / cos x with both from the sin/cos core as unrounded
// double-doubles (sincos_core.hpp, ~1e-21 relative) and the quotient formed
// as a double-double. Near the poles cos x is small, but the core keeps its
// relative accuracy there: the table holds cos(pi/2) = 0 exactly, so cos x
// comes from the exact rotation term -S*r.

#include "stx_auxlib/math.hpp"

#include "sincos_core.hpp"

#include <limits>

namespace stx_auxlib::math {

namespace detail {
namespace {

// For 2^-27 <= |x| <= 2^32 (fast or medium range).
Wide tan_wide(const double x) noexcept {
    double s_hi = 0.0;
    double s_lo = 0.0;
    double c_hi = 0.0;
    double c_lo = 0.0;
    if (sincos_fast_path(x)) {
        sincos_wide(x, s_hi, s_lo, c_hi, c_lo);
    } else {
        sincos_wide_medium(x, s_hi, s_lo, c_hi, c_lo);
    }
    // The core's pairs are unnormalized; divide needs a normalized divisor.
    double divisor = 0.0;
    double divisor_lo = 0.0;
    two_sum(c_hi, c_lo, divisor, divisor_lo);
    double dividend = 0.0;
    double dividend_lo = 0.0;
    two_sum(s_hi, s_lo, dividend, dividend_lo);
    const Wide result = divide(dividend, dividend_lo, divisor, divisor_lo);
    return result;
}

} // namespace
} // namespace detail

double tan(const double x) noexcept {
    if (!detail::sincos_fast_path(x) && !detail::sincos_medium_path(x)) {
        // Below 2^-27 tan x = x (x^3/3 < ulp/2), sign of zero kept; above
        // 2^32 and for non-finite x refused with NaN, as for sincos.
        if (std::fabs(x) < detail::sincos_tiny_limit) return x;
        if (x != x) return x + x;
        return std::numeric_limits<double>::quiet_NaN();
    }
    const detail::Wide quotient = detail::tan_wide(x);
    const double result = quotient.hi + quotient.lo;
    return result;
}

float tan(const float value) noexcept {
    const double x = static_cast<double>(value);
    if (!detail::sincos_fast_path(x) && !detail::sincos_medium_path(x)) {
        if (std::fabs(x) < detail::sincos_tiny_limit) return value;
        if (x != x) return value + value;
        return std::numeric_limits<float>::quiet_NaN();
    }
    const detail::Wide quotient = detail::tan_wide(x);
    const float result = detail::round_wide_to_float(quotient.hi, quotient.lo);
    return result;
}

} // namespace stx_auxlib::math
