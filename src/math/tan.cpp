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

#include "variant_functions.hpp"

STX_AUXLIB_MATH_TARGET_PUSH

namespace stx_auxlib::math::STX_AUXLIB_MATH_VARIANT {

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

// Absolute error bound of the plain-double sine and cosine below: the table
// values rounded to double (2^-54 each, s_lo and c_lo dropped), the
// remainder's roundings (2^-52 of |r| <= 0.0124, and q times the 2^-118 left
// of pi/128 by its three parts), the sine polynomial's truncation at r^7/5040
// (below 2^-56.7), about ten roundings of terms below 0.0124, and the final
// sum's rounding (2^-53): under 2^-52.2 in all, so 2^-51 holds with margin.
constexpr double tan_float_sincos_error = 0x1p-51;

// Float tan's cheap path for 2^-27 <= |x| <= 2^20: sin and cos in plain
// double as float sincos forms them, and their quotient, certified with
// float_if_certain. With |s - sin x|, |c - cos x| <= E, the quotient's error
// is at most E (1 + |t|) / (|c| - E) plus its own rounding (2^-53 |t|; 2^-52
// is used). Returns false when the rounding is uncertain, including every x
// close enough to a pole that |c| <= 2E. A certified result is the correctly
// rounded float, which the double-double path also returns.
bool tan_float_cheap(const double x, float& result) noexcept {
    std::uint64_t index = 0;
    const double q = sincos_step(x, index);
    const double r = sincos_remainder(x, q);
    const double r2 = r * r;
    const double sin_r = r + r * (r2 * (kernel_constants.sin_3 + r2 * kernel_constants.sin_5));
    const double cos_r_minus_1 =
        (r2 * r2) * (kernel_constants.cos_4 + r2 * kernel_constants.cos_6) - r2 * cos_half;
    const SinCosEntry& entry = sincos_table_256[index];
    const double s = entry.s + (entry.s * cos_r_minus_1 + entry.c * sin_r);
    const double c = entry.c + (entry.c * cos_r_minus_1 - entry.s * sin_r);
    const double c_magnitude = std::fabs(c);
    if (!(c_magnitude > 2.0 * tan_float_sincos_error)) return false;
    const double t = s / c;
    const double t_magnitude = std::fabs(t);
    const double error = tan_float_sincos_error * (1.0 + t_magnitude) / (c_magnitude - tan_float_sincos_error) +
                         t_magnitude * 0x1p-52;
    const bool certain = float_if_certain(t, error, result);
    return certain;
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
    const double x = detail::widen(value);
    if (!detail::sincos_fast_path(x) && !detail::sincos_medium_path(x)) {
        if (std::fabs(x) < detail::sincos_tiny_limit) return value;
        if (x != x) return value + value;
        return std::numeric_limits<float>::quiet_NaN();
    }
    if (detail::sincos_fast_path(x)) {
        float fast = 0.0f;
        if (detail::tan_float_cheap(x, fast)) return fast;
    }
    const detail::Wide quotient = detail::tan_wide(x);
    const float result = detail::round_wide_to_float(quotient.hi, quotient.lo);
    return result;
}

} // namespace stx_auxlib::math::STX_AUXLIB_MATH_VARIANT

STX_AUXLIB_MATH_TARGET_POP
