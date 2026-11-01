// atan, atan2, asin and acos (declared, with their contracts, in
// stx_auxlib/math.hpp).
//
// One core, atan_unit, takes u in [0, 1] as a double-double u + u_lo:
//   atan u = atan c + atan d,   c = j/128 nearest u,   d = (u - c) / (1 + u c),
// with atan c from a 129-entry table (hi + lo), |d| <= 1/256 formed as a
// double-double, and atan d - d from a Taylor polynomial truncated at
// d^11/11 ~ 3e-28. Every function reduces to it: atan above 1 through
// pi/2 - atan(1/x), atan2 through the ratio of the smaller to the larger
// operand and the quadrant, asin and acos through atan2 of x and
// sqrt(1 - x^2), with 1 - x^2 = (1 - x)(1 + x) formed exactly.

#include "stx_auxlib/math.hpp"

#include "atan_table128.hpp"
#include "kernel_support.hpp"

#include <limits>

namespace stx_auxlib::math {

namespace detail {
namespace {

constexpr double atan_tiny_limit = 0x1p-27;  // below it atan x = x (x^3/3 < ulp/2)
constexpr double atan_huge_limit = 0x1p60;   // above it atan x rounds to pi/2
constexpr double atan_3 = -1.0 / 3.0;
constexpr double atan_5 = 1.0 / 5.0;
constexpr double atan_7 = -1.0 / 7.0;
constexpr double atan_9 = 1.0 / 9.0;

// atan(u + u_lo) for 0 <= u <= 1, as a double-double.
Wide atan_unit(const double u, const double u_lo) noexcept {
    const std::int64_t j = static_cast<std::int64_t>(u * 128.0 + 0.5);
    const double c = static_cast<double>(j) / 128.0;  // exact
    // Numerator u - c + u_lo: u - c is exact (u is within 1/256 of c).
    double numerator = 0.0;
    double numerator_lo = 0.0;
    two_sum(u - c, u_lo, numerator, numerator_lo);
    // Denominator 1 + u c as a double-double.
    double uc = 0.0;
    double uc_error = 0.0;
    two_product(u, c, uc, uc_error);
    double denominator = 0.0;
    double denominator_error = 0.0;
    two_sum(1.0, uc, denominator, denominator_error);
    const double denominator_lo = denominator_error + (uc_error + u_lo * c);
    const Wide d = divide(numerator, numerator_lo, denominator, denominator_lo);
    // atan(d + d_lo) = d + [d_lo (1 - d^2) + d * polynomial(d^2)].
    const double d_squared = d.hi * d.hi;
    const double polynomial = d_squared * (atan_3 + d_squared * (atan_5 + d_squared * (atan_7 + d_squared * atan_9)));
    const AtanEntry& entry = atan_table_129[j];
    double head = 0.0;
    double head_error = 0.0;
    two_sum(entry.hi, d.hi, head, head_error);
    const double tail = head_error + (entry.lo + (d.lo * (1.0 - d_squared) + d.hi * polynomial));
    const Wide result{head, tail};
    return result;
}

// pi/2 - a and pi - a for a double-double a.
Wide half_pi_minus(const Wide a) noexcept {
    double head = 0.0;
    double head_error = 0.0;
    two_sum(atan_half_pi_hi, -a.hi, head, head_error);
    const Wide result{head, head_error + (atan_half_pi_lo - a.lo)};
    return result;
}

Wide pi_minus(const Wide a) noexcept {
    double head = 0.0;
    double head_error = 0.0;
    two_sum(atan_pi_hi, -a.hi, head, head_error);
    const Wide result{head, head_error + (atan_pi_lo - a.lo)};
    return result;
}

// The angle of (x, y) in [0, pi/2] for non-negative double-doubles x, y,
// not both zero: atan of the smaller over the larger, flipped above pi/4.
Wide first_quadrant_angle(const Wide x, const Wide y) noexcept {
    if (y.hi <= x.hi) {
        const Wide ratio = divide(y.hi, y.lo, x.hi, x.lo);
        const Wide result = atan_unit(ratio.hi, ratio.lo);
        return result;
    }
    const Wide ratio = divide(x.hi, x.lo, y.hi, y.lo);
    const Wide result = half_pi_minus(atan_unit(ratio.hi, ratio.lo));
    return result;
}

// atan of |x| for finite |x| in [tiny_limit, huge_limit].
Wide atan_magnitude(const double a) noexcept {
    if (a <= 1.0) {
        const Wide result = atan_unit(a, 0.0);
        return result;
    }
    const Wide inverse = divide(1.0, 0.0, a, 0.0);
    const Wide result = half_pi_minus(atan_unit(inverse.hi, inverse.lo));
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

// Ratios below 2^-60 make atan2 the quotient itself (or pi, pi/2 next to it):
// atan r = r (1 - r^2/3 + ...) rounds to r.
constexpr double atan2_negligible_ratio = 0x1p-60;
// atan2_finite's exact pre-scaling of operands far from 1 (see there).
constexpr double atan2_far_above = 0x1p900;
constexpr double atan2_far_below = 0x1p-900;
constexpr double atan2_far_step_down = 0x1p-600;
constexpr double atan2_far_step_up = 0x1p600;

// atan2 for finite, nonzero x and y, as the signed angle in double-double,
// or `direct` when the quotient or a constant is already the rounded result.
// Returns true when `direct` holds the answer.
bool atan2_finite(const double y, const double x, Wide& angle, double& direct) noexcept {
    const double ax = std::fabs(x);
    const double ay = std::fabs(y);
    const bool negative = std::signbit(y);
    if (ay < ax * atan2_negligible_ratio) {
        if (x > 0.0) {
            direct = y / x;  // IEEE division: the correctly rounded quotient
        } else {
            direct = negative ? -atan_pi_hi : atan_pi_hi;
        }
        return true;
    }
    if (ax < ay * atan2_negligible_ratio) {
        direct = negative ? -atan_half_pi_hi : atan_half_pi_hi;
        return true;
    }
    // Scale both by the larger's exponent, which puts the larger in [0.5, 1)
    // and keeps the smaller above 2^-61 (the ratio is at least 2^-60 here),
    // so both products are exact. The exponent is read from the larger's
    // bits once it is a normal double within 2^+-900; outside that, both are
    // first moved 600 octaves toward 1, exactly (the smaller stays normal or
    // only grows), so 2^-exponent is itself a normal double. The libm frexp
    // and two ldexp calls this replaces cost more than the rest of the
    // reduction (x86-64 review, 2026-11-01); the scaled values are the same.
    double larger = std::fmax(ax, ay);
    double scaled_x = ax;
    double scaled_y = ay;
    if (larger > atan2_far_above) {
        larger *= atan2_far_step_down;
        scaled_x *= atan2_far_step_down;
        scaled_y *= atan2_far_step_down;
    } else if (larger < atan2_far_below) {
        larger *= atan2_far_step_up;
        scaled_x *= atan2_far_step_up;
        scaled_y *= atan2_far_step_up;
    }
    // larger is in [2^(exponent - 1), 2^exponent).
    const std::int64_t exponent =
        static_cast<std::int64_t>(std::bit_cast<std::uint64_t>(larger) >> 52) - (exponent_bias - 1);
    const double scale = power_of_two(-exponent);
    const Wide sx{scaled_x * scale, 0.0};
    const Wide sy{scaled_y * scale, 0.0};
    Wide result = first_quadrant_angle(sx, sy);
    if (x < 0.0) result = pi_minus(result);
    if (negative) result = Wide{-result.hi, -result.lo};
    angle = result;
    return false;
}

// atan2's Annex F cases: NaN, zeros and infinities. Returns true when the
// result is in `result`.
bool atan2_special(const double y, const double x, double& result) noexcept {
    if (x != x || y != y) {
        result = x + y;
        return true;
    }
    const bool negative = std::signbit(y);
    const double sign = negative ? -1.0 : 1.0;
    if (y == 0.0) {
        // +-0 against x > 0 or +0 keeps y; against x < 0 or -0 gives +-pi.
        if (x > 0.0 || (x == 0.0 && !std::signbit(x))) {
            result = y;
        } else {
            result = sign * atan_pi_hi;
        }
        return true;
    }
    if (x == 0.0) {
        result = sign * atan_half_pi_hi;
        return true;
    }
    if (std::isinf(y)) {
        if (std::isinf(x)) {
            // +-pi/4 or +-3pi/4, rounded: atan(1) and pi - atan(1).
            const Wide quarter{atan_table_129[128].hi, atan_table_129[128].lo};
            const Wide angle = x > 0.0 ? quarter : pi_minus(quarter);
            result = sign * (angle.hi + angle.lo);
        } else {
            result = sign * atan_half_pi_hi;
        }
        return true;
    }
    if (std::isinf(x)) {
        result = x > 0.0 ? sign * 0.0 : sign * atan_pi_hi;
        return true;
    }
    return false;
}

// sqrt(1 - a^2) for 0 <= a <= 1 as a double-double: 1 - a^2 = (1 - a)(1 + a)
// with both factors as double-doubles, then sqrt and a Newton correction.
Wide complement_root(const double a) noexcept {
    double one_minus = 0.0;
    double one_minus_error = 0.0;
    two_sum(1.0, -a, one_minus, one_minus_error);
    double one_plus = 0.0;
    double one_plus_error = 0.0;
    two_sum(1.0, a, one_plus, one_plus_error);
    double square = 0.0;
    double square_error = 0.0;
    two_product(one_minus, one_plus, square, square_error);
    const double square_lo = square_error + (one_minus * one_plus_error + one_minus_error * one_plus);
    const double root = std::sqrt(square);  // IEEE sqrt: correctly rounded
    if (root == 0.0) {
        const Wide result{0.0, 0.0};
        return result;
    }
    double root_squared = 0.0;
    double root_squared_error = 0.0;
    two_product(root, root, root_squared, root_squared_error);
    const double residual = ((square - root_squared) - root_squared_error) + square_lo;
    const Wide result{root, residual / (2.0 * root)};
    return result;
}

// ---- cheap float paths ------------------------------------------------------
//
// Float results come first from plain-double versions of the kernels below,
// certified with float_if_certain against a bound of 2^-46 of the result
// (the derivations give at most ~2^-49), and only otherwise from the
// double-double path. A certified result is the correctly rounded float,
// which the double-double path also returns, so the bits do not depend on
// which path ran.
constexpr double float_relative_bound = 0x1p-46;

// atan u for 0 <= u <= 1 in plain double. With c = j/128 exact, u - c exact
// and d = (u - c)/(1 + u c) carrying three roundings, the polynomial
// truncated at d^7/7 (below 2^-50 of d) and two final roundings, the error
// stays below 8 * 2^-53 of (|atan c| + |d|) <= 3 |atan u|: under 2^-49.4
// relative (for j >= 1 |d| <= atan(c)/2; for j = 0 the result is d itself).
double atan_unit_cheap(const double u) noexcept {
    const std::int64_t j = static_cast<std::int64_t>(u * 128.0 + 0.5);
    const double c = static_cast<double>(j) / 128.0;
    const double d = (u - c) / (1.0 + u * c);
    const double d_squared = d * d;
    const AtanEntry& entry = atan_table_129[j];
    const double result = entry.hi + (d + d * (d_squared * (atan_3 + d_squared * atan_5)));
    return result;
}

// The angle of (x, y) in [0, pi/2] for x, y >= 0 not both zero, in plain
// double; a ratio error of e relative moves atan by at most e times the
// angle (u / (1 + u^2) <= atan u), and pi/2 - t adds two roundings.
double first_quadrant_cheap(const double x, const double y) noexcept {
    if (y <= x) {
        const double result = atan_unit_cheap(y / x);
        return result;
    }
    const double result = (atan_half_pi_hi - atan_unit_cheap(x / y)) + atan_half_pi_lo;
    return result;
}

} // namespace
} // namespace detail

double atan(const double x) noexcept {
    if (x != x) return x + x;
    const double a = std::fabs(x);
    if (a < detail::atan_tiny_limit) return x;
    const bool negative = std::signbit(x);
    if (a > detail::atan_huge_limit) return negative ? -detail::atan_half_pi_hi : detail::atan_half_pi_hi;
    const double result = detail::signed_double(detail::atan_magnitude(a), negative);
    return result;
}

float atan(const float value) noexcept {
    const double x = detail::widen(value);
    if (x != x) return value + value;
    const double a = std::fabs(x);
    if (a < detail::atan_tiny_limit) return value;
    const bool negative = std::signbit(x);
    if (a > detail::atan_huge_limit) {
        const detail::Wide half_pi{detail::atan_half_pi_hi, detail::atan_half_pi_lo};
        return detail::signed_float(half_pi, negative);
    }
    {
        const double angle = detail::first_quadrant_cheap(1.0, a);
        const double signed_angle = negative ? -angle : angle;
        float fast = 0.0f;
        if (detail::float_if_certain(signed_angle, angle * detail::float_relative_bound, fast)) return fast;
    }
    const float result = detail::signed_float(detail::atan_magnitude(a), negative);
    return result;
}

double atan2(const double y, const double x) noexcept {
    double special = 0.0;
    if (detail::atan2_special(y, x, special)) return special;
    detail::Wide angle{};
    double direct = 0.0;
    if (detail::atan2_finite(y, x, angle, direct)) return direct;
    const double result = angle.hi + angle.lo;
    return result;
}

// Float operands are exact doubles; specials and the direct cases (quotient,
// pi, pi/2) are rounded to float from their exact or double-double values.
float atan2(const float y, const float x) noexcept {
    const double yd = detail::widen(y);
    const double xd = detail::widen(x);
    const bool negative = std::signbit(yd);
    double special = 0.0;
    if (detail::atan2_special(yd, xd, special)) {
        // Specials are 0, +-pi/2, +-pi, +-pi/4, +-3pi/4: round those once.
        const double magnitude = std::fabs(special);
        if (magnitude == detail::atan_pi_hi) {
            return detail::signed_float(detail::Wide{detail::atan_pi_hi, detail::atan_pi_lo}, negative);
        }
        if (magnitude == detail::atan_half_pi_hi) {
            return detail::signed_float(detail::Wide{detail::atan_half_pi_hi, detail::atan_half_pi_lo},
                                        negative);
        }
        return static_cast<float>(special);
    }
    {
        // Cheap path for finite nonzero operands: the ratio of two floats
        // in double carries one rounding.
        double angle = detail::first_quadrant_cheap(std::fabs(xd), std::fabs(yd));
        if (xd < 0.0) angle = (detail::atan_pi_hi - angle) + detail::atan_pi_lo;
        const double signed_angle = negative ? -angle : angle;
        float fast = 0.0f;
        if (detail::float_if_certain(signed_angle, angle * detail::float_relative_bound, fast)) return fast;
    }
    detail::Wide angle{};
    double direct = 0.0;
    if (detail::atan2_finite(yd, xd, angle, direct)) {
        const double magnitude = std::fabs(direct);
        if (magnitude == detail::atan_pi_hi) {
            return detail::signed_float(detail::Wide{detail::atan_pi_hi, detail::atan_pi_lo}, negative);
        }
        if (magnitude == detail::atan_half_pi_hi) {
            return detail::signed_float(detail::Wide{detail::atan_half_pi_hi, detail::atan_half_pi_lo},
                                        negative);
        }
        // A quotient of floats: y / x rounded once, to float.
        const detail::Wide quotient = detail::divide(yd, 0.0, xd, 0.0);
        return detail::round_wide_to_float(quotient.hi, quotient.lo);
    }
    const float result = detail::round_wide_to_float(angle.hi, angle.lo);
    return result;
}

double asin(const double x) noexcept {
    if (x != x) return x + x;
    const double a = std::fabs(x);
    if (a > 1.0) return std::numeric_limits<double>::quiet_NaN();
    if (a < detail::atan_tiny_limit) return x;
    const bool negative = std::signbit(x);
    const detail::Wide root = detail::complement_root(a);
    const detail::Wide angle = detail::first_quadrant_angle(root, detail::Wide{a, 0.0});
    const double result = detail::signed_double(angle, negative);
    return result;
}

float asin(const float value) noexcept {
    const double x = detail::widen(value);
    if (x != x) return value + value;
    const double a = std::fabs(x);
    if (a > 1.0) return std::numeric_limits<float>::quiet_NaN();
    if (a < detail::atan_tiny_limit) return value;
    const bool negative = std::signbit(x);
    {
        // sqrt((1 - a)(1 + a)) in plain double: at most 3.5 roundings
        // relative, which the angle carries at most proportionally.
        const double root = std::sqrt((1.0 - a) * (1.0 + a));
        const double angle = detail::first_quadrant_cheap(root, a);
        const double signed_angle = negative ? -angle : angle;
        float fast = 0.0f;
        if (detail::float_if_certain(signed_angle, angle * detail::float_relative_bound, fast)) return fast;
    }
    const detail::Wide root = detail::complement_root(a);
    const detail::Wide angle = detail::first_quadrant_angle(root, detail::Wide{a, 0.0});
    const float result = detail::signed_float(angle, negative);
    return result;
}

namespace detail {
namespace {

// acos as a double-double: the angle of (x, sqrt(1 - x^2)) for |x| <= 1.
Wide acos_wide(const double x) noexcept {
    const double a = std::fabs(x);
    const Wide root = complement_root(a);
    Wide angle = first_quadrant_angle(Wide{a, 0.0}, root);
    if (x < 0.0) angle = pi_minus(angle);
    return angle;
}

} // namespace
} // namespace detail

double acos(const double x) noexcept {
    if (x != x) return x + x;
    if (std::fabs(x) > 1.0) return std::numeric_limits<double>::quiet_NaN();
    if (x == 1.0) return 0.0;
    const detail::Wide angle = detail::acos_wide(x);
    const double result = angle.hi + angle.lo;
    return result;
}

float acos(const float value) noexcept {
    const double x = detail::widen(value);
    if (x != x) return value + value;
    if (std::fabs(x) > 1.0) return std::numeric_limits<float>::quiet_NaN();
    if (x == 1.0) return 0.0f;
    {
        const double a = std::fabs(x);
        const double root = std::sqrt((1.0 - a) * (1.0 + a));
        double angle = detail::first_quadrant_cheap(a, root);
        if (x < 0.0) angle = (detail::atan_pi_hi - angle) + detail::atan_pi_lo;
        float fast = 0.0f;
        if (detail::float_if_certain(angle, angle * detail::float_relative_bound, fast)) return fast;
    }
    const detail::Wide angle = detail::acos_wide(x);
    const float result = detail::round_wide_to_float(angle.hi, angle.lo);
    return result;
}

} // namespace stx_auxlib::math
