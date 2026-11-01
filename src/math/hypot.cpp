// hypot (declared, with its contracts, in stx_auxlib/math.hpp).
//
// With a = max(|x|, |y|) and b = min(|x|, |y|), both scaled by 2^-e so that
// a lies in [1, 2): a^2 + b^2 is formed exactly as s + s_lo (two_product,
// two_sum), r = sqrt(s) is the correctly rounded double, and one Newton
// step r_lo = (s - r^2 + s_lo) / (2r) brings r + r_lo to ~1e-32. The result
// (r + r_lo) * 2^e is rounded once (scale_wide), subnormals included.

#include "stx_auxlib/math.hpp"

#include "kernel_support.hpp"

#include <limits>

namespace stx_auxlib::math {

namespace detail {
namespace {

// Below a * 2^-27, b changes a^2 + b^2 by under a * 2^-55, less than half an
// ulp of a: hypot rounds to a.
constexpr double negligible_ratio = 0x1p-27;

// The binary exponent of a positive finite double, subnormals included.
std::int64_t binary_exponent(const double a) noexcept {
    const std::uint64_t bits = std::bit_cast<std::uint64_t>(a);
    const std::int64_t field = static_cast<std::int64_t>(bits >> 52);
    if (field != 0) return field - exponent_bias;
    // Subnormal: scale into the normal range exactly and count the scaling.
    const std::uint64_t scaled_bits = std::bit_cast<std::uint64_t>(a * 0x1p64);
    const std::int64_t scaled_field = static_cast<std::int64_t>(scaled_bits >> 52);
    const std::int64_t result = scaled_field - exponent_bias - 64;
    return result;
}

// a * 2^-exponent for exponent in [-1074, 1023], exact while the result is
// normal (two steps, so 2^-exponent itself never overflows).
double scale_down(const double a, const std::int64_t exponent) noexcept {
    const std::int64_t first = exponent / 2;
    const double result = (a * power_of_two(-first)) * power_of_two(-(exponent - first));
    return result;
}

// hypot of finite a >= b >= 0 with a > 0, as (r + r_lo) * 2^exponent with
// r in [1, 2.83) and r + r_lo within ~1e-32 of the exact value.
void hypot_wide(const double a, const double b, double& r, double& r_lo,
                std::int64_t& exponent) noexcept {
    exponent = binary_exponent(a);
    const double a_scaled = scale_down(a, exponent);  // in [1, 2): exact
    const double b_scaled = scale_down(b, exponent);  // >= 2^-27: exact
    double a_squared = 0.0;
    double a_squared_error = 0.0;
    two_product(a_scaled, a_scaled, a_squared, a_squared_error);
    double b_squared = 0.0;
    double b_squared_error = 0.0;
    two_product(b_scaled, b_scaled, b_squared, b_squared_error);
    double s = 0.0;
    double s_error = 0.0;
    two_sum(a_squared, b_squared, s, s_error);
    const double s_lo = s_error + (a_squared_error + b_squared_error);
    r = std::sqrt(s);  // IEEE sqrt: correctly rounded everywhere
    double r_squared = 0.0;
    double r_squared_error = 0.0;
    two_product(r, r, r_squared, r_squared_error);
    // s - r^2 is exact (r^2 is within an ulp of s); the residual over 2r is
    // the Newton correction.
    const double residual = ((s - r_squared) - r_squared_error) + s_lo;
    r_lo = residual / (2.0 * r);
}

// With the larger operand in [2^-450, 2^450] the squares and their errors
// are normal doubles, so no scaling is needed.
constexpr double unscaled_low = 0x1p-450;
constexpr double unscaled_high = 0x1p450;

// hypot of a >= b > a * 2^-27 with a in [2^-450, 2^450], unscaled.
// r = sqrt(s) is within 0.75 ulp of the exact root, and the exact
// correction r_lo = residual / 2r decides the rounding: the result is r's
// upper neighbour exactly when residual > r * gap_up (r_lo above half the
// gap), the lower one when -residual > r * gap_down, and r otherwise. Both
// thresholds are exact products (gaps are powers of two), so no division
// and no hard-to-predict branch is needed. (An exact tie would need
// a^2 + b^2 to equal a 54-bit midpoint's square; the residual is not exact
// enough to detect one, as before.)
double hypot_unscaled(const double a, const double b) noexcept {
    double a_squared = 0.0;
    double a_squared_error = 0.0;
    two_product(a, a, a_squared, a_squared_error);
    double b_squared = 0.0;
    double b_squared_error = 0.0;
    two_product(b, b, b_squared, b_squared_error);
    double s = 0.0;
    double s_error = 0.0;
    two_sum(a_squared, b_squared, s, s_error);
    const double s_lo = s_error + (a_squared_error + b_squared_error);
    const double r = std::sqrt(s);  // IEEE sqrt: correctly rounded everywhere
    double r_squared = 0.0;
    double r_squared_error = 0.0;
    two_product(r, r, r_squared, r_squared_error);
    const double residual = ((s - r_squared) - r_squared_error) + s_lo;
    const std::uint64_t bits = std::bit_cast<std::uint64_t>(r);
    const double gap_up = std::bit_cast<double>(bits + 1) - r;
    const double gap_down = r - std::bit_cast<double>(bits - 1);
    const double up = residual > r * gap_up ? gap_up : 0.0;
    const double down = -residual > r * gap_down ? gap_down : 0.0;
    const double result = r + (up - down);
    return result;
}

double hypot_ordered(const double a, const double b) noexcept {
    if (b < a * negligible_ratio) return a;
    if (a >= unscaled_low && a <= unscaled_high) {
        const double result = hypot_unscaled(a, b);
        return result;
    }
    double r = 0.0;
    double r_lo = 0.0;
    std::int64_t exponent = 0;
    hypot_wide(a, b, r, r_lo, exponent);
    const double result = scale_wide(r, r_lo, exponent);
    return result;
}

} // namespace
} // namespace detail

namespace detail {
namespace {

// Every input the one-test fast path declines: NaN, infinities, zeros, a
// negligible smaller operand, and magnitudes needing scaling.
STX_AUXLIB_MATH_COLD double hypot_outside(const double x, const double y) noexcept {
    // Annex F: an infinite operand gives +infinity even when the other is NaN.
    if (std::isinf(x) || std::isinf(y)) return std::numeric_limits<double>::infinity();
    if (x != x || y != y) return x + y;
    const double a = std::fmax(std::fabs(x), std::fabs(y));
    const double b = std::fmin(std::fabs(x), std::fabs(y));
    if (a == 0.0) return 0.0;
    const double result = hypot_ordered(a, b);
    return result;
}

} // namespace
} // namespace detail

double hypot(const double x, const double y) noexcept {
    // The common case in one test: a in [2^-450, 2^450] and b >= a 2^-27.
    // Ordered by comparison (not fmax, which would drop a NaN), every
    // comparison with NaN is false, and infinities and zeros fail the
    // range, so all of them reach hypot_outside.
    const double ax = std::fabs(x);
    const double ay = std::fabs(y);
    const bool x_larger = ax >= ay;
    const double a = x_larger ? ax : ay;
    const double b = x_larger ? ay : ax;
    if (a >= detail::unscaled_low && a <= detail::unscaled_high && b >= a * detail::negligible_ratio) {
        const double result = detail::hypot_unscaled(a, b);
        return result;
    }
    const double result = detail::hypot_outside(x, y);
    return result;
}

// Float operands are exact doubles, so the double kernel applies; its
// ~1e-32 result r + r_lo is rounded once, directly to float (rounding it to
// double first could round twice). In the float range (r + r_lo) * 2^e
// scales exactly into normal doubles.
float hypot(const float x, const float y) noexcept {
    const double xd = static_cast<double>(x);
    const double yd = static_cast<double>(y);
    if (std::isinf(xd) || std::isinf(yd)) return std::numeric_limits<float>::infinity();
    if (xd != xd || yd != yd) return x + y;
    const double a = std::fmax(std::fabs(xd), std::fabs(yd));
    const double b = std::fmin(std::fabs(xd), std::fabs(yd));
    if (a == 0.0) return 0.0f;
    {
        // Cheap path: the squares of floats are exact doubles, so
        // sqrt(a^2 + b^2) in double carries two roundings (2^-52 relative
        // together); certified or abandoned.
        const double approx = std::sqrt(a * a + b * b);
        float fast = 0.0f;
        if (detail::float_if_certain(approx, approx * 0x1p-51, fast)) return fast;
    }
    // Below 2^-27 * a, b cannot move a's float rounding either (a is a float).
    if (b < a * detail::negligible_ratio) return static_cast<float>(a);
    double r = 0.0;
    double r_lo = 0.0;
    std::int64_t exponent = 0;
    detail::hypot_wide(a, b, r, r_lo, exponent);
    const double scale = detail::power_of_two(exponent);
    const float result = detail::round_wide_to_float(r * scale, r_lo * scale);
    return result;
}

} // namespace stx_auxlib::math
