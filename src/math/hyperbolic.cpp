// sinh, cosh, tanh, asinh, acosh and atanh (declared, with their contracts,
// in stx_auxlib/math.hpp).
//
// The direct functions come from the exp core: E = e^|x| as a double-double,
// sinh and cosh as (E -+ 1/E)/2, tanh as sinh/cosh, and e^|x|/2 alone where
// e^-|x| no longer matters (|x| > 37). Below |x| = 1/8, where E - 1/E
// cancels, sinh comes from its Taylor series with x^3 (1/6 + ...) formed as a
// double-double. The inverse functions come from the precise log core
// applied to a double-double argument v: log(v_hi + v_lo) = log v_hi +
// v_lo / v_hi, with v = |x| + sqrt(x^2 + 1), x + sqrt(x^2 - 1) (the square
// formed as (x - 1)(x + 1)), or (1 + x)/(1 - x).

#include "stx_auxlib/math.hpp"

#include "exp_core.hpp"
#include "log_core.hpp"

#include <limits>

#include "variant_functions.hpp"

STX_AUXLIB_MATH_TARGET_PUSH

namespace stx_auxlib::math::STX_AUXLIB_MATH_VARIANT {

namespace detail {
namespace {

constexpr double hyperbolic_tiny = 0x1p-27;     // below it sinh x = tanh x = x, cosh x = 1
constexpr double series_limit = 0.125;          // below it sinh uses its series
constexpr double single_exponential = 37.0;     // above it e^-x < 2^-106 e^x
constexpr double sinh_overflow = 710.4758600739439;  // above it sinh and cosh are infinite
constexpr double tanh_saturation = 22.0;        // above it tanh rounds to +-1
constexpr double inverse_large = 0x1p28;        // above it sqrt(x^2 +- 1) = x to 2^-56

constexpr double one_sixth_hi = 1.0 / 6.0;
constexpr double one_sixth_lo = 0x1.5555555555555p-57;  // 1/6 - double(1/6) = 2^-55 / 3
constexpr double sinh_5 = 1.0 / 120.0;
constexpr double sinh_7 = 1.0 / 5040.0;
constexpr double sinh_9 = 1.0 / 362880.0;
constexpr double sinh_11 = 1.0 / 39916800.0;
constexpr double sinh_13 = 1.0 / 6227020800.0;

// e^a for 0 <= a <= 709 as a double-double (normal throughout).
Wide exponential(const double a) noexcept {
    double head = 0.0;
    double tail = 0.0;
    std::int64_t exponent = 0;
    exp_core(a, 0.0, head, tail, exponent);
    const double scale = power_of_two(exponent);
    double hi = 0.0;
    double lo = 0.0;
    two_sum(head * scale, tail * scale, hi, lo);
    const Wide result{hi, lo};
    return result;
}

// e^a / 2 for a in (37, 710.48] as a double-double, or rounded to double.
double half_exponential(const double a) noexcept {
    double head = 0.0;
    double tail = 0.0;
    std::int64_t exponent = 0;
    exp_core(a, 0.0, head, tail, exponent);
    const double result = scale_wide(head, tail, exponent - 1);
    return result;
}

Wide half_exponential_wide(const double a) noexcept {
    double head = 0.0;
    double tail = 0.0;
    std::int64_t exponent = 0;
    exp_core(a, 0.0, head, tail, exponent);
    const double scale = power_of_two(exponent - 1);
    double hi = 0.0;
    double lo = 0.0;
    two_sum(head * scale, tail * scale, hi, lo);
    const Wide result{hi, lo};
    return result;
}

// sinh a for 0 <= a < 1/8: a + a^3 (1/6 + a^2 P(a^2)), truncated at
// a^15/15! (3.5e-25 relative).
Wide sinh_series(const double a) noexcept {
    const double a_squared_rounded = a * a;
    const double rest =
        a_squared_rounded *
        (sinh_5 + a_squared_rounded * (sinh_7 + a_squared_rounded * (sinh_9 + a_squared_rounded * (sinh_11 + a_squared_rounded * sinh_13))));
    double factor = 0.0;
    double factor_error = 0.0;
    two_sum(one_sixth_hi, rest, factor, factor_error);
    const double factor_lo = factor_error + one_sixth_lo;
    double square = 0.0;
    double square_error = 0.0;
    two_product(a, a, square, square_error);
    double cube = 0.0;
    double cube_error = 0.0;
    two_product(square, a, cube, cube_error);
    const double cube_lo = cube_error + square_error * a;
    double cubic = 0.0;
    double cubic_error = 0.0;
    two_product(cube, factor, cubic, cubic_error);
    const double cubic_lo = cubic_error + (cube * factor_lo + cube_lo * factor);
    double head = 0.0;
    double head_error = 0.0;
    two_sum(a, cubic, head, head_error);
    const Wide result{head, head_error + cubic_lo};
    return result;
}

// (e^a -+ e^-a) / 2 for 1/8 <= a <= 37, both exponentials from one
// reduction (exp_pair); e^a > e^-a, so the leading sum is a fast two-sum.
Wide exponential_pair(const double a, const bool difference) noexcept {
    Wide plus{};
    Wide minus{};
    exp_pair(a, plus, minus);
    const double sign = difference ? -1.0 : 1.0;
    double head = 0.0;
    double head_error = 0.0;
    fast_two_sum(plus.hi, sign * minus.hi, head, head_error);
    const double tail = head_error + (plus.lo + sign * minus.lo);
    const Wide result{head * 0.5, tail * 0.5};  // exact halving
    return result;
}

Wide sinh_wide(const double a) noexcept {
    if (a < series_limit) return sinh_series(a);
    const Wide result = exponential_pair(a, true);
    return result;
}

Wide cosh_wide(const double a) noexcept {
    const Wide result = exponential_pair(a, false);
    return result;
}

double signed_double(const Wide value, const bool negative) noexcept {
    const double magnitude = value.hi + value.lo;
    if (negative) return -magnitude;
    return magnitude;
}

float signed_float(const Wide value, const bool negative) noexcept {
    if (negative) {
        const float result = round_wide_to_float(-value.hi, -value.lo);
        return result;
    }
    const float result = round_wide_to_float(value.hi, value.lo);
    return result;
}

// log(v_hi + v_lo) for v_hi >= 1 (normal), as a double-double.
Wide log_of_wide(const double v_hi, const double v_lo) noexcept {
    double hi = 0.0;
    double lo = 0.0;
    two_sum(v_hi, v_lo, hi, lo);
    double head = 0.0;
    double tail = 0.0;
    // The plain log core (~2^-69 relative) suffices here; the precise one
    // exists for pow, whose error is multiplied by |y log x|.
    log_natural_plain(std::bit_cast<std::uint64_t>(hi), 0.0, head, tail);
    const Wide result{head, tail + lo / hi};
    return result;
}

// log(a) + ln 2 for large a (asinh, acosh above 2^28).
Wide log_twice(const double a) noexcept {
    double head = 0.0;
    double tail = 0.0;
    log_natural_plain(std::bit_cast<std::uint64_t>(a), 0.0, head, tail);
    double sum = 0.0;
    double sum_error = 0.0;
    two_sum(head, log_ln2_hi, sum, sum_error);
    const Wide result{sum, sum_error + (tail + log_ln2_lo)};
    return result;
}

// sqrt(s + s_lo) for a positive double-double, with one Newton correction.
Wide root_wide(const double s, const double s_lo) noexcept {
    const double root = std::sqrt(s);
    double root_squared = 0.0;
    double root_squared_error = 0.0;
    two_product(root, root, root_squared, root_squared_error);
    const double residual = ((s - root_squared) - root_squared_error) + s_lo;
    const Wide result{root, residual / (2.0 * root)};
    return result;
}

// asinh a for 2^-27 <= a <= 2^28: log(a + sqrt(a^2 + 1)).
Wide asinh_wide(const double a) noexcept {
    double square = 0.0;
    double square_error = 0.0;
    two_product(a, a, square, square_error);
    double sum = 0.0;
    double sum_error = 0.0;
    two_sum(square, 1.0, sum, sum_error);
    const Wide root = root_wide(sum, sum_error + square_error);
    double v = 0.0;
    double v_error = 0.0;
    two_sum(a, root.hi, v, v_error);
    const Wide result = log_of_wide(v, v_error + root.lo);
    return result;
}

// acosh x for 1 < x <= 2^28: log(x + sqrt((x - 1)(x + 1))).
Wide acosh_wide(const double x) noexcept {
    double below = 0.0;
    double below_error = 0.0;
    two_sum(x, -1.0, below, below_error);
    double above = 0.0;
    double above_error = 0.0;
    two_sum(x, 1.0, above, above_error);
    double square = 0.0;
    double square_error = 0.0;
    two_product(below, above, square, square_error);
    const double square_lo = square_error + (below * above_error + below_error * above);
    const Wide root = root_wide(square, square_lo);
    double v = 0.0;
    double v_error = 0.0;
    two_sum(x, root.hi, v, v_error);
    const Wide result = log_of_wide(v, v_error + root.lo);
    return result;
}

// atanh a for 2^-27 <= a < 1: log((1 + a)/(1 - a)) / 2.
Wide atanh_wide(const double a) noexcept {
    double numerator = 0.0;
    double numerator_error = 0.0;
    two_sum(1.0, a, numerator, numerator_error);
    double denominator = 0.0;
    double denominator_error = 0.0;
    two_sum(1.0, -a, denominator, denominator_error);
    const Wide ratio = divide(numerator, numerator_error, denominator, denominator_error);
    const Wide logarithm = log_of_wide(ratio.hi, ratio.lo);
    const Wide result{logarithm.hi * 0.5, logarithm.lo * 0.5};  // exact halving
    return result;
}

// ---- cheap float paths ------------------------------------------------------
//
// As in atan.cpp: plain-double kernels certified with float_if_certain, the
// double-double paths above as the fallback, identical bits either way. The
// bounds below are 4 to 8 times the derived worst case.

// The float rounding of the signed magnitude when certain under a relative
// error bound; false otherwise.
bool certified(const double magnitude, const double relative, const bool negative,
               float& result) noexcept {
    const double value = negative ? -magnitude : magnitude;
    const bool certain = float_if_certain(value, magnitude * relative, result);
    return certain;
}

// sinh a for 2^-27 <= a <= 90: the series below 1/8 (to a^9, truncation
// below 2^-55 relative); (E - 1/E)/2 up to 37, where E = exp_cheap(a) is
// within 2^-51 and 1/E adds one rounding, so the difference carries at most
// 2^-50 (E + 1/E), which coth(a) <= 8.03 turns into 2^-47 of the result;
// E/2 above. Returns the relative bound in `relative`.
double sinh_cheap(const double a, double& relative) noexcept {
    if (a < series_limit) {
        const double a2 = a * a;
        relative = 0x1p-48;
        const double result =
            a + a * (a2 * (one_sixth_hi + a2 * (sinh_5 + a2 * (sinh_7 + a2 * sinh_9))));
        return result;
    }
    const double e = exp_cheap(a);
    if (a > single_exponential) {
        relative = 0x1p-48;
        return e * 0.5;
    }
    relative = 0x1p-44;
    const double result = (e - 1.0 / e) * 0.5;
    return result;
}

// cosh a for 2^-27 <= a <= 90: (E + 1/E)/2, no cancellation (2^-50).
double cosh_cheap(const double a) noexcept {
    const double e = exp_cheap(a);
    if (a > single_exponential) return e * 0.5;
    const double result = (e + 1.0 / e) * 0.5;
    return result;
}

// tanh a for 2^-27 <= a <= 22: the series below 1/16 (to a^13; truncation
// below 2^-55 relative); (E2 - 1)/(E2 + 1) with E2 = exp_cheap(2a) above,
// where E2 - 1 >= 0.133 of E2 makes the relative error at most 2^-51 * 8.5
// plus three roundings: under 2^-46.5.
double tanh_cheap(const double a, double& relative) noexcept {
    if (a < 0.0625) {
        const double a2 = a * a;
        relative = 0x1p-48;
        const double polynomial =
            -1.0 / 3.0 +
            a2 * (2.0 / 15.0 +
                  a2 * (-17.0 / 315.0 +
                        a2 * (62.0 / 2835.0 + a2 * (-1382.0 / 155925.0 + a2 * (21844.0 / 6081075.0)))));
        const double result = a + a * (a2 * polynomial);
        return result;
    }
    relative = 0x1p-44;
    const double e2 = exp_cheap(2.0 * a);
    const double result = (e2 - 1.0) / (e2 + 1.0);
    return result;
}

// log of v (positive normal) in plain double, with log_cheap's bound plus
// `v_error` (an absolute bound on v's own relative error, carried 1:1).
double log_value_cheap(const double v, const double v_error, double& error) noexcept {
    double value = 0.0;
    double log_error = 0.0;
    log_cheap(natural, std::bit_cast<std::uint64_t>(v), value, log_error);
    error = log_error + v_error;
    return value;
}

} // namespace
} // namespace detail

double sinh(const double x) noexcept {
    if (x != x) return x + x;
    const double a = std::fabs(x);
    const bool negative = std::signbit(x);
    if (a < detail::hyperbolic_tiny) return x;
    if (a > detail::sinh_overflow) return negative ? -std::numeric_limits<double>::infinity()
                                                   : std::numeric_limits<double>::infinity();
    if (a > detail::single_exponential) {
        const double magnitude = detail::half_exponential(a);
        return negative ? -magnitude : magnitude;
    }
    const double result = detail::signed_double(detail::sinh_wide(a), negative);
    return result;
}

float sinh(const float value) noexcept {
    const double x = detail::widen(value);
    if (x != x) return value + value;
    const double a = std::fabs(x);
    const bool negative = std::signbit(x);
    if (a < detail::hyperbolic_tiny) return value;
    // Above 90 every float sinh overflows.
    if (a > 90.0) return negative ? -std::numeric_limits<float>::infinity()
                                  : std::numeric_limits<float>::infinity();
    {
        double relative = 0.0;
        const double magnitude = detail::sinh_cheap(a, relative);
        float fast = 0.0f;
        if (detail::certified(magnitude, relative, negative, fast)) return fast;
    }
    if (a > detail::single_exponential) {
        return detail::signed_float(detail::half_exponential_wide(a), negative);
    }
    const float result = detail::signed_float(detail::sinh_wide(a), negative);
    return result;
}

double cosh(const double x) noexcept {
    if (x != x) return x + x;
    const double a = std::fabs(x);
    if (a < detail::hyperbolic_tiny) return 1.0;
    if (a > detail::sinh_overflow) return std::numeric_limits<double>::infinity();
    if (a > detail::single_exponential) return detail::half_exponential(a);
    const detail::Wide value = detail::cosh_wide(a);
    const double result = value.hi + value.lo;
    return result;
}

float cosh(const float value) noexcept {
    const double x = detail::widen(value);
    if (x != x) return value + value;
    const double a = std::fabs(x);
    if (a < detail::hyperbolic_tiny) return 1.0f;
    if (a > 90.0) return std::numeric_limits<float>::infinity();
    {
        float fast = 0.0f;
        if (detail::certified(detail::cosh_cheap(a), 0x1p-47, false, fast)) return fast;
    }
    if (a > detail::single_exponential) {
        const detail::Wide half = detail::half_exponential_wide(a);
        return detail::round_wide_to_float(half.hi, half.lo);
    }
    const detail::Wide cosine = detail::cosh_wide(a);
    const float result = detail::round_wide_to_float(cosine.hi, cosine.lo);
    return result;
}

namespace detail {
namespace {

// tanh a for 2^-27 <= a <= 22 as a double-double. Below 1/8, sinh/cosh
// from the series and the exponential pair (E2 - 1 would cancel there);
// above, (E2 - 1)/(E2 + 1) with E2 = e^(2a) from one exp core call: 1 and
// E2 are summed exactly into double-doubles, and one division finishes.
Wide tanh_wide(const double a) noexcept {
    if (a < series_limit) {
        const Wide s = sinh_wide(a);
        const Wide c = cosh_wide(a);
        double divisor = 0.0;
        double divisor_lo = 0.0;
        two_sum(c.hi, c.lo, divisor, divisor_lo);
        double dividend = 0.0;
        double dividend_lo = 0.0;
        two_sum(s.hi, s.lo, dividend, dividend_lo);
        const Wide result = divide(dividend, dividend_lo, divisor, divisor_lo);
        return result;
    }
    const Wide e2 = exponential(2.0 * a);  // 2a is exact
    double numerator = 0.0;
    double numerator_error = 0.0;
    fast_two_sum(e2.hi, -1.0, numerator, numerator_error);  // e2 >= e^0.25 > 1
    double denominator = 0.0;
    double denominator_error = 0.0;
    fast_two_sum(e2.hi, 1.0, denominator, denominator_error);
    const Wide result = divide(numerator, numerator_error + e2.lo, denominator,
                               denominator_error + e2.lo);
    return result;
}

} // namespace
} // namespace detail

double tanh(const double x) noexcept {
    if (x != x) return x + x;
    const double a = std::fabs(x);
    const bool negative = std::signbit(x);
    if (a < detail::hyperbolic_tiny) return x;
    if (a > detail::tanh_saturation) return negative ? -1.0 : 1.0;
    const double result = detail::signed_double(detail::tanh_wide(a), negative);
    return result;
}

float tanh(const float value) noexcept {
    const double x = detail::widen(value);
    if (x != x) return value + value;
    const double a = std::fabs(x);
    const bool negative = std::signbit(x);
    if (a < detail::hyperbolic_tiny) return value;
    if (a > detail::tanh_saturation) return negative ? -1.0f : 1.0f;
    {
        double relative = 0.0;
        const double magnitude = detail::tanh_cheap(a, relative);
        float fast = 0.0f;
        if (detail::certified(magnitude, relative, negative, fast)) return fast;
    }
    const float result = detail::signed_float(detail::tanh_wide(a), negative);
    return result;
}

double asinh(const double x) noexcept {
    if (x != x || std::isinf(x)) return x + x;
    const double a = std::fabs(x);
    const bool negative = std::signbit(x);
    if (a < detail::hyperbolic_tiny) return x;
    if (a > detail::inverse_large) return detail::signed_double(detail::log_twice(a), negative);
    const double result = detail::signed_double(detail::asinh_wide(a), negative);
    return result;
}

float asinh(const float value) noexcept {
    const double x = detail::widen(value);
    if (x != x || std::isinf(x)) return value + value;
    const double a = std::fabs(x);
    const bool negative = std::signbit(x);
    if (a < detail::hyperbolic_tiny) return value;
    if (a > detail::inverse_large) return detail::signed_float(detail::log_twice(a), negative);
    {
        double magnitude = 0.0;
        double error = 0.0;
        if (a < 0x1p-5) {
            // asinh a = a - a^3/6 + 3a^5/40 - 15a^7/336 + 105a^9/3456 - ...
            // (truncation below 2^-55 relative).
            const double a2 = a * a;
            magnitude = a + a * (a2 * (-1.0 / 6.0 + a2 * (3.0 / 40.0 + a2 * (-15.0 / 336.0 + a2 * (105.0 / 3456.0)))));
            error = magnitude * 0x1p-48;
        } else {
            // v = a + sqrt(a^2 + 1) carries at most 4 roundings relative;
            // the result is at least asinh(2^-5) = 0.031.
            const double v = a + std::sqrt(a * a + 1.0);
            magnitude = detail::log_value_cheap(v, 0x1p-50, error);
            error *= 4.0;
        }
        const double signed_value = negative ? -magnitude : magnitude;
        float fast = 0.0f;
        if (detail::float_if_certain(signed_value, error, fast)) return fast;
    }
    const float result = detail::signed_float(detail::asinh_wide(a), negative);
    return result;
}

double acosh(const double x) noexcept {
    if (x != x) return x + x;
    if (x < 1.0) return std::numeric_limits<double>::quiet_NaN();
    if (x == 1.0) return 0.0;
    if (std::isinf(x)) return x;
    if (x > detail::inverse_large) {
        const detail::Wide value = detail::log_twice(x);
        return value.hi + value.lo;
    }
    const detail::Wide value = detail::acosh_wide(x);
    const double result = value.hi + value.lo;
    return result;
}

float acosh(const float input) noexcept {
    const double x = detail::widen(input);
    if (x != x) return input + input;
    if (x < 1.0) return std::numeric_limits<float>::quiet_NaN();
    if (x == 1.0) return 0.0f;
    if (std::isinf(x)) return input;
    if (x <= detail::inverse_large) {
        // w = sqrt((x - 1)(x + 1)) and v = x + w carry at most 5 roundings
        // relative to v; log v then has that as absolute error.
        const double w = std::sqrt((x - 1.0) * (x + 1.0));
        double error = 0.0;
        const double approx = detail::log_value_cheap(x + w, 0x1p-50, error);
        float fast = 0.0f;
        if (detail::float_if_certain(approx, error * 4.0, fast)) return fast;
    }
    detail::Wide value{};
    if (x > detail::inverse_large) {
        value = detail::log_twice(x);
    } else {
        value = detail::acosh_wide(x);
    }
    const float result = detail::round_wide_to_float(value.hi, value.lo);
    return result;
}

double atanh(const double x) noexcept {
    if (x != x) return x + x;
    const double a = std::fabs(x);
    const bool negative = std::signbit(x);
    if (a > 1.0) return std::numeric_limits<double>::quiet_NaN();
    if (a == 1.0) return negative ? -std::numeric_limits<double>::infinity()
                                  : std::numeric_limits<double>::infinity();
    if (a < detail::hyperbolic_tiny) return x;
    const double result = detail::signed_double(detail::atanh_wide(a), negative);
    return result;
}

float atanh(const float value) noexcept {
    const double x = detail::widen(value);
    if (x != x) return value + value;
    const double a = std::fabs(x);
    const bool negative = std::signbit(x);
    if (a > 1.0) return std::numeric_limits<float>::quiet_NaN();
    if (a == 1.0) return negative ? -std::numeric_limits<float>::infinity()
                                  : std::numeric_limits<float>::infinity();
    if (a < detail::hyperbolic_tiny) return value;
    {
        double magnitude = 0.0;
        double error = 0.0;
        if (a < 0x1p-12) {
            // atanh a = a + a^3/3 + a^5/5 + ... (truncation below 2^-70).
            const double a2 = a * a;
            magnitude = a + a * (a2 * (1.0 / 3.0 + a2 * 0.2));
            error = magnitude * 0x1p-48;
        } else {
            // q = (1 + a)/(1 - a) carries at most 3 roundings relative; half
            // its log then errs by at most half of log_cheap's bound plus
            // 2^-52; the result is at least atanh(2^-12).
            const double q = (1.0 + a) / (1.0 - a);
            magnitude = detail::log_value_cheap(q, 0x1p-50, error) * 0.5;
            error *= 2.0;
        }
        const double signed_value = negative ? -magnitude : magnitude;
        float fast = 0.0f;
        if (detail::float_if_certain(signed_value, error, fast)) return fast;
    }
    const float result = detail::signed_float(detail::atanh_wide(a), negative);
    return result;
}

} // namespace stx_auxlib::math::STX_AUXLIB_MATH_VARIANT

STX_AUXLIB_MATH_TARGET_POP
