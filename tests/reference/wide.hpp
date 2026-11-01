#pragma once

// Double-double arithmetic and the high-accuracy references that
// stx_auxlib::math is measured against (house style section 8: the
// reference and the comparison are kept permanently). Test and tool code
// only; it uses std::fma, whose result is exact everywhere.
//
// sin/cos: Cody-Waite reduction by pi/2 held to ~160 bits (three doubles),
// carried in double-double, then Taylor series in double-double to ~1e-33.
// exp: the same reduction by ln 2, then Taylor series; checked against an
// independent constant for e (tests/math_tests.cpp).
// log: one Newton step on exp_reference from the C++ library's log, which
// squares a 1e-16 error to ~1e-32; checked by exp(log x) = x.

#include <cmath>
#include <cstdint>

namespace wide {

// Double-double: value = hi + lo, |lo| <= ulp(hi)/2.
struct Wide final {
    double hi{0.0};
    double lo{0.0};
};

inline Wide two_sum(const double a, const double b) {
    const double s = a + b;
    const double bb = s - a;
    const double e = (a - (s - bb)) + (b - bb);
    const Wide result{s, e};
    return result;
}

inline Wide quick_two_sum(const double a, const double b) {
    const double s = a + b;
    const double e = b - (s - a);
    const Wide result{s, e};
    return result;
}

inline Wide add(const Wide a, const Wide b) {
    const Wide s = two_sum(a.hi, b.hi);
    const Wide t = two_sum(a.lo, b.lo);
    Wide u = quick_two_sum(s.hi, s.lo + t.hi);
    u = quick_two_sum(u.hi, u.lo + t.lo);
    return u;
}

inline Wide negate(const Wide a) {
    const Wide result{-a.hi, -a.lo};
    return result;
}

inline Wide multiply(const Wide a, const Wide b) {
    const double p = a.hi * b.hi;
    const double e = std::fma(a.hi, b.hi, -p);
    const double cross = a.hi * b.lo + a.lo * b.hi;
    const Wide result = quick_two_sum(p, e + cross);
    return result;
}

inline Wide multiply(const Wide a, const double b) {
    const Wide wide_b{b, 0.0};
    const Wide result = multiply(a, wide_b);
    return result;
}

// a / b by one Newton correction of the double quotient.
inline Wide divide(const Wide a, const double b) {
    const double q1 = a.hi / b;
    const Wide product = multiply(Wide{b, 0.0}, q1);
    const Wide remainder = add(a, negate(product));
    const double q2 = remainder.hi / b;
    const Wide result = quick_two_sum(q1, q2);
    return result;
}

// pi/2 as four doubles (QD library constants halved; checked against a
// 115-digit Machin pi: the sum is within 3e-66), ~218 significant bits.
constexpr double half_pi_1 = 1.5707963267948966192313216916397514;
constexpr double half_pi_2 = 6.123233995736766035868820147291818e-17;
constexpr double half_pi_3 = -1.4973849048591698329435713513330e-33;
constexpr double half_pi_4 = 0x1.4cf98e804177dp-164;

// x - k*pi/2 in double-double. Each k*half_pi_i is formed exactly as a
// double-double, so the error is k * |pi/2 - (p1+...+p4)| ~ k * 3e-66 plus
// double-double rounding (~1e-32 of r) -- ample for |x| <= 2^32.
inline Wide reduce(const double x, const double k) {
    Wide r{x, 0.0};
    const double parts[4] = {half_pi_1, half_pi_2, half_pi_3, half_pi_4};
    for (const double part : parts) {
        const double p = k * part;
        const double e = std::fma(k, part, -p);
        r = add(r, Wide{-p, -e});
    }
    return r;
}

struct WideSinCos final {
    Wide s{};
    Wide c{};
};

// Taylor series for |r| <= pi/4 + tiny; 22 terms each reach far below 1e-33.
inline WideSinCos series(const Wide r) {
    const Wide r2 = multiply(r, r);
    Wide sin_term = r;              // r^(2n+1)/(2n+1)!
    Wide cos_term{1.0, 0.0};        // r^(2n)/(2n)!
    Wide sin_sum = r;
    Wide cos_sum{1.0, 0.0};
    for (int n = 1; n <= 22; ++n) {
        sin_term = divide(multiply(sin_term, r2), static_cast<double>((2 * n) * (2 * n + 1)));
        cos_term = divide(multiply(cos_term, r2), static_cast<double>((2 * n - 1) * (2 * n)));
        if (n % 2 == 1) {
            sin_sum = add(sin_sum, negate(sin_term));
            cos_sum = add(cos_sum, negate(cos_term));
        } else {
            sin_sum = add(sin_sum, sin_term);
            cos_sum = add(cos_sum, cos_term);
        }
    }
    const WideSinCos result{sin_sum, cos_sum};
    return result;
}

inline WideSinCos reference(const double x) {
    const double k = std::nearbyint(x / half_pi_1);
    const Wide r = reduce(x, k);
    const WideSinCos base = series(r);
    const std::int64_t quadrant = static_cast<std::int64_t>(k) & 3;
    WideSinCos result{};
    if (quadrant == 0) {
        result = base;
    } else if (quadrant == 1) {
        result = WideSinCos{base.c, negate(base.s)};
    } else if (quadrant == 2) {
        result = WideSinCos{negate(base.s), negate(base.c)};
    } else {
        result = WideSinCos{negate(base.c), base.s};
    }
    return result;
}

inline double wide_to_double(const Wide a) {
    const double result = a.hi + a.lo;
    return result;
}

// ---- exp ----------------------------------------------------------------------

// ln 2 as three doubles (QD library constants), ~160 significant bits.
constexpr double ln2_1 = 6.931471805599452862e-01;
constexpr double ln2_2 = 2.319046813846299558e-17;
constexpr double ln2_3 = 5.707708438416212066e-34;

// e as a double-double (QD), for checking exp_reference independently of ln2.
constexpr Wide e_constant{2.718281828459045091e+00, 1.445646891729250158e-16};

// e^r for |r| <= ln2 by Taylor series; 32 terms reach far below 1e-33.
inline Wide exp_series(const Wide r) {
    Wide term{1.0, 0.0};
    Wide sum{1.0, 0.0};
    for (int n = 1; n <= 32; ++n) {
        term = divide(multiply(term, r), static_cast<double>(n));
        sum = add(sum, term);
    }
    return sum;
}

// e^x = mantissa * 2^exponent with mantissa a double-double in
// [sqrt(1/2), sqrt(2)]. Kept unscaled: scaling the low part by 2^exponent
// would make it subnormal (and lose its bits) for x below about -669.
struct ScaledWide final {
    Wide mantissa{};
    int exponent{0};
};

// x = k*ln2 + r with |r| <= ln2/2 (the k*ln2_i products formed exactly),
// e^r by Taylor series to ~1e-33. Valid for |x| <= 2^30.
inline ScaledWide exp_reference(const double x) {
    const double k = std::nearbyint(x / ln2_1);
    Wide r{x, 0.0};
    const double parts[3] = {ln2_1, ln2_2, ln2_3};
    for (const double part : parts) {
        const double p = k * part;
        const double e = std::fma(k, part, -p);
        r = add(r, Wide{-p, -e});
    }
    const ScaledWide result{exp_series(r), static_cast<int>(k)};
    return result;
}

// e^z for a double-double z, |z| <= 745 (pow's reference: e^(y * log x)).
inline ScaledWide exp_reference(const Wide z) {
    const double k = std::nearbyint(z.hi / ln2_1);
    Wide r = z;
    const double parts[3] = {ln2_1, ln2_2, ln2_3};
    for (const double part : parts) {
        const double p = k * part;
        const double e = std::fma(k, part, -p);
        r = add(r, Wide{-p, -e});
    }
    const ScaledWide result{exp_series(r), static_cast<int>(k)};
    return result;
}

// ---- log ----------------------------------------------------------------------

// a / b for a double-double divisor: two Newton corrections of the quotient.
inline Wide divide(const Wide a, const Wide b) {
    const double q1 = a.hi / b.hi;
    const Wide r1 = add(a, negate(multiply(b, q1)));
    const double q2 = r1.hi / b.hi;
    const Wide r2 = add(r1, negate(multiply(b, q2)));
    const double q3 = r2.hi / b.hi;
    const Wide head = quick_two_sum(q1, q2);
    const Wide result = add(head, Wide{q3, 0.0});
    return result;
}

// log x for a positive normal or subnormal double, as a double-double.
// y0 = std::log(x); then log x = y0 + log1p(t) with t = x * e^-y0 - 1
// (|t| ~ 1e-16), and log1p(t) = t - t^2/2 to ~1e-48.
inline Wide log_reference(const double x) {
    const double y0 = std::log(x);
    const ScaledWide e_y0 = exp_reference(y0);
    // x / e^y0 = (x * 2^-exponent) / mantissa; the scaling is exact for any
    // positive double (the exponent is at most 1075 in magnitude).
    const double scaled_x = std::ldexp(std::ldexp(x, -e_y0.exponent / 2),
                                       -(e_y0.exponent - e_y0.exponent / 2));
    const Wide ratio = divide(Wide{scaled_x, 0.0}, e_y0.mantissa);
    const Wide t = add(ratio, Wide{-1.0, 0.0});
    const Wide t_squared_half = multiply(multiply(t, t), 0.5);
    const Wide result = add(add(Wide{y0, 0.0}, t), negate(t_squared_half));
    return result;
}

// ---- hypot ----------------------------------------------------------------------

// sqrt(s) for a positive double-double: two Newton steps carried entirely in
// double-double arithmetic, each squaring the error (1e-16 -> 1e-32).
inline Wide sqrt_reference(const Wide s) {
    Wide y{std::sqrt(s.hi), 0.0};
    for (int step = 0; step < 2; ++step) {
        const Wide residual = add(s, negate(multiply(y, y)));
        const Wide correction = divide(residual, multiply(y, 2.0));
        y = add(y, correction);
    }
    return y;
}

// sqrt(x^2 + y^2) = mantissa * 2^exponent for finite x, y not both zero:
// both scaled by the same power of two (exactly, while normal) so that the
// larger magnitude lies in [1, 2), then squared and summed in double-double.
inline ScaledWide hypot_reference(const double x, const double y) {
    const double a = std::fmax(std::fabs(x), std::fabs(y));
    int exponent = 0;
    (void)std::frexp(a, &exponent);
    exponent -= 1;
    const double a_scaled = std::ldexp(std::fabs(x), -exponent);
    const double b_scaled = std::ldexp(std::fabs(y), -exponent);
    const Wide sum = add(multiply(Wide{a_scaled, 0.0}, a_scaled), multiply(Wide{b_scaled, 0.0}, b_scaled));
    const ScaledWide result{sqrt_reference(sum), exponent};
    return result;
}

// ---- atan -----------------------------------------------------------------------

// atan(u) for u >= 0 as a double-double, independently of the kernel's
// method: atan(u) = pi/2 - atan(1/u) above 1, then three halvings
// atan(u) = 2 atan(u / (1 + sqrt(1 + u^2))) bring u below tan(pi/32), and a
// Taylor series of 40 terms finishes to ~1e-33.
inline Wide atan_reference(const Wide u) {
    const Wide half_pi{half_pi_1, half_pi_2};
    if (u.hi > 1.0) {
        const Wide inverse = divide(Wide{1.0, 0.0}, u);
        const Wide result = add(half_pi, negate(atan_reference(inverse)));
        return result;
    }
    Wide v = u;
    for (int halving = 0; halving < 3; ++halving) {
        const Wide root = sqrt_reference(add(Wide{1.0, 0.0}, multiply(v, v)));
        v = divide(v, add(Wide{1.0, 0.0}, root));
    }
    const Wide v_squared = multiply(v, v);
    Wide power = v;
    Wide sum = v;
    for (int n = 1; n <= 40; ++n) {
        power = multiply(power, v_squared);
        const Wide term = divide(power, static_cast<double>(2 * n + 1));
        if (n % 2 == 1) {
            sum = add(sum, negate(term));
        } else {
            sum = add(sum, term);
        }
    }
    const Wide result = multiply(sum, 8.0);
    return result;
}

// atan2(y, x) for finite x, y not both zero, as a double-double: the angle
// of (|x|, |y|) from atan_reference of the exact ratio, then the quadrant.
inline Wide atan2_reference(const double y, const double x) {
    const Wide pi{2.0 * half_pi_1, 2.0 * half_pi_2};
    const double ax = std::fabs(x);
    const double ay = std::fabs(y);
    // The ratio as a double-double: both scaled by the larger's exponent.
    const double larger = std::fmax(ax, ay);
    int exponent = 0;
    (void)std::frexp(larger, &exponent);
    const Wide ratio = divide(Wide{std::ldexp(ay, -exponent), 0.0}, Wide{std::ldexp(ax, -exponent), 0.0});
    Wide angle{0.0, 0.0};
    if (ax == 0.0) {
        angle = Wide{half_pi_1, half_pi_2};
    } else {
        angle = atan_reference(ratio);
    }
    if (x < 0.0) angle = add(pi, negate(angle));
    if (y < 0.0) angle = negate(angle);
    return angle;
}

// asin(x) for |x| <= 1: the angle of (sqrt(1 - x^2), |x|), with the root
// from sqrt_reference of (1 - |x|)(1 + |x|), and the sign of x.
inline Wide asin_reference(const double x) {
    const Wide half_pi{half_pi_1, half_pi_2};
    const double a = std::fabs(x);
    const Wide square = multiply(add(Wide{1.0, 0.0}, Wide{-a, 0.0}), add(Wide{1.0, 0.0}, Wide{a, 0.0}));
    const Wide root = sqrt_reference(square);
    Wide angle{0.0, 0.0};
    if (root.hi == 0.0) {
        angle = half_pi;
    } else if (a <= root.hi) {
        angle = atan_reference(divide(Wide{a, 0.0}, root));
    } else {
        angle = add(half_pi, negate(atan_reference(divide(root, Wide{a, 0.0}))));
    }
    if (x < 0.0) angle = negate(angle);
    return angle;
}

// acos(x) = pi/2 - asin(x), exact in double-double.
inline Wide acos_reference(const double x) {
    const Wide result = add(Wide{half_pi_1, half_pi_2}, negate(asin_reference(x)));
    return result;
}

// ---- tan, cbrt, hyperbolic ---------------------------------------------------------

// tan x = sin x / cos x from the sin/cos reference.
inline Wide tan_reference(const double x) {
    const WideSinCos value = reference(x);
    const Wide result = divide(value.s, value.c);
    return result;
}

// cbrt x for finite nonzero x: the C++ library's cube root as a start, then
// two double-double Newton steps y -= (y^3 - x) / (3 y^2), in scaled units
// m = |x| * 2^-3k so every intermediate stays normal.
inline ScaledWide cbrt_reference(const double x) {
    int exponent = 0;
    (void)std::frexp(std::fabs(x), &exponent);
    const int third = (exponent >= 0 ? exponent : exponent - 2) / 3;
    const double m = std::ldexp(std::fabs(x), -3 * third);
    Wide y{std::cbrt(m), 0.0};
    for (int step = 0; step < 2; ++step) {
        const Wide cube = multiply(multiply(y, y), y);
        const Wide residual = add(cube, Wide{-m, 0.0});
        const Wide correction = divide(residual, multiply(multiply(y, y), 3.0));
        y = add(y, negate(correction));
    }
    if (x < 0.0) y = negate(y);
    const ScaledWide result{y, third};
    return result;
}

// e^a for |a| <= 700 as a plain double-double (normal throughout).
inline Wide exp_value(const double a) {
    const ScaledWide e = exp_reference(a);
    const Wide result{std::ldexp(e.mantissa.hi, e.exponent), std::ldexp(e.mantissa.lo, e.exponent)};
    return result;
}

// sinh, cosh, tanh for |x| <= 700 from (e^x -+ e^-x) / 2 in double-double.
// sinh cancels for small x: its relative error is ~1e-32 / |x|, which stays
// below 1e-23 for the |x| >= 2^-27 the tests use.
inline Wide sinh_reference(const double x) {
    const Wide e = exp_value(x);
    const Wide inverse = divide(Wide{1.0, 0.0}, e);
    const Wide result = multiply(add(e, negate(inverse)), 0.5);
    return result;
}

inline Wide cosh_reference(const double x) {
    const Wide e = exp_value(x);
    const Wide inverse = divide(Wide{1.0, 0.0}, e);
    const Wide result = multiply(add(e, inverse), 0.5);
    return result;
}

inline Wide tanh_reference(const double x) {
    const Wide result = divide(sinh_reference(x), cosh_reference(x));
    return result;
}

// log(v) for a double-double v >= 1: log_reference(v.hi) + v.lo / v.hi.
inline Wide log_of(const Wide v) {
    const Wide normal = two_sum(v.hi, v.lo);
    const Wide result = add(log_reference(normal.hi), Wide{normal.lo / normal.hi, 0.0});
    return result;
}

inline Wide asinh_reference(const double x) {
    const double a = std::fabs(x);
    const Wide root = sqrt_reference(add(multiply(Wide{a, 0.0}, a), Wide{1.0, 0.0}));
    Wide result = log_of(add(Wide{a, 0.0}, root));
    if (x < 0.0) result = negate(result);
    return result;
}

inline Wide acosh_reference(const double x) {
    const Wide square = multiply(add(Wide{x, 0.0}, Wide{-1.0, 0.0}), add(Wide{x, 0.0}, Wide{1.0, 0.0}));
    const Wide result = log_of(add(Wide{x, 0.0}, sqrt_reference(square)));
    return result;
}

inline Wide atanh_reference(const double x) {
    const double a = std::fabs(x);
    const Wide ratio = divide(add(Wide{1.0, 0.0}, Wide{a, 0.0}), add(Wide{1.0, 0.0}, Wide{-a, 0.0}));
    Wide result = multiply(log_of(ratio), 0.5);
    if (x < 0.0) result = negate(result);
    return result;
}

} // namespace wide
