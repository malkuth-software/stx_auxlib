// tan (declared, with its contracts, in stx_auxlib/math.hpp).
//
// tan x = (S + C t) / (C - S t) with S, C the table's sin and cos at
// k*pi/128 and t = tan r (the tangent addition formula; numerator and
// denominator are sin x and cos x over cos r), each formed as the sin/cos
// core forms its rotation (sincos_core.hpp: the owner's 26-bit split, no
// exact product), and the quotient by a split division. Near the poles the
// denominator is small, but it keeps its relative accuracy there: the table
// holds cos(pi/2) = 0 exactly, so it comes from the exact term -s1*r1.

#include "stx_auxlib/math.hpp"

#include "sincos_core.hpp"

#include <limits>

#include "variant_functions.hpp"

STX_AUXLIB_MATH_TARGET_PUSH

namespace stx_auxlib::math::STX_AUXLIB_MATH_VARIANT {

namespace detail {
namespace {

// The numerator S + C t and the denominator C - S t, each normalized as
// head + low part, from the table index and r + r_lo:
//   n = S + c1*r1 + [(c1*r2 + c2*r) + C*(t - r)] + S_lo
//   d = C - s1*r1 - [(s1*r2 + s2*r) + S*(t - r)] + C_lo
// with t - r = r^3 (tan_3 + ...) + r_lo (1 + r^2).
void tan_rotate(const std::uint64_t index, const double r, const double r_lo, Wide& numerator,
                Wide& denominator) noexcept {
    const double r_squared = r * r;
    const double polynomial =
        kernel_constants.tan_3 + r_squared * (kernel_constants.tan_5 + r_squared * (kernel_constants.tan_7 + r_squared * (kernel_constants.tan_9 + r_squared * kernel_constants.tan_11)));
    const double t_minus_r = r * (r_squared * polynomial) + r_lo * (1.0 + r_squared);
    const SinCosEntry& entry = sincos_table_256[index];
    const SinCosCut& cut = sincos_cuts.values[index];
    const double r1 = cut_26(r);
    const double r2 = r - r1;  // exact

    double n_head = 0.0;
    double n_head_error = 0.0;
    fast_two_sum(entry.s, cut.c1 * r1, n_head, n_head_error);  // c1 r1 exact
    const double n_low = (n_head_error + ((cut.c1 * r2 + cut.c2 * r) + entry.c * t_minus_r)) + entry.s_lo;
    fast_two_sum(n_head, n_low, numerator.hi, numerator.lo);

    double d_head = 0.0;
    double d_head_error = 0.0;
    fast_two_sum(entry.c, -(cut.s1 * r1), d_head, d_head_error);  // s1 r1 exact
    const double d_low = (d_head_error - ((cut.s1 * r2 + cut.s2 * r) + entry.s * t_minus_r)) + entry.c_lo;
    fast_two_sum(d_head, d_low, denominator.hi, denominator.lo);
}

// (n + n_lo) / (d + d_lo) as q1 + correction, for normalized pairs. q1 is
// the double quotient cut to 26 bits, so q1 * d1 (d1 = cut_26(d)) is exact
// and within 2^-23.9 of n, where n - q1 d1 is exact (Sterbenz); the
// residual's four other roundings are of terms below 2^-23 |n|, and the
// correction residual * (1/d) is below 2^-22.9 |q|: 2^-73.3 relative in all
// (tools/sincos_certificate.py), against 2^-102 for the exact-product
// division it replaces.
Wide divide_split(const Wide& numerator, const Wide& denominator) noexcept {
    const double inverse = 1.0 / denominator.hi;
    const double q1 = cut_26(numerator.hi * inverse);
    const double d1 = cut_26(denominator.hi);
    const double d2 = denominator.hi - d1;  // exact
    const double residual =
        (((numerator.hi - q1 * d1) - q1 * d2) + numerator.lo) - q1 * denominator.lo;
    const Wide result{q1, residual * inverse};
    return result;
}

// For 2^-27 <= |x| <= 2^32 (fast or medium range).
Wide tan_wide(const double x) noexcept {
    std::uint64_t index = 0;
    double r = 0.0;
    double r_lo = 0.0;
    if (sincos_fast_path(x)) {
        (void)sincos_reduce_fast(x, index, r, r_lo);
    } else {
        sincos_reduce_medium(x, index, r, r_lo);
    }
    Wide numerator{};
    Wide denominator{};
    tan_rotate(index, r, r_lo, numerator, denominator);
    const Wide result = divide_split(numerator, denominator);
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
