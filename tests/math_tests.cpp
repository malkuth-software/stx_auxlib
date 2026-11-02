// Accuracy, edge-case and determinism tests for stx_auxlib::math, measured
// against the double-double references in tests/reference/wide.hpp.
//
//   math_tests               sampled checks (the ctest run)
//   math_tests --exhaustive  every float on each kernel's fast path and 20
//                            million doubles per region
//   math_tests --bench       ns per call against the C++ library
//   math_tests --bench-gate  the same, failing above 2x the C++ library on
//                            any gated region (the owner's ceiling)
//
// The determinism check hashes outputs over fixed inputs and compares the
// hash with a frozen value: a different architecture, compiler or fma use
// that changed any bit fails it.

#include "stx_auxlib/math.hpp"

#include "reference/sincos_worst_cases.hpp"
#include "reference/wide.hpp"
#include "../src/checked.hpp"
// The certified worst-case test calls the reduction directly. src/math/
// compiles once per kernel variant (src/math/variant.hpp); this test compiles
// its own copy as the native variant, which every variant matches bit for bit.
// The Dekker test target defines the variant already.
#if !defined(STX_AUXLIB_MATH_VARIANT)
#define STX_AUXLIB_MATH_VARIANT native
#endif
#include "../src/math/sincos_core.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {

void require(const bool condition, const char* const message) {
    if (!condition) throw std::runtime_error(message);
}

// Deterministic inputs: a 64-bit LCG.
struct Lcg final {
    std::uint64_t state{0};

    // A 64-bit linear congruential step: arithmetic modulo 2^64 by design.
    STX_AUXLIB_MODULAR double unit() {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        const double result = static_cast<double>(state >> 11) * 0x1.0p-53;
        return result;
    }
};

// |approx - exact| in ulps of the correctly rounded double.
double double_ulps(const double approx, const wide::Wide exact) {
    const double rounded = std::fabs(exact.hi + exact.lo);
    const double ulp = std::nextafter(rounded, std::numeric_limits<double>::infinity()) - rounded;
    const wide::Wide difference = wide::add(wide::Wide{approx, 0.0}, wide::negate(exact));
    const double result = std::fabs(difference.hi + difference.lo) / ulp;
    return result;
}

// sin and cos of a double: the stated bound, proved for every double
// 2^-27 <= |x| <= 2^32 by tools/sincos_certificate.py (the rotation's own
// error is at most 0.0011 ulp; the reduction is held to the rest). Measured
// worst over the sampled regions: 0.500415 ulp.
constexpr double double_ulp_limit = sincos_worst_cases::stated_bound_ulps;
// exp: measured 0.500001 ulp over 100 million inputs.
constexpr double exp_ulp_limit = 0.5005;
// log, log2, log10: measured 0.500000 ulp over 24 million inputs.
constexpr double log_ulp_limit = 0.5005;
// pow: measured 0.500000 ulp over 5 million inputs.
constexpr double pow_ulp_limit = 0.5005;
// hypot: measured 0.500000 ulp over 10 million inputs (none misrounded).
constexpr double hypot_ulp_limit = 0.5005;
// atan, atan2, asin, acos: measured 0.500000 ulp over 4 million inputs each.
constexpr double inverse_trig_ulp_limit = 0.5005;
// tan: the stated bound, proved like sincos' (the rotation and quotient
// together at most 0.0054 ulp). Measured worst over the sampled regions:
// 0.500154 ulp. cbrt and the hyperbolic family (remaining_ulp_limit):
// measured 0.500000 ulp, sampled only.
constexpr double tan_ulp_limit = sincos_worst_cases::stated_bound_ulps;
constexpr double remaining_ulp_limit = 0.5005;

// hi + lo rounded once to float.
float correctly_rounded_float(const wide::Wide exact) {
    const float rounded = static_cast<float>(exact.hi);
    const double error = (exact.hi - static_cast<double>(rounded)) + exact.lo;
    const float up = std::nextafter(rounded, std::numeric_limits<float>::infinity());
    const float down = std::nextafter(rounded, -std::numeric_limits<float>::infinity());
    const double to_up = static_cast<double>(up) - static_cast<double>(rounded);
    const double to_down = static_cast<double>(rounded) - static_cast<double>(down);
    if (error > to_up / 2.0) return up;
    if (-error > to_down / 2.0) return down;
    return rounded;
}

void check_reference() {
    Lcg lcg{7};
    double worst = 0.0;
    for (int i = 0; i < 200000; ++i) {
        const double x = (2.0 * lcg.unit() - 1.0) * 1000.0;
        const wide::WideSinCos r = wide::reference(x);
        const wide::Wide one = wide::add(wide::multiply(r.s, r.s), wide::multiply(r.c, r.c));
        const wide::Wide deviation = wide::add(one, wide::Wide{-1.0, 0.0});
        worst = std::max(worst, std::fabs(deviation.hi + deviation.lo));
    }
    require(worst < 1e-30, "reference: sin^2 + cos^2 departs from 1");
    const double anchors[] = {0.5, 1.0, 2.0, 3.0, 1e-8, 100.0, 710.0};
    for (const double x : anchors) {
        const wide::WideSinCos r = wide::reference(x);
        require(double_ulps(std::sin(x), r.s) < 1.0 && double_ulps(std::cos(x), r.c) < 1.0,
                "reference: disagrees with the C++ library by an ulp or more");
    }
}

bool same_bits(const double a, const double b) {
    const bool both_nan = std::isnan(a) && std::isnan(b);
    const bool result = both_nan || std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
    return result;
}

bool same_bits(const float a, const float b) {
    const bool both_nan = std::isnan(a) && std::isnan(b);
    const bool result = both_nan || std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
    return result;
}

// Inputs off the computed range must return exactly what the stated rule
// says: x and 1 below 2^-27 (keeping the sign of zero), NaN above 2^32 and
// for infinities and NaN. Edges of the computed range must meet the bound.
void special_values() {
    const double tiny[] = {0.0, -0.0, 0x1p-1074, -0x1p-1030, 1e-300, 0x1.fffffffffffffp-28};
    for (const double x : tiny) {
        double s = 0.0;
        double c = 0.0;
        stx_auxlib::math::sincos(x, s, c);
        require(same_bits(s, x) && same_bits(c, 1.0), "sincos(double): tiny input is not (x, 1)");
        const float xf = static_cast<float>(x);
        float sf = 0.0f;
        float cf = 0.0f;
        stx_auxlib::math::sincos(xf, sf, cf);
        require(same_bits(sf, xf) && same_bits(cf, 1.0f), "sincos(float): tiny input is not (x, 1)");
    }
    const double refused[] = {0x1.0000000000001p32, -0x1.0000000000001p32, 1e300,
                              std::numeric_limits<double>::infinity(),
                              -std::numeric_limits<double>::infinity(),
                              std::numeric_limits<double>::quiet_NaN()};
    for (const double x : refused) {
        double s = 0.0;
        double c = 0.0;
        stx_auxlib::math::sincos(x, s, c);
        require(std::isnan(s) && std::isnan(c) && std::isnan(stx_auxlib::math::tan(x)),
                "sincos/tan(double): an angle above 2^32 was not refused");
        const float xf = static_cast<float>(x);
        if (std::isfinite(xf) && std::fabs(xf) <= 0x1p32f) continue;
        float sf = 0.0f;
        float cf = 0.0f;
        stx_auxlib::math::sincos(xf, sf, cf);
        require(std::isnan(sf) && std::isnan(cf) && std::isnan(stx_auxlib::math::tan(xf)),
                "sincos/tan(float): an angle above 2^32 was not refused");
    }
    const double edges[] = {0x1p-27, -0x1p-27, 0x1p20, -0x1p20, 0x1.fffffffffffffp19,
                            0x1.0000000000001p20, 0x1p32, -0x1p32};
    for (const double x : edges) {
        double s = 0.0;
        double c = 0.0;
        stx_auxlib::math::sincos(x, s, c);
        const wide::WideSinCos exact = wide::reference(x);
        require(std::max(double_ulps(s, exact.s), double_ulps(c, exact.c)) <= double_ulp_limit,
                "sincos(double): edge above the stated bound");
    }
}

struct Region final {
    const char* name{nullptr};
    double low{0.0};
    double high{0.0};
    // Whether --bench-gate holds this region to the 2x ceiling. Edge regions
    // where a C library has a dedicated shortcut (exp next to its overflow
    // and underflow thresholds) or that only owned code serves (sincos
    // beyond 2^20) are reported but not gated.
    bool gated{true};
};

const Region double_regions[] = {
    {"|x| < 2pi", -6.283185307179586, 6.283185307179586},
    {"|x| < 1000", -1000.0, 1000.0},
    {"|x| < 2^20", -1048576.0, 1048576.0},
    {"[0, 0.1)", 0.0, 0.1},
    {"pi/2 +- 0.05", 1.5207963267948966, 1.6207963267948966},
    {"pi +- 0.05", 3.0915926535897931, 3.1915926535897931},
    {"2^20..2^32", 1048576.0, 4294967296.0, false},
};

// The double nearest k * pi/128 for random k up to 2^38: the remainder is
// then tiny, the hardest case for the medium-range reduction.
void sincos_hard_reduction(const int count) {
    Lcg lcg{2038};
    double worst = 0.0;
    double worst_x = 0.0;
    for (int i = 0; i < count; ++i) {
        const double k = std::floor(lcg.unit() * 0x1p38);
        const wide::Wide step = wide::multiply(wide::Wide{wide::half_pi_1, wide::half_pi_2}, 1.0 / 64.0);
        const wide::Wide nearest = wide::multiply(step, k);
        const double x = nearest.hi + nearest.lo;
        if (std::fabs(x) > 0x1p32) continue;
        double s = 0.0;
        double c = 0.0;
        stx_auxlib::math::sincos(x, s, c);
        const wide::WideSinCos exact = wide::reference(x);
        const double error = std::max(double_ulps(s, exact.s), double_ulps(c, exact.c));
        if (error > worst) {
            worst = error;
            worst_x = x;
        }
        require(double_ulps(stx_auxlib::math::tan(x), wide::tan_reference(x)) <= tan_ulp_limit,
                "tan(double): hard reduction case above the stated bound");
    }
    std::printf("sincos(double) near k*pi/128 %10d inputs  max %.6f ulp at %a\n", count, worst, worst_x);
    require(worst <= double_ulp_limit, "sincos(double): hard reduction case above the stated bound");
}

// The certified worst cases (tests/reference/sincos_worst_cases.hpp, from
// tools/sincos_certificate.py): for every binade and residue class, the
// double nearest a multiple of pi/128. At each, and at its negation, the
// reduction must deliver r + r_lo within the certificate's contract of the
// exact remainder, and sin, cos and tan must be within the stated bound of
// the exact values the certificate computed (not of the reference).
void sincos_certified_worst_cases() {
    namespace detail = stx_auxlib::math::STX_AUXLIB_MATH_VARIANT::detail;
    int checked = 0;
    for (const sincos_worst_cases::Case& entry : sincos_worst_cases::cases) {
        for (int sign = 0; sign < 2; ++sign) {
            const double flip = sign == 0 ? 1.0 : -1.0;
            const double x = flip * entry.x;
            std::uint64_t index = 0;
            double r = 0.0;
            double r_lo = 0.0;
            double q = 0.0;
            if (detail::sincos_fast_path(x)) {
                q = detail::sincos_reduce_fast(x, index, r, r_lo);
            } else {
                q = detail::sincos_step(x, index);
                detail::sincos_remainder_medium(x, q, r, r_lo);
            }
            require(q == flip * entry.q, "sincos worst case: the reduction chose another multiple");
            const wide::Wide exact_remainder{flip * entry.remainder_hi, flip * entry.remainder_lo};
            const wide::Wide difference = wide::add(wide::two_sum(r, r_lo), wide::negate(exact_remainder));
            const double reduction_error = std::fabs(difference.hi + difference.lo);
            const double allowed = std::min(sincos_worst_cases::contract_kappa * std::fabs(entry.remainder_hi),
                                            sincos_worst_cases::contract_general);
            if (reduction_error > allowed) {
                std::fprintf(stderr, "x = %a: reduction error %a above the contract %a\n", x, reduction_error,
                             allowed);
            }
            require(reduction_error <= allowed, "sincos worst case: reduction outside the certified contract");
            double s = 0.0;
            double c = 0.0;
            stx_auxlib::math::sincos(x, s, c);
            const double t = stx_auxlib::math::tan(x);
            const double sin_error = double_ulps(s, wide::Wide{flip * entry.sin_hi, flip * entry.sin_lo});
            const double cos_error = double_ulps(c, wide::Wide{entry.cos_hi, entry.cos_lo});
            const double tan_error = double_ulps(t, wide::Wide{flip * entry.tan_hi, flip * entry.tan_lo});
            if (sin_error > double_ulp_limit || cos_error > double_ulp_limit || tan_error > tan_ulp_limit) {
                std::fprintf(stderr, "x = %a: sin %.6f cos %.6f tan %.6f ulp\n", x, sin_error, cos_error, tan_error);
            }
            require(sin_error <= double_ulp_limit && cos_error <= double_ulp_limit,
                    "sincos worst case: above the stated bound");
            require(tan_error <= tan_ulp_limit, "tan worst case: above the stated bound");
            checked += 1;
        }
    }
    std::printf("sincos(double), tan(double): %d certified worst cases within %.2f ulp\n", checked,
                double_ulp_limit);
}

void double_accuracy(const int per_region) {
    Lcg lcg{2026};
    for (const Region& region : double_regions) {
        double worst = 0.0;
        double worst_x = 0.0;
        std::uint64_t misrounded = 0;
        for (int i = 0; i < per_region; ++i) {
            const double x = region.low + (region.high - region.low) * lcg.unit();
            double s = 0.0;
            double c = 0.0;
            stx_auxlib::math::sincos(x, s, c);
            const wide::WideSinCos exact = wide::reference(x);
            const double error = std::max(double_ulps(s, exact.s), double_ulps(c, exact.c));
            if (error > 0.5) misrounded += 1;
            if (error > worst) {
                worst = error;
                worst_x = x;
            }
        }
        std::printf("sincos(double) %-14s %10d inputs  max %.6f ulp at %a  %llu above 0.5 ulp\n",
                    region.name, per_region, worst, worst_x,
                    static_cast<unsigned long long>(misrounded));
        require(worst <= double_ulp_limit, "sincos(double): error above the stated bound");
    }
}

// Every `stride`-th float in [2^-27, 2^32], positive and negated: the result
// must be the correctly rounded sin and cos. The double libm result rounded
// to float screens; the reference decides where the screen disagrees.
void float_rounding(const std::uint32_t stride) {
    const std::uint32_t first = std::bit_cast<std::uint32_t>(0x1p-27f);
    const std::uint32_t last = std::bit_cast<std::uint32_t>(0x1p32f);
    std::uint64_t checked = 0;
    for (std::uint32_t bits = first; bits <= last; bits += stride) {
        const float magnitude = std::bit_cast<float>(bits);
        const float signs[2] = {magnitude, -magnitude};
        for (const float x : signs) {
            float s = 0.0f;
            float c = 0.0f;
            stx_auxlib::math::sincos(x, s, c);
            const double xd = static_cast<double>(x);
            const float screen_s = static_cast<float>(std::sin(xd));
            const float screen_c = static_cast<float>(std::cos(xd));
            checked += 1;
            if (s == screen_s && c == screen_c) continue;
            const wide::WideSinCos exact = wide::reference(xd);
            require(s == correctly_rounded_float(exact.s) && c == correctly_rounded_float(exact.c),
                    "sincos(float): result is not correctly rounded");
        }
    }
    std::printf("sincos(float) correctly rounded at %llu floats (stride %u)\n",
                static_cast<unsigned long long>(checked), stride);
}

// FNV-1a over double and float outputs at fixed inputs, all on the fast or
// tiny paths (no C++ library fallback, whose bits are the host's).
// FNV-1a: multiplication modulo 2^64 by design.
STX_AUXLIB_MODULAR std::uint64_t output_hash() {
    std::uint64_t hash = 1469598103934665603ULL;
    Lcg lcg{99};
    const double ranges[] = {6.283185307179586, 100.0, 1e6, 0.1};
    for (const double range : ranges) {
        for (int i = 0; i < 4000000; ++i) {
            const double x = (2.0 * lcg.unit() - 1.0) * range;
            double s = 0.0;
            double c = 0.0;
            stx_auxlib::math::sincos(x, s, c);
            float sf = 0.0f;
            float cf = 0.0f;
            stx_auxlib::math::sincos(static_cast<float>(x), sf, cf);
            const std::uint64_t words[4] = {std::bit_cast<std::uint64_t>(s),
                                            std::bit_cast<std::uint64_t>(c),
                                            std::bit_cast<std::uint32_t>(sf),
                                            std::bit_cast<std::uint32_t>(cf)};
            for (const std::uint64_t word : words) {
                hash ^= word;
                hash *= 1099511628211ULL;
            }
        }
    }
    return hash;
}

// Frozen on 2026-10-31 (Apple arm64, fma and Dekker builds, -O0 and -O2).
constexpr std::uint64_t expected_output_hash = 0xde89d2d2ed037d01ULL;

void determinism() {
    const std::uint64_t hash = output_hash();
    std::printf("output hash %016llx\n", static_cast<unsigned long long>(hash));
    require(hash == expected_output_hash, "sincos: outputs differ from the frozen bits");
}

// ---- exp ----------------------------------------------------------------------

// |approx - exact| in ulps of the correctly rounded double, for a normal
// approx, compared at the reference's scale (scaling by 2^-exponent is exact).
double exp_ulps(const double approx, const wide::ScaledWide exact) {
    const double scaled = std::ldexp(approx, -exact.exponent);
    const double rounded = std::fabs(exact.mantissa.hi + exact.mantissa.lo);
    const double ulp = std::nextafter(rounded, std::numeric_limits<double>::infinity()) - rounded;
    const wide::Wide difference = wide::add(wide::Wide{scaled, 0.0}, wide::negate(exact.mantissa));
    const double result = std::fabs(difference.hi + difference.lo) / ulp;
    return result;
}

// mantissa * 2^exponent rounded once to float; valid while hi and lo stay
// normal doubles after scaling (|x| <= 110 here).
float exp_correctly_rounded_float(const wide::ScaledWide exact) {
    const wide::Wide value{std::ldexp(exact.mantissa.hi, exact.exponent),
                           std::ldexp(exact.mantissa.lo, exact.exponent)};
    const float result = correctly_rounded_float(value);
    return result;
}

void exp_check_reference() {
    const wide::ScaledWide one = wide::exp_reference(1.0);
    const wide::Wide e_value{std::ldexp(one.mantissa.hi, one.exponent),
                             std::ldexp(one.mantissa.lo, one.exponent)};
    const wide::Wide difference = wide::add(e_value, wide::negate(wide::e_constant));
    require(std::fabs(difference.hi + difference.lo) < 1e-31,
            "exp reference: e^1 departs from the independent constant for e");
    // e^n against e multiplied n times: catches an error in the ln 2 parts,
    // which grows with k.
    wide::Wide power{1.0, 0.0};
    for (int n = 1; n <= 600; ++n) {
        power = wide::multiply(power, wide::e_constant);
        const wide::ScaledWide exact = wide::exp_reference(static_cast<double>(n));
        const wide::Wide scaled{std::ldexp(exact.mantissa.hi, exact.exponent),
                                std::ldexp(exact.mantissa.lo, exact.exponent)};
        const wide::Wide gap = wide::add(scaled, wide::negate(power));
        require(std::fabs(gap.hi + gap.lo) / power.hi < 1e-29,
                "exp reference: e^n departs from e multiplied n times");
    }
}

// e^x rounded once onto the subnormal grid (multiples of 2^-1074), for
// results below 2^-1022: mantissa * 2^exponent in units of 2^-1074 is a
// normal double-double, rounded here to the nearest integer.
double exp_correctly_rounded_subnormal(const wide::ScaledWide exact) {
    const int shift = exact.exponent + 1074;
    const double units = std::ldexp(exact.mantissa.hi, shift);
    const double units_lo = std::ldexp(exact.mantissa.lo, shift);
    double nearest = std::nearbyint(units);
    const double remainder = (units - nearest) + units_lo;
    if (remainder > 0.5) nearest += 1.0;
    if (remainder < -0.5) nearest -= 1.0;
    const double result = std::ldexp(nearest, -1074);
    return result;
}

// exp owns every input: NaN, infinities, overflow, the subnormal range and
// underflow to zero follow the stated rules, and e^0 is exactly 1.
void exp_special_values() {
    const double infinity = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    require(std::isnan(stx_auxlib::math::exp(nan)) &&
                std::isnan(stx_auxlib::math::exp(static_cast<float>(nan))),
            "exp: NaN did not return NaN");
    require(same_bits(stx_auxlib::math::exp(infinity), infinity) &&
                same_bits(stx_auxlib::math::exp(-infinity), 0.0),
            "exp(double): infinities do not give +infinity and +0");
    require(same_bits(stx_auxlib::math::exp(static_cast<float>(infinity)),
                      std::numeric_limits<float>::infinity()) &&
                same_bits(stx_auxlib::math::exp(static_cast<float>(-infinity)), 0.0f),
            "exp(float): infinities do not give +infinity and +0");
    const double overflowing[] = {709.7827128933841, 709.79, 710.0, 1e300};
    for (const double x : overflowing) {
        require(same_bits(stx_auxlib::math::exp(x), infinity), "exp(double): did not overflow");
    }
    require(std::isfinite(stx_auxlib::math::exp(709.782712893384)),
            "exp(double): overflowed below ln(DBL_MAX)");
    const double vanishing[] = {-745.1332191019412, -746.0, -800.0, -1e300};
    for (const double x : vanishing) {
        require(same_bits(stx_auxlib::math::exp(x), 0.0), "exp(double): did not underflow to +0");
    }
    require(stx_auxlib::math::exp(-745.13321910194) == 0x1p-1074,
            "exp(double): the smallest subnormal result is wrong");
    require(same_bits(stx_auxlib::math::exp(0.0), 1.0) && same_bits(stx_auxlib::math::exp(-0.0), 1.0),
            "exp(double): e^0 is not exactly 1");
    require(same_bits(stx_auxlib::math::exp(0.0f), 1.0f), "exp(float): e^0 is not exactly 1");
    require(std::isinf(stx_auxlib::math::exp(89.0f)), "exp(float): 89 did not overflow");
    require(stx_auxlib::math::exp(-104.0f) == 0.0f, "exp(float): -104 did not underflow");
    const double edges[] = {708.0, -708.0, 1e-300, -1e-300, 0x1p-54, -0x1p-54, 709.78, -708.39};
    for (const double x : edges) {
        require(exp_ulps(stx_auxlib::math::exp(x), wide::exp_reference(x)) <= exp_ulp_limit,
                "exp(double): edge above the stated bound");
    }
}

// The ranges beyond the fast path: (708, ln(DBL_MAX)] against the bound,
// and the subnormal results in [ln(2^-1075), ln(2^-1022)) against the
// correctly rounded subnormal.
void exp_double_outside(const int count) {
    Lcg lcg{2028};
    double worst = 0.0;
    for (int i = 0; i < count; ++i) {
        const double x = 708.0 + (709.7827128933840 - 708.0) * lcg.unit();
        worst = std::max(worst, exp_ulps(stx_auxlib::math::exp(x), wide::exp_reference(x)));
    }
    std::printf("exp(double) (708, 709.78]   %10d inputs  max %.6f ulp\n", count, worst);
    require(worst <= exp_ulp_limit, "exp(double): error above the bound near overflow");
    std::uint64_t misrounded = 0;
    for (int i = 0; i < count; ++i) {
        const double x = -745.1332191019412 + (745.1332191019412 - 708.3964185322641) * lcg.unit();
        const double y = stx_auxlib::math::exp(x);
        const double expected = exp_correctly_rounded_subnormal(wide::exp_reference(x));
        if (y != expected) misrounded += 1;
        require(std::fabs(y - expected) <= 0x1p-1074, "exp(double): subnormal off by more than an ulp");
    }
    std::printf("exp(double) subnormal range  %10d inputs  %llu not correctly rounded\n", count,
                static_cast<unsigned long long>(misrounded));
    require(misrounded * 1000000 <= static_cast<std::uint64_t>(count),
            "exp(double): subnormal results misrounded more than one in a million");
}

const Region exp_regions[] = {
    {"|x| <= 708", -708.0, 708.0},
    {"|x| <= 1", -1.0, 1.0},
    {"|x| <= 1e-3", -1e-3, 1e-3},
    {"[700, 708]", 700.0, 708.0, false},
    {"[-708, -700]", -708.0, -700.0, false},
};

void exp_double_accuracy(const int per_region) {
    Lcg lcg{2027};
    for (const Region& region : exp_regions) {
        double worst = 0.0;
        double worst_x = 0.0;
        std::uint64_t misrounded = 0;
        for (int i = 0; i < per_region; ++i) {
            const double x = region.low + (region.high - region.low) * lcg.unit();
            const double error = exp_ulps(stx_auxlib::math::exp(x), wide::exp_reference(x));
            if (error > 0.5) misrounded += 1;
            if (error > worst) {
                worst = error;
                worst_x = x;
            }
        }
        std::printf("exp(double) %-14s %10d inputs  max %.6f ulp at %a  %llu above 0.5 ulp\n",
                    region.name, per_region, worst, worst_x,
                    static_cast<unsigned long long>(misrounded));
        require(worst <= exp_ulp_limit, "exp(double): error above the stated bound");
    }
}

// Every `stride`-th finite float, both signs: the result must be the
// correctly rounded e^x (infinity, subnormals and zero included). Screened
// by the double libm result rounded to float; the reference decides where
// they disagree.
void exp_float_rounding(const std::uint32_t stride) {
    const std::uint32_t last = std::bit_cast<std::uint32_t>(std::numeric_limits<float>::max());
    std::uint64_t checked = 0;
    for (std::uint64_t wide_bits = 0; wide_bits <= last; wide_bits += stride) {
        const float magnitude = std::bit_cast<float>(static_cast<std::uint32_t>(wide_bits));
        const float signs[2] = {magnitude, -magnitude};
        for (const float x : signs) {
            const float y = stx_auxlib::math::exp(x);
            const double xd = static_cast<double>(x);
            const float screen = static_cast<float>(std::exp(xd));
            checked += 1;
            if (y == screen) continue;
            // Beyond these, float results are infinity or zero with margin.
            require(xd >= -110.0 && xd <= 90.0, "exp(float): wrong overflow or underflow");
            require(y == exp_correctly_rounded_float(wide::exp_reference(xd)),
                    "exp(float): result is not correctly rounded");
        }
    }
    std::printf("exp(float) correctly rounded at %llu floats (stride %u)\n",
                static_cast<unsigned long long>(checked), stride);
}

// FNV-1a: multiplication modulo 2^64 by design.
STX_AUXLIB_MODULAR std::uint64_t exp_output_hash() {
    std::uint64_t hash = 1469598103934665603ULL;
    Lcg lcg{98};
    // 760 covers overflow, the subnormal range and underflow: exp owns every
    // input, so no C++ library result enters the hash.
    const double ranges[] = {708.0, 50.0, 1.0, 1e-3, 760.0};
    for (const double range : ranges) {
        for (int i = 0; i < 4000000; ++i) {
            const double x = (2.0 * lcg.unit() - 1.0) * range;
            const double y = stx_auxlib::math::exp(x);
            const float yf = stx_auxlib::math::exp(static_cast<float>(x));
            const std::uint64_t words[2] = {std::bit_cast<std::uint64_t>(y),
                                            std::bit_cast<std::uint32_t>(yf)};
            for (const std::uint64_t word : words) {
                hash ^= word;
                hash *= 1099511628211ULL;
            }
        }
    }
    return hash;
}

// Frozen on 2026-10-31 (Apple arm64, fma and Dekker builds, -O0 and -O2).
constexpr std::uint64_t expected_exp_output_hash = 0xbad54eddef5ab9feULL;

void exp_determinism() {
    const std::uint64_t hash = exp_output_hash();
    std::printf("exp output hash %016llx\n", static_cast<unsigned long long>(hash));
    require(hash == expected_exp_output_hash, "exp: outputs differ from the frozen bits");
}

// ---- log, log2, log10 -----------------------------------------------------------

// One logarithm under test: the function pair and its base's 1/ln b as a
// double-double (1 for ln), which turns log_reference into this base.
using LogDouble = double (*)(double x);
using LogFloat = float (*)(float x);

struct LogKernel final {
    const char* name{nullptr};
    LogDouble owned_double{nullptr};
    LogFloat owned_float{nullptr};
    LogDouble library_double{nullptr};
    wide::Wide inverse{1.0, 0.0};
};

double owned_log_d(const double x) { return stx_auxlib::math::log(x); }
float owned_log_f(const float x) { return stx_auxlib::math::log(x); }
double owned_log2_d(const double x) { return stx_auxlib::math::log2(x); }
float owned_log2_f(const float x) { return stx_auxlib::math::log2(x); }
double owned_log10_d(const double x) { return stx_auxlib::math::log10(x); }
float owned_log10_f(const float x) { return stx_auxlib::math::log10(x); }
double library_log_d(const double x) { return std::log(x); }
double library_log2_d(const double x) { return std::log2(x); }
double library_log10_d(const double x) { return std::log10(x); }

std::vector<LogKernel> log_kernels() {
    const wide::Wide ln2{wide::ln2_1, wide::ln2_2};
    const wide::Wide inv_ln2 = wide::divide(wide::Wide{1.0, 0.0}, ln2);
    const wide::Wide inv_ln10 = wide::divide(wide::Wide{1.0, 0.0}, wide::log_reference(10.0));
    std::vector<LogKernel> kernels{};
    kernels.push_back(LogKernel{"log", owned_log_d, owned_log_f, library_log_d, wide::Wide{1.0, 0.0}});
    kernels.push_back(LogKernel{"log2", owned_log2_d, owned_log2_f, library_log2_d, inv_ln2});
    kernels.push_back(LogKernel{"log10", owned_log10_d, owned_log10_f, library_log10_d, inv_ln10});
    return kernels;
}

wide::Wide log_exact(const LogKernel& kernel, const double x) {
    const wide::Wide natural = wide::log_reference(x);
    const wide::Wide result = wide::multiply(natural, kernel.inverse);
    return result;
}

// Logarithms own every input; the identities below are exact.
void log_special_values(const std::vector<LogKernel>& kernels) {
    const double infinity = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    for (const LogKernel& kernel : kernels) {
        require(same_bits((*kernel.owned_double)(0.0), -infinity) &&
                    same_bits((*kernel.owned_double)(-0.0), -infinity),
                "log(double): zero does not give -infinity");
        require(std::isnan((*kernel.owned_double)(-1.0)) &&
                    std::isnan((*kernel.owned_double)(-infinity)) &&
                    std::isnan((*kernel.owned_double)(nan)) &&
                    std::isnan((*kernel.owned_double)(-0x1p-1074)),
                "log(double): negative or NaN input does not give NaN");
        require(same_bits((*kernel.owned_double)(infinity), infinity),
                "log(double): +infinity does not give +infinity");
        require(same_bits((*kernel.owned_double)(1.0), 0.0) &&
                    same_bits((*kernel.owned_float)(1.0f), 0.0f),
                "log: log(1) is not +0");
        require(same_bits((*kernel.owned_float)(0.0f), -std::numeric_limits<float>::infinity()) &&
                    std::isnan((*kernel.owned_float)(-1.0f)) &&
                    same_bits((*kernel.owned_float)(std::numeric_limits<float>::infinity()),
                              std::numeric_limits<float>::infinity()),
                "log(float): special input broken");
        const double edges[] = {0x1p-1074, 0x1p-1022, 0x1.fffffffffffffp-1023, 0x1.fffffffffffffp1023,
                                0.5, 2.0, 0x1.0000000000001p0, 0x1.fffffffffffffp-1};
        for (const double x : edges) {
            const double error = double_ulps((*kernel.owned_double)(x), log_exact(kernel, x));
            require(error <= log_ulp_limit, "log(double): edge above the stated bound");
        }
    }
    for (int k = -1074; k <= 1023; ++k) {
        require(stx_auxlib::math::log2(std::ldexp(1.0, k)) == static_cast<double>(k),
                "log2(double): log2(2^k) is not exactly k");
    }
    for (int k = -149; k <= 127; ++k) {
        require(stx_auxlib::math::log2(std::ldexp(1.0f, k)) == static_cast<float>(k),
                "log2(float): log2(2^k) is not exactly k");
    }
    double power = 1.0;
    for (int n = 0; n <= 22; ++n) {
        require(stx_auxlib::math::log10(power) == static_cast<double>(n),
                "log10(double): log10(10^n) is not exactly n");
        power *= 10.0;
    }
    float power_float = 1.0f;
    for (int n = 0; n <= 10; ++n) {
        require(stx_auxlib::math::log10(power_float) == static_cast<float>(n),
                "log10(float): log10(10^n) is not exactly n");
        power_float *= 10.0f;
    }
}

// Inputs: random bit patterns over every exponent, [0.5, 2], 1 +- 1e-3 and
// subnormals.
double log_input(const int region, Lcg& lcg) {
    if (region == 0) {
        const double exponent = std::floor(lcg.unit() * 2046.0) - 1022.0;
        return std::ldexp(1.0 + lcg.unit(), static_cast<int>(exponent));
    }
    if (region == 1) return 0.5 + 1.5 * lcg.unit();
    if (region == 2) return 1.0 + (lcg.unit() - 0.5) * 2e-3;
    const double pattern = std::floor(lcg.unit() * 0x1p52) + 1.0;
    return std::ldexp(pattern, -1074);
}

const char* const log_region_names[] = {"all exponents", "[0.5, 2]", "1 +- 1e-3", "subnormal"};

void log_double_accuracy(const std::vector<LogKernel>& kernels, const int per_region) {
    for (const LogKernel& kernel : kernels) {
        Lcg lcg{2029};
        for (int region = 0; region < 4; ++region) {
            double worst = 0.0;
            double worst_x = 0.0;
            std::uint64_t misrounded = 0;
            for (int i = 0; i < per_region; ++i) {
                const double x = log_input(region, lcg);
                const double error = double_ulps((*kernel.owned_double)(x), log_exact(kernel, x));
                if (error > 0.5) misrounded += 1;
                if (error > worst) {
                    worst = error;
                    worst_x = x;
                }
            }
            std::printf("%s(double) %-14s %10d inputs  max %.6f ulp at %a  %llu above 0.5 ulp\n",
                        kernel.name, log_region_names[region], per_region, worst, worst_x,
                        static_cast<unsigned long long>(misrounded));
            require(worst <= log_ulp_limit, "log(double): error above the stated bound");
        }
    }
}

// Every `stride`-th positive finite float: the result must be the correctly
// rounded logarithm. Screened by the double libm result rounded to float.
void log_float_rounding(const std::vector<LogKernel>& kernels, const std::uint32_t stride) {
    const std::uint32_t last = std::bit_cast<std::uint32_t>(std::numeric_limits<float>::max());
    for (const LogKernel& kernel : kernels) {
        std::uint64_t checked = 0;
        for (std::uint64_t pattern = 1; pattern <= last; pattern += stride) {
            const float x = std::bit_cast<float>(static_cast<std::uint32_t>(pattern));
            const float y = (*kernel.owned_float)(x);
            const double xd = static_cast<double>(x);
            const float screen = static_cast<float>((*kernel.library_double)(xd));
            checked += 1;
            if (y == screen) continue;
            require(y == correctly_rounded_float(log_exact(kernel, xd)),
                    "log(float): result is not correctly rounded");
        }
        std::printf("%s(float) correctly rounded at %llu floats (stride %u)\n", kernel.name,
                    static_cast<unsigned long long>(checked), stride);
    }
}

// FNV-1a: multiplication modulo 2^64 by design.
STX_AUXLIB_MODULAR std::uint64_t log_output_hash(const std::vector<LogKernel>& kernels) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const LogKernel& kernel : kernels) {
        Lcg lcg{97};
        for (int region = 0; region < 4; ++region) {
            for (int i = 0; i < 1000000; ++i) {
                const double x = log_input(region, lcg);
                const double y = (*kernel.owned_double)(x);
                const float yf = (*kernel.owned_float)(static_cast<float>(x));
                const std::uint64_t words[2] = {std::bit_cast<std::uint64_t>(y),
                                                std::bit_cast<std::uint32_t>(yf)};
                for (const std::uint64_t word : words) {
                    hash ^= word;
                    hash *= 1099511628211ULL;
                }
            }
        }
    }
    return hash;
}

// Frozen on 2026-10-31 (Apple arm64, fma and Dekker builds, -O0 and -O2).
constexpr std::uint64_t expected_log_output_hash = 0x2e4ea58329a77abcULL;

void log_determinism(const std::vector<LogKernel>& kernels) {
    const std::uint64_t hash = log_output_hash(kernels);
    std::printf("log output hash %016llx\n", static_cast<unsigned long long>(hash));
    require(hash == expected_log_output_hash, "log: outputs differ from the frozen bits");
}

// ---- pow ----------------------------------------------------------------------

// x^y = e^(y * log x) for positive x, as reference mantissa * 2^exponent.
wide::ScaledWide pow_reference(const double x, const double y) {
    const wide::Wide log_x = wide::log_reference(x);
    const wide::ScaledWide result = wide::exp_reference(wide::multiply(log_x, y));
    return result;
}

// Annex F: every combination of special x and y gives the C library's
// result bit for bit (the host library follows Annex F; the values are
// exact zeros, infinities, ones and NaN, so rounding plays no part).
void pow_special_values() {
    const double infinity = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double xs[] = {0.0, -0.0, 1.0, -1.0, 0.5, -0.5, 2.0, -2.0, infinity, -infinity, nan,
                         0x1p-1074, -0x1p-1074, 0x1.fffffffffffffp1023, -3.0};
    const double ys[] = {0.0, -0.0, 1.0, -1.0, 2.0, -2.0, 3.0, -3.0, 0.5, -0.5, infinity,
                         -infinity, nan, 0x1p53, -0x1p53, 0x1.0000000000001p52, 1.5};
    for (const double x : xs) {
        for (const double y : ys) {
            const double owned = stx_auxlib::math::pow(x, y);
            const double library = std::pow(x, y);
            const bool special = !std::isfinite(library) || library == 0.0 || library == 1.0 ||
                                 library == -1.0 || x == 0.0 || !std::isfinite(x) ||
                                 !std::isfinite(y) || y == 0.0;
            if (!special) continue;
            require(same_bits(owned, library), "pow(double): special input differs from Annex F");
            const float xf = static_cast<float>(x);
            const float yf = static_cast<float>(y);
            const float owned_float = stx_auxlib::math::pow(xf, yf);
            const float library_float = std::pow(xf, yf);
            if (std::isfinite(library_float) && library_float != 0.0f && std::fabs(library_float) != 1.0f)
                continue;
            require(same_bits(owned_float, library_float),
                    "pow(float): special input differs from Annex F");
        }
    }
    // Exactly representable results are exact.
    for (int n = -1074; n <= 1023; ++n) {
        require(stx_auxlib::math::pow(2.0, static_cast<double>(n)) == std::ldexp(1.0, n),
                "pow(double): 2^n is not exact");
    }
    double power = 1.0;
    for (int n = 0; n <= 22; ++n) {
        require(stx_auxlib::math::pow(10.0, static_cast<double>(n)) == power,
                "pow(double): 10^n is not exact");
        power *= 10.0;
    }
    for (int a = 1; a <= 100; ++a) {
        double exact = 1.0;
        for (int b = 0; b <= 12 && exact < 0x1p53; ++b) {
            require(stx_auxlib::math::pow(static_cast<double>(a), static_cast<double>(b)) == exact &&
                        stx_auxlib::math::pow(-static_cast<double>(a), static_cast<double>(b)) ==
                            (b % 2 == 1 ? -exact : exact),
                    "pow(double): small integer power is not exact");
            exact *= static_cast<double>(a);
        }
    }
    require(stx_auxlib::math::pow(4.0, 0.5) == 2.0 && stx_auxlib::math::pow(0.25, -0.5) == 2.0,
            "pow(double): exact square roots are not exact");
}

// Inputs: x in [0.5, 2] with y in [-100, 100]; any exponent of x with
// |y log x| < 700; x within 1e-10 of 1 with |y| ~ 1e10; results near
// overflow; and subnormal results.
void pow_input(const int region, Lcg& lcg, double& x, double& y) {
    if (region == 0) {
        x = 0.5 + 1.5 * lcg.unit();
        y = (lcg.unit() - 0.5) * 200.0;
    } else if (region == 1) {
        const double exponent = std::floor(lcg.unit() * 2000.0) - 1000.0;
        x = std::ldexp(1.0 + lcg.unit(), static_cast<int>(exponent));
        y = (lcg.unit() - 0.5) * 1400.0 / std::fabs(stx_auxlib::math::log(x));
    } else if (region == 2) {
        x = 1.0 + (lcg.unit() - 0.5) * 2e-10;
        y = (lcg.unit() - 0.5) * 2e10;
    } else if (region == 3) {
        x = 1.0 + lcg.unit();
        y = 709.0 / stx_auxlib::math::log(x) * (0.999 + 0.0012 * lcg.unit());
    } else {
        x = 1.0 + lcg.unit();
        y = -1.0 * (708.5 + 36.5 * lcg.unit()) / stx_auxlib::math::log(x);
    }
}

const char* const pow_region_names[] = {"x in [0.5, 2]", "any x", "x = 1 +- 1e-10",
                                        "near overflow", "subnormal"};

void pow_double_accuracy(const int per_region) {
    Lcg lcg{2030};
    for (int region = 0; region < 5; ++region) {
        double worst = 0.0;
        double worst_x = 0.0;
        double worst_y = 0.0;
        std::uint64_t misrounded = 0;
        int measured = 0;
        for (int i = 0; i < per_region; ++i) {
            double x = 0.0;
            double y = 0.0;
            pow_input(region, lcg, x, y);
            const double result = stx_auxlib::math::pow(x, y);
            const wide::ScaledWide exact = pow_reference(x, y);
            double error = 0.0;
            if (region == 4) {
                if (result >= 0x1p-1022) continue;
                const double expected = exp_correctly_rounded_subnormal(exact);
                error = std::fabs(result - expected) / 0x1p-1074;
                if (result != expected) misrounded += 1;
            } else {
                if (!std::isfinite(result) || result < 0x1p-1022) continue;
                error = exp_ulps(result, exact);
                if (error > 0.5) misrounded += 1;
            }
            measured += 1;
            if (error > worst) {
                worst = error;
                worst_x = x;
                worst_y = y;
            }
        }
        std::printf("pow(double) %-15s %10d inputs  max %.6f ulp at (%a, %a)  %llu %s\n",
                    pow_region_names[region], measured, worst, worst_x, worst_y,
                    static_cast<unsigned long long>(misrounded),
                    region == 4 ? "not correctly rounded" : "above 0.5 ulp");
        require(worst <= pow_ulp_limit, "pow(double): error above the stated bound");
    }
}

// Random float pairs, with results across the float range: the result must
// be the correctly rounded x^y.
void pow_float_rounding(const int count) {
    Lcg lcg{2031};
    int checked = 0;
    for (int i = 0; i < count; ++i) {
        const float x = static_cast<float>(lcg.unit() * 16.0);
        const float y = static_cast<float>((lcg.unit() - 0.5) * 80.0);
        const float result = stx_auxlib::math::pow(x, y);
        if (!(x > 0.0f) || !std::isfinite(result) || result < 0x1p-126f) continue;
        const wide::ScaledWide exact = pow_reference(static_cast<double>(x), static_cast<double>(y));
        require(result == exp_correctly_rounded_float(exact),
                "pow(float): result is not correctly rounded");
        checked += 1;
    }
    std::printf("pow(float) correctly rounded at %d random pairs\n", checked);
}

// FNV-1a: multiplication modulo 2^64 by design.
STX_AUXLIB_MODULAR std::uint64_t pow_output_hash() {
    std::uint64_t hash = 1469598103934665603ULL;
    Lcg lcg{96};
    for (int region = 0; region < 5; ++region) {
        for (int i = 0; i < 400000; ++i) {
            double x = 0.0;
            double y = 0.0;
            pow_input(region, lcg, x, y);
            const double result = stx_auxlib::math::pow(x, y);
            const double negative = stx_auxlib::math::pow(-x, std::floor(y));
            const float result_float = stx_auxlib::math::pow(static_cast<float>(x), static_cast<float>(y));
            const std::uint64_t words[3] = {std::bit_cast<std::uint64_t>(result),
                                            std::bit_cast<std::uint64_t>(negative),
                                            std::bit_cast<std::uint32_t>(result_float)};
            for (const std::uint64_t word : words) {
                hash ^= word;
                hash *= 1099511628211ULL;
            }
        }
    }
    return hash;
}

// Frozen on 2026-11-02 (Apple arm64, fma and Dekker builds, -O0 and -O2;
// x86-64 MinGW, reported by the Windows side). pow_input scales its
// exponents by the owned log, so the inputs no longer depend on the host's
// std::log (the 2026-10-31 value did, and differed on Windows).
constexpr std::uint64_t expected_pow_output_hash = 0xc5ae8756613caabfULL;

void pow_determinism() {
    const std::uint64_t hash = pow_output_hash();
    std::printf("pow output hash %016llx\n", static_cast<unsigned long long>(hash));
    require(hash == expected_pow_output_hash, "pow: outputs differ from the frozen bits");
}

// ---- hypot ------------------------------------------------------------------

// Annex F and exact cases.
void hypot_special_values() {
    const double infinity = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double values[] = {0.0, -0.0, 1.0, -3.0, infinity, -infinity, nan, 0x1p-1074,
                             0x1.fffffffffffffp1023};
    for (const double x : values) {
        for (const double y : values) {
            const double library = std::hypot(x, y);
            if (std::isfinite(library) && library != 0.0 && !(x == 0.0 || y == 0.0)) continue;
            require(same_bits(stx_auxlib::math::hypot(x, y), library),
                    "hypot(double): special input differs from Annex F");
            const float xf = static_cast<float>(x);
            const float yf = static_cast<float>(y);
            const float library_float = std::hypot(xf, yf);
            if (std::isfinite(library_float) && library_float != 0.0f && !(xf == 0.0f || yf == 0.0f))
                continue;
            require(same_bits(stx_auxlib::math::hypot(xf, yf), library_float),
                    "hypot(float): special input differs from Annex F");
        }
    }
    // Pythagorean triples are exact, at any common power of two.
    const double triples[][3] = {{3.0, 4.0, 5.0}, {5.0, 12.0, 13.0}, {8.0, 15.0, 17.0},
                                 {20.0, 21.0, 29.0}, {119.0, 120.0, 169.0}};
    for (const double* const triple : triples) {
        const int scales[] = {-1060, -500, 0, 500, 1010};
        for (const int scale : scales) {
            const double x = std::ldexp(triple[0], scale);
            const double y = std::ldexp(triple[1], scale);
            require(stx_auxlib::math::hypot(x, y) == std::ldexp(triple[2], scale) &&
                        stx_auxlib::math::hypot(-y, x) == std::ldexp(triple[2], scale),
                    "hypot(double): Pythagorean triple is not exact");
        }
        require(stx_auxlib::math::hypot(static_cast<float>(triple[0]), static_cast<float>(triple[1])) ==
                    static_cast<float>(triple[2]),
                "hypot(float): Pythagorean triple is not exact");
    }
    require(std::isinf(stx_auxlib::math::hypot(0x1.fffffffffffffp1023, 0x1.fffffffffffffp1023)),
            "hypot(double): did not overflow");
}

void hypot_input(const int region, Lcg& lcg, double& x, double& y) {
    if (region == 0) {
        const double exponent = std::floor(lcg.unit() * 2000.0) - 1000.0;
        x = std::ldexp(lcg.unit(), static_cast<int>(exponent));
        y = x * std::ldexp(lcg.unit(), -static_cast<int>(lcg.unit() * 30.0));
    } else if (region == 1) {
        x = 1.0 + lcg.unit();
        y = x * (1.0 - 1e-3 * lcg.unit());
    } else if (region == 2) {
        x = 1.0 + lcg.unit();
        y = x * std::ldexp(1.0 + lcg.unit(), -28);
    } else if (region == 3) {
        x = std::ldexp(1.0 + lcg.unit(), 1022);
        y = std::ldexp(1.0 + lcg.unit(), 1021);
    } else {
        x = std::ldexp(lcg.unit(), -1060);
        y = std::ldexp(lcg.unit(), -1062);
    }
}

const char* const hypot_region_names[] = {"wide exponents", "ratio ~ 1", "ratio ~ 2^-27",
                                          "near overflow", "subnormal"};

void hypot_double_accuracy(const int per_region) {
    Lcg lcg{2032};
    for (int region = 0; region < 5; ++region) {
        double worst = 0.0;
        std::uint64_t misrounded = 0;
        int measured = 0;
        for (int i = 0; i < per_region; ++i) {
            double x = 0.0;
            double y = 0.0;
            hypot_input(region, lcg, x, y);
            if (x == 0.0 && y == 0.0) continue;
            const double result = stx_auxlib::math::hypot(x, y);
            if (!std::isfinite(result)) continue;
            const wide::ScaledWide exact = wide::hypot_reference(x, y);
            double error = 0.0;
            if (result < 0x1p-1022) {
                const double expected = exp_correctly_rounded_subnormal(exact);
                error = std::fabs(result - expected) / 0x1p-1074;
                if (result != expected) misrounded += 1;
            } else {
                error = exp_ulps(result, exact);
                if (error > 0.5) misrounded += 1;
            }
            measured += 1;
            worst = std::max(worst, error);
        }
        std::printf("hypot(double) %-15s %10d inputs  max %.6f ulp  %llu not correctly rounded\n",
                    hypot_region_names[region], measured, worst,
                    static_cast<unsigned long long>(misrounded));
        require(worst <= hypot_ulp_limit, "hypot(double): error above the stated bound");
    }
}

void hypot_float_rounding(const int count) {
    Lcg lcg{2033};
    int checked = 0;
    for (int i = 0; i < count; ++i) {
        const float x = static_cast<float>(std::ldexp(lcg.unit(), static_cast<int>(lcg.unit() * 250.0) - 125));
        float y = static_cast<float>(x * std::ldexp(lcg.unit(), -static_cast<int>(lcg.unit() * 14.0)));
        if (i % 3 == 0) y = static_cast<float>(lcg.unit());
        const float result = stx_auxlib::math::hypot(x, y);
        if (!std::isfinite(result) || result == 0.0f) continue;
        const float expected = exp_correctly_rounded_float(
            wide::hypot_reference(static_cast<double>(x), static_cast<double>(y)));
        require(result == expected, "hypot(float): result is not correctly rounded");
        checked += 1;
    }
    std::printf("hypot(float) correctly rounded at %d random pairs\n", checked);
}

// FNV-1a: multiplication modulo 2^64 by design.
STX_AUXLIB_MODULAR std::uint64_t hypot_output_hash() {
    std::uint64_t hash = 1469598103934665603ULL;
    Lcg lcg{95};
    for (int region = 0; region < 5; ++region) {
        for (int i = 0; i < 400000; ++i) {
            double x = 0.0;
            double y = 0.0;
            hypot_input(region, lcg, x, y);
            const double result = stx_auxlib::math::hypot(x, -y);
            const float result_float = stx_auxlib::math::hypot(static_cast<float>(x), static_cast<float>(y));
            const std::uint64_t words[2] = {std::bit_cast<std::uint64_t>(result),
                                            std::bit_cast<std::uint32_t>(result_float)};
            for (const std::uint64_t word : words) {
                hash ^= word;
                hash *= 1099511628211ULL;
            }
        }
    }
    return hash;
}

// Frozen on 2026-10-31 (Apple arm64, fma and Dekker builds, -O0 and -O2).
constexpr std::uint64_t expected_hypot_output_hash = 0x1c19c8b2f3910d6cULL;

void hypot_determinism() {
    const std::uint64_t hash = hypot_output_hash();
    std::printf("hypot output hash %016llx\n", static_cast<unsigned long long>(hash));
    require(hash == expected_hypot_output_hash, "hypot: outputs differ from the frozen bits");
}

// ---- atan, atan2, asin, acos ------------------------------------------------------

using UnaryDouble = double (*)(double x);
using UnaryFloat = float (*)(float x);
using UnaryReference = wide::Wide (*)(double x);

double owned_atan_d(const double x) { return stx_auxlib::math::atan(x); }
float owned_atan_f(const float x) { return stx_auxlib::math::atan(x); }
double owned_asin_d(const double x) { return stx_auxlib::math::asin(x); }
float owned_asin_f(const float x) { return stx_auxlib::math::asin(x); }
double owned_acos_d(const double x) { return stx_auxlib::math::acos(x); }
float owned_acos_f(const float x) { return stx_auxlib::math::acos(x); }
double library_atan_d(const double x) { return std::atan(x); }
double library_asin_d(const double x) { return std::asin(x); }
double library_acos_d(const double x) { return std::acos(x); }

wide::Wide atan_exact(const double x) {
    const wide::Wide magnitude = wide::atan_reference(wide::Wide{std::fabs(x), 0.0});
    if (x < 0.0) return wide::negate(magnitude);
    return magnitude;
}

struct UnaryKernel final {
    const char* name{nullptr};
    UnaryDouble owned_double{nullptr};
    UnaryFloat owned_float{nullptr};
    UnaryDouble library_double{nullptr};
    UnaryReference exact{nullptr};
    double domain{0.0};  // |x| <= domain is the finite domain (0: unbounded)
};

const UnaryKernel inverse_trig_kernels[] = {
    {"atan", owned_atan_d, owned_atan_f, library_atan_d, atan_exact, 0.0},
    {"asin", owned_asin_d, owned_asin_f, library_asin_d, wide::asin_reference, 1.0},
    {"acos", owned_acos_d, owned_acos_f, library_acos_d, wide::acos_reference, 1.0},
};

// Annex F: special inputs match the C library bit for bit (their results
// are zeros, NaN, or pi multiples that both round from the same constant).
void inverse_trig_special_values() {
    const double infinity = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double unary_inputs[] = {0.0, -0.0, 1.0, -1.0, 2.0, -2.0, infinity, -infinity, nan,
                                   0x1p-1074, -0x1p-30, 0x1p61, -0x1p70};
    for (const UnaryKernel& kernel : inverse_trig_kernels) {
        for (const double x : unary_inputs) {
            // Ordinary values (atan(+-2), asin and acos inside the domain) are
            // checked for accuracy elsewhere, not for the library's bits.
            const bool ordinary = std::isfinite(x) && std::fabs(x) > 0x1p-27 &&
                                  (kernel.domain == 0.0 ? std::fabs(x) < 0x1p60 : std::fabs(x) < 1.0);
            if (ordinary) continue;
            require(same_bits((*kernel.owned_double)(x), (*kernel.library_double)(x)),
                    "inverse trig (double): special input differs from Annex F");
            const float xf = static_cast<float>(x);
            require(same_bits((*kernel.owned_float)(xf), static_cast<float>((*kernel.library_double)(xf))) ||
                        std::fabs(xf) == 1.0f || std::fabs(xf) > 0x1p60f,
                    "inverse trig (float): special input differs from Annex F");
        }
    }
    const double atan2_inputs[] = {0.0, -0.0, 1.0, -1.0, infinity, -infinity, nan, 0x1p-1074,
                                   -0x1.fffffffffffffp1023};
    for (const double y : atan2_inputs) {
        for (const double x : atan2_inputs) {
            const double library = std::atan2(y, x);
            const bool special = !std::isfinite(x) || !std::isfinite(y) || x == 0.0 || y == 0.0;
            if (!special) continue;
            require(same_bits(stx_auxlib::math::atan2(y, x), library),
                    "atan2(double): special input differs from Annex F");
        }
    }
    require(same_bits(stx_auxlib::math::acos(1.0), 0.0) && same_bits(stx_auxlib::math::acos(1.0f), 0.0f),
            "acos: acos(1) is not +0");
}

double inverse_trig_input(const UnaryKernel& kernel, const int region, Lcg& lcg) {
    if (kernel.domain == 0.0) {
        const double magnitude = std::ldexp(lcg.unit(), static_cast<int>(lcg.unit() * 80.0) - 40);
        return region == 0 ? magnitude : -magnitude;
    }
    if (region == 0) return 2.0 * lcg.unit() - 1.0;
    // Next to +-1, where 1 - x^2 cancels.
    const double gap = std::ldexp(lcg.unit(), -static_cast<int>(lcg.unit() * 50.0));
    return lcg.unit() < 0.5 ? 1.0 - gap : gap - 1.0;
}

void inverse_trig_double_accuracy(const int per_region) {
    for (const UnaryKernel& kernel : inverse_trig_kernels) {
        Lcg lcg{2034};
        for (int region = 0; region < 2; ++region) {
            double worst = 0.0;
            double worst_x = 0.0;
            std::uint64_t misrounded = 0;
            for (int i = 0; i < per_region; ++i) {
                const double x = inverse_trig_input(kernel, region, lcg);
                const double error = double_ulps((*kernel.owned_double)(x), (*kernel.exact)(x));
                if (error > 0.5) misrounded += 1;
                if (error > worst) {
                    worst = error;
                    worst_x = x;
                }
            }
            const char* const region_name = kernel.domain == 0.0 ? (region == 0 ? "positive" : "negative")
                                                                 : (region == 0 ? "[-1, 1]" : "near +-1");
            std::printf("%s(double) %-10s %10d inputs  max %.6f ulp at %a  %llu above 0.5 ulp\n",
                        kernel.name, region_name, per_region, worst, worst_x,
                        static_cast<unsigned long long>(misrounded));
            require(worst <= inverse_trig_ulp_limit, "inverse trig (double): error above the bound");
        }
    }
    Lcg lcg{2035};
    double worst = 0.0;
    for (int i = 0; i < per_region; ++i) {
        const double x = std::ldexp(lcg.unit() - 0.5, static_cast<int>(lcg.unit() * 40.0) - 20);
        const double y = std::ldexp(lcg.unit() - 0.5, static_cast<int>(lcg.unit() * 40.0) - 20);
        if (x == 0.0 || y == 0.0) continue;
        worst = std::max(worst, double_ulps(stx_auxlib::math::atan2(y, x), wide::atan2_reference(y, x)));
    }
    std::printf("atan2(double) %10d pairs  max %.6f ulp\n", per_region, worst);
    require(worst <= inverse_trig_ulp_limit, "atan2(double): error above the bound");
}

// Every `stride`-th float in each function's domain, both signs.
void inverse_trig_float_rounding(const std::uint32_t stride) {
    for (const UnaryKernel& kernel : inverse_trig_kernels) {
        const float top = kernel.domain == 0.0 ? std::numeric_limits<float>::max() : 1.0f;
        const std::uint32_t last = std::bit_cast<std::uint32_t>(top);
        std::uint64_t checked = 0;
        for (std::uint64_t pattern = 0; pattern <= last; pattern += stride) {
            const float magnitude = std::bit_cast<float>(static_cast<std::uint32_t>(pattern));
            const float signs[2] = {magnitude, -magnitude};
            for (const float x : signs) {
                const float y = (*kernel.owned_float)(x);
                const double xd = static_cast<double>(x);
                const float screen = static_cast<float>((*kernel.library_double)(xd));
                checked += 1;
                if (y == screen) continue;
                require(y == correctly_rounded_float((*kernel.exact)(xd)),
                        "inverse trig (float): result is not correctly rounded");
            }
        }
        std::printf("%s(float) correctly rounded at %llu floats (stride %u)\n", kernel.name,
                    static_cast<unsigned long long>(checked), stride);
    }
    Lcg lcg{2036};
    int checked = 0;
    for (int i = 0; i < 300000; ++i) {
        const float x = static_cast<float>(std::ldexp(lcg.unit() - 0.5, static_cast<int>(lcg.unit() * 60.0) - 30));
        const float y = static_cast<float>(std::ldexp(lcg.unit() - 0.5, static_cast<int>(lcg.unit() * 60.0) - 30));
        if (x == 0.0f || y == 0.0f) continue;
        const float result = stx_auxlib::math::atan2(y, x);
        require(result == correctly_rounded_float(wide::atan2_reference(static_cast<double>(y), static_cast<double>(x))),
                "atan2(float): result is not correctly rounded");
        checked += 1;
    }
    std::printf("atan2(float) correctly rounded at %d random pairs\n", checked);
}

// FNV-1a: multiplication modulo 2^64 by design.
STX_AUXLIB_MODULAR std::uint64_t inverse_trig_output_hash() {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const UnaryKernel& kernel : inverse_trig_kernels) {
        Lcg lcg{94};
        for (int region = 0; region < 2; ++region) {
            for (int i = 0; i < 500000; ++i) {
                const double x = inverse_trig_input(kernel, region, lcg);
                const double y = (*kernel.owned_double)(x);
                const float yf = (*kernel.owned_float)(static_cast<float>(x));
                const std::uint64_t words[2] = {std::bit_cast<std::uint64_t>(y), std::bit_cast<std::uint32_t>(yf)};
                for (const std::uint64_t word : words) {
                    hash ^= word;
                    hash *= 1099511628211ULL;
                }
            }
        }
    }
    Lcg lcg{93};
    for (int i = 0; i < 1000000; ++i) {
        const double x = std::ldexp(lcg.unit() - 0.5, static_cast<int>(lcg.unit() * 40.0) - 20);
        const double y = std::ldexp(lcg.unit() - 0.5, static_cast<int>(lcg.unit() * 40.0) - 20);
        const std::uint64_t words[2] = {std::bit_cast<std::uint64_t>(stx_auxlib::math::atan2(y, x)),
                                        std::bit_cast<std::uint32_t>(stx_auxlib::math::atan2(
                                            static_cast<float>(y), static_cast<float>(x)))};
        for (const std::uint64_t word : words) {
            hash ^= word;
            hash *= 1099511628211ULL;
        }
    }
    return hash;
}

// Frozen on 2026-10-31 (Apple arm64, fma and Dekker builds, -O0 and -O2).
constexpr std::uint64_t expected_inverse_trig_output_hash = 0xcaa4628c93d69869ULL;

void inverse_trig_determinism() {
    const std::uint64_t hash = inverse_trig_output_hash();
    std::printf("inverse trig output hash %016llx\n", static_cast<unsigned long long>(hash));
    require(hash == expected_inverse_trig_output_hash, "inverse trig: outputs differ from the frozen bits");
}

// ---- tan, cbrt, hyperbolic ------------------------------------------------------------

double owned_tan_d(const double x) { return stx_auxlib::math::tan(x); }
float owned_tan_f(const float x) { return stx_auxlib::math::tan(x); }
double owned_sinh_d(const double x) { return stx_auxlib::math::sinh(x); }
float owned_sinh_f(const float x) { return stx_auxlib::math::sinh(x); }
double owned_cosh_d(const double x) { return stx_auxlib::math::cosh(x); }
float owned_cosh_f(const float x) { return stx_auxlib::math::cosh(x); }
double owned_tanh_d(const double x) { return stx_auxlib::math::tanh(x); }
float owned_tanh_f(const float x) { return stx_auxlib::math::tanh(x); }
double owned_asinh_d(const double x) { return stx_auxlib::math::asinh(x); }
float owned_asinh_f(const float x) { return stx_auxlib::math::asinh(x); }
double owned_acosh_d(const double x) { return stx_auxlib::math::acosh(x); }
float owned_acosh_f(const float x) { return stx_auxlib::math::acosh(x); }
double owned_atanh_d(const double x) { return stx_auxlib::math::atanh(x); }
float owned_atanh_f(const float x) { return stx_auxlib::math::atanh(x); }
double owned_cbrt_d(const double x) { return stx_auxlib::math::cbrt(x); }
float owned_cbrt_f(const float x) { return stx_auxlib::math::cbrt(x); }
double library_tan_d(const double x) { return std::tan(x); }
double library_sinh_d(const double x) { return std::sinh(x); }
double library_cosh_d(const double x) { return std::cosh(x); }
double library_tanh_d(const double x) { return std::tanh(x); }
double library_asinh_d(const double x) { return std::asinh(x); }
double library_acosh_d(const double x) { return std::acosh(x); }
double library_atanh_d(const double x) { return std::atanh(x); }
double library_cbrt_d(const double x) { return std::cbrt(x); }

// cbrt's reference as a plain double-double (results stay normal).
wide::Wide cbrt_exact(const double x) {
    const wide::ScaledWide value = wide::cbrt_reference(x);
    const wide::Wide result{std::ldexp(value.mantissa.hi, value.exponent),
                            std::ldexp(value.mantissa.lo, value.exponent)};
    return result;
}

// A sampled region: uniform in [low, high], or (logarithmic) magnitudes
// 2^[low, high) with both signs.
struct SampleRegion final {
    double low{0.0};
    double high{0.0};
    bool logarithmic{false};
};

struct RegionKernel final {
    const char* name{nullptr};
    UnaryDouble owned_double{nullptr};
    UnaryFloat owned_float{nullptr};
    UnaryDouble library_double{nullptr};
    UnaryReference exact{nullptr};
    SampleRegion regions[3]{};
    float float_low{0.0f};   // float exhaustive range (magnitudes, both signs unless
    float float_high{0.0f};  // float_low >= 1, which marks acosh's one-sided domain)
};

const RegionKernel remaining_kernels[] = {
    {"tan", owned_tan_d, owned_tan_f, library_tan_d, wide::tan_reference,
     {{-20.0, 20.0, false}, {1.5, 1.6415, false}, {-26.0, 32.0, true}}, 0x1p-27f, 0x1p32f},
    {"cbrt", owned_cbrt_d, owned_cbrt_f, library_cbrt_d, cbrt_exact,
     {{-1000.0, 1000.0, true}, {0.0, 8.0, false}, {-1074.0, -1022.0, true}}, 0.0f, 3.4e38f},
    {"sinh", owned_sinh_d, owned_sinh_f, library_sinh_d, wide::sinh_reference,
     {{-26.0, 9.0, true}, {-700.0, 700.0, false}, {-0.125, 0.125, false}}, 0.0f, 90.0f},
    {"cosh", owned_cosh_d, owned_cosh_f, library_cosh_d, wide::cosh_reference,
     {{-26.0, 9.0, true}, {-700.0, 700.0, false}, {-1.0, 1.0, false}}, 0.0f, 90.0f},
    {"tanh", owned_tanh_d, owned_tanh_f, library_tanh_d, wide::tanh_reference,
     {{-26.0, 5.0, true}, {-0.125, 0.125, false}, {-22.0, 22.0, false}}, 0.0f, 23.0f},
    {"asinh", owned_asinh_d, owned_asinh_f, library_asinh_d, wide::asinh_reference,
     {{-26.0, 27.0, true}, {-1.0, 1.0, false}, {-1e8, 1e8, false}}, 0.0f, 0x1p28f},
    {"acosh", owned_acosh_d, owned_acosh_f, library_acosh_d, wide::acosh_reference,
     {{1.0, 3.0, false}, {1.0, 1.000001, false}, {2.0, 1e8, false}}, 1.0f, 0x1p28f},
    {"atanh", owned_atanh_d, owned_atanh_f, library_atanh_d, wide::atanh_reference,
     {{-26.0, -1.0, true}, {0.99, 0.9999999, false}, {-0.5, 0.5, false}}, 0.0f, 1.0f},
};

double region_input(const SampleRegion& region, Lcg& lcg, const int i) {
    if (!region.logarithmic) return region.low + (region.high - region.low) * lcg.unit();
    const double exponent = std::floor(region.low + (region.high - region.low) * lcg.unit());
    const double magnitude = std::ldexp(0.5 + 0.5 * lcg.unit(), static_cast<int>(exponent));
    return (i % 2 == 0) ? magnitude : -magnitude;
}

// Recorded results for inputs whose results are zeros, ones, infinities, NaN
// or tiny (|x| < 2^-27). They are recorded rather than taken from the host C
// library, which differs between hosts: UCRT's cbrt(-0x1p-40) is one ulp
// below the correctly rounded -0x1.965fea53d6e3dp-14 (long double cbrtl
// agrees with the recorded value), and UCRT's tan(+-infinity) is a negative
// NaN. Any NaN matches a recorded NaN, since C leaves its sign unspecified.
struct SpecialValue final {
    UnaryDouble owned{nullptr};
    double x{0.0};
    std::uint64_t expected_bits{0};
};

void remaining_special_values() {
    const double infinity = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const SpecialValue recorded[] = {
        {owned_tan_d, 0.0, 0x0000000000000000ULL},
        {owned_tan_d, -0.0, 0x8000000000000000ULL},
        {owned_tan_d, infinity, 0x7ff8000000000000ULL},
        {owned_tan_d, -infinity, 0x7ff8000000000000ULL},
        {owned_tan_d, nan, 0x7ff8000000000000ULL},
        {owned_tan_d, 0x1p-1074, 0x0000000000000001ULL},
        {owned_tan_d, -0x1p-40, 0xbd70000000000000ULL},
        {owned_cbrt_d, 0.0, 0x0000000000000000ULL},
        {owned_cbrt_d, -0.0, 0x8000000000000000ULL},
        {owned_cbrt_d, 1.0, 0x3ff0000000000000ULL},
        {owned_cbrt_d, -1.0, 0xbff0000000000000ULL},
        {owned_cbrt_d, infinity, 0x7ff0000000000000ULL},
        {owned_cbrt_d, -infinity, 0xfff0000000000000ULL},
        {owned_cbrt_d, nan, 0x7ff8000000000000ULL},
        {owned_cbrt_d, 0x1p-1074, 0x2990000000000000ULL},
        {owned_cbrt_d, -0x1p-40, 0xbf1965fea53d6e3dULL},
        {owned_sinh_d, 0.0, 0x0000000000000000ULL},
        {owned_sinh_d, -0.0, 0x8000000000000000ULL},
        {owned_sinh_d, infinity, 0x7ff0000000000000ULL},
        {owned_sinh_d, -infinity, 0xfff0000000000000ULL},
        {owned_sinh_d, nan, 0x7ff8000000000000ULL},
        {owned_sinh_d, 0x1p-1074, 0x0000000000000001ULL},
        {owned_sinh_d, -0x1p-40, 0xbd70000000000000ULL},
        {owned_sinh_d, 800.0, 0x7ff0000000000000ULL},
        {owned_sinh_d, -800.0, 0xfff0000000000000ULL},
        {owned_sinh_d, 1e300, 0x7ff0000000000000ULL},
        {owned_cosh_d, 0.0, 0x3ff0000000000000ULL},
        {owned_cosh_d, -0.0, 0x3ff0000000000000ULL},
        {owned_cosh_d, infinity, 0x7ff0000000000000ULL},
        {owned_cosh_d, -infinity, 0x7ff0000000000000ULL},
        {owned_cosh_d, nan, 0x7ff8000000000000ULL},
        {owned_cosh_d, 0x1p-1074, 0x3ff0000000000000ULL},
        {owned_cosh_d, -0x1p-40, 0x3ff0000000000000ULL},
        {owned_cosh_d, 800.0, 0x7ff0000000000000ULL},
        {owned_cosh_d, -800.0, 0x7ff0000000000000ULL},
        {owned_cosh_d, 1e300, 0x7ff0000000000000ULL},
        {owned_tanh_d, 0.0, 0x0000000000000000ULL},
        {owned_tanh_d, -0.0, 0x8000000000000000ULL},
        {owned_tanh_d, infinity, 0x3ff0000000000000ULL},
        {owned_tanh_d, -infinity, 0xbff0000000000000ULL},
        {owned_tanh_d, nan, 0x7ff8000000000000ULL},
        {owned_tanh_d, 0x1p-1074, 0x0000000000000001ULL},
        {owned_tanh_d, -0x1p-40, 0xbd70000000000000ULL},
        {owned_tanh_d, 800.0, 0x3ff0000000000000ULL},
        {owned_tanh_d, -800.0, 0xbff0000000000000ULL},
        {owned_tanh_d, 1e300, 0x3ff0000000000000ULL},
        {owned_asinh_d, 0.0, 0x0000000000000000ULL},
        {owned_asinh_d, -0.0, 0x8000000000000000ULL},
        {owned_asinh_d, infinity, 0x7ff0000000000000ULL},
        {owned_asinh_d, -infinity, 0xfff0000000000000ULL},
        {owned_asinh_d, nan, 0x7ff8000000000000ULL},
        {owned_asinh_d, 0x1p-1074, 0x0000000000000001ULL},
        {owned_asinh_d, -0x1p-40, 0xbd70000000000000ULL},
        {owned_acosh_d, 0.0, 0x7ff8000000000000ULL},
        {owned_acosh_d, -0.0, 0x7ff8000000000000ULL},
        {owned_acosh_d, 1.0, 0x0000000000000000ULL},
        {owned_acosh_d, -1.0, 0x7ff8000000000000ULL},
        {owned_acosh_d, -2.0, 0x7ff8000000000000ULL},
        {owned_acosh_d, infinity, 0x7ff0000000000000ULL},
        {owned_acosh_d, -infinity, 0x7ff8000000000000ULL},
        {owned_acosh_d, nan, 0x7ff8000000000000ULL},
        {owned_acosh_d, 0x1p-1074, 0x7ff8000000000000ULL},
        {owned_acosh_d, -0x1p-40, 0x7ff8000000000000ULL},
        {owned_acosh_d, -800.0, 0x7ff8000000000000ULL},
        {owned_atanh_d, 0.0, 0x0000000000000000ULL},
        {owned_atanh_d, -0.0, 0x8000000000000000ULL},
        {owned_atanh_d, 1.0, 0x7ff0000000000000ULL},
        {owned_atanh_d, -1.0, 0xfff0000000000000ULL},
        {owned_atanh_d, 2.0, 0x7ff8000000000000ULL},
        {owned_atanh_d, -2.0, 0x7ff8000000000000ULL},
        {owned_atanh_d, infinity, 0x7ff8000000000000ULL},
        {owned_atanh_d, -infinity, 0x7ff8000000000000ULL},
        {owned_atanh_d, nan, 0x7ff8000000000000ULL},
        {owned_atanh_d, 0x1p-1074, 0x0000000000000001ULL},
        {owned_atanh_d, -0x1p-40, 0xbd70000000000000ULL},
        {owned_atanh_d, 800.0, 0x7ff8000000000000ULL},
        {owned_atanh_d, -800.0, 0x7ff8000000000000ULL},
        {owned_atanh_d, 1e300, 0x7ff8000000000000ULL},
    };
    for (const SpecialValue& special : recorded) {
        const double expected = std::bit_cast<double>(special.expected_bits);
        require(same_bits((*special.owned)(special.x), expected),
                "tan/cbrt/hyperbolic (double): special input differs from its recorded result");
    }
    for (int k = 1; k <= 200; ++k) {
        const double cube = static_cast<double>(k) * static_cast<double>(k) * static_cast<double>(k);
        require(stx_auxlib::math::cbrt(cube) == static_cast<double>(k) &&
                    stx_auxlib::math::cbrt(-cube) == -static_cast<double>(k) &&
                    stx_auxlib::math::cbrt(static_cast<float>(cube)) == static_cast<float>(k),
                "cbrt: an exact cube's root is not exact");
    }
    require(stx_auxlib::math::cbrt(0x1p-1074) == 0x1p-358, "cbrt(double): 2^-1074 has no exact root");
}

void remaining_double_accuracy(const int per_region) {
    for (const RegionKernel& kernel : remaining_kernels) {
        Lcg lcg{2037};
        for (const SampleRegion& region : kernel.regions) {
            double worst = 0.0;
            double worst_x = 0.0;
            std::uint64_t misrounded = 0;
            for (int i = 0; i < per_region; ++i) {
                const double x = region_input(region, lcg, i);
                const double error = double_ulps((*kernel.owned_double)(x), (*kernel.exact)(x));
                if (error > 0.5) misrounded += 1;
                if (error > worst) {
                    worst = error;
                    worst_x = x;
                }
            }
            std::printf("%s(double) [%g, %g]%s %10d inputs  max %.6f ulp at %a  %llu above 0.5 ulp\n",
                        kernel.name, region.low, region.high, region.logarithmic ? " (2^)" : "",
                        per_region, worst, worst_x, static_cast<unsigned long long>(misrounded));
            const double limit = std::strcmp(kernel.name, "tan") == 0 ? tan_ulp_limit : remaining_ulp_limit;
            require(worst <= limit, "tan/cbrt/hyperbolic (double): error above the stated bound");
        }
    }
}

// Every `stride`-th float with magnitude in each kernel's float range (both
// signs, except acosh's one-sided domain).
void remaining_float_rounding(const std::uint32_t stride) {
    for (const RegionKernel& kernel : remaining_kernels) {
        const std::uint32_t first = std::bit_cast<std::uint32_t>(kernel.float_low);
        const std::uint32_t last = std::bit_cast<std::uint32_t>(kernel.float_high);
        const bool one_sided = kernel.float_low >= 1.0f;
        std::uint64_t checked = 0;
        for (std::uint64_t pattern = first; pattern <= last; pattern += stride) {
            const float magnitude = std::bit_cast<float>(static_cast<std::uint32_t>(pattern));
            const float signs[2] = {magnitude, -magnitude};
            for (int sign = 0; sign < (one_sided ? 1 : 2); ++sign) {
                const float x = signs[sign];
                const float y = (*kernel.owned_float)(x);
                const double xd = static_cast<double>(x);
                const float screen = static_cast<float>((*kernel.library_double)(xd));
                checked += 1;
                if (same_bits(y, screen)) continue;
                require(std::isfinite(y) && y == correctly_rounded_float((*kernel.exact)(xd)),
                        "tan/cbrt/hyperbolic (float): result is not correctly rounded");
            }
        }
        std::printf("%s(float) correctly rounded at %llu floats (stride %u)\n", kernel.name,
                    static_cast<unsigned long long>(checked), stride);
    }
}

// FNV-1a: multiplication modulo 2^64 by design.
STX_AUXLIB_MODULAR std::uint64_t remaining_output_hash() {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const RegionKernel& kernel : remaining_kernels) {
        Lcg lcg{92};
        for (const SampleRegion& region : kernel.regions) {
            for (int i = 0; i < 300000; ++i) {
                const double x = region_input(region, lcg, i);
                const std::uint64_t words[2] = {
                    std::bit_cast<std::uint64_t>((*kernel.owned_double)(x)),
                    std::bit_cast<std::uint32_t>((*kernel.owned_float)(static_cast<float>(x)))};
                for (const std::uint64_t word : words) {
                    hash ^= word;
                    hash *= 1099511628211ULL;
                }
            }
        }
    }
    return hash;
}

// Frozen on 2026-10-31 (Apple arm64, fma and Dekker builds, -O0 and -O2).
constexpr std::uint64_t expected_remaining_output_hash = 0xd4817d61597e5a9dULL;

void remaining_determinism() {
    const std::uint64_t hash = remaining_output_hash();
    std::printf("tan/cbrt/hyperbolic output hash %016llx\n", static_cast<unsigned long long>(hash));
    require(hash == expected_remaining_output_hash, "tan/cbrt/hyperbolic: outputs differ from the frozen bits");
}

// ---- --bench ----------------------------------------------------------------

struct BenchBuffers final {
    std::vector<double> x{};
    std::vector<float> xf{};
    std::vector<double> s{};
    std::vector<double> c{};
    std::vector<float> sf{};
    std::vector<float> cf{};
};

void library_double(BenchBuffers& b) {
    const std::size_t count = b.x.size();
    for (std::size_t i = 0; i < count; ++i) {
        b.s[i] = std::sin(b.x[i]);
        b.c[i] = std::cos(b.x[i]);
    }
}

void owned_double(BenchBuffers& b) {
    const std::size_t count = b.x.size();
    for (std::size_t i = 0; i < count; ++i) stx_auxlib::math::sincos(b.x[i], b.s[i], b.c[i]);
}

void library_float(BenchBuffers& b) {
    const std::size_t count = b.xf.size();
    for (std::size_t i = 0; i < count; ++i) {
        b.sf[i] = std::sin(b.xf[i]);
        b.cf[i] = std::cos(b.xf[i]);
    }
}

void owned_float(BenchBuffers& b) {
    const std::size_t count = b.xf.size();
    for (std::size_t i = 0; i < count; ++i) stx_auxlib::math::sincos(b.xf[i], b.sf[i], b.cf[i]);
}

void library_exp_double(BenchBuffers& b) {
    const std::size_t count = b.x.size();
    for (std::size_t i = 0; i < count; ++i) b.s[i] = std::exp(b.x[i]);
}

void owned_exp_double(BenchBuffers& b) {
    const std::size_t count = b.x.size();
    for (std::size_t i = 0; i < count; ++i) b.s[i] = stx_auxlib::math::exp(b.x[i]);
}

void library_exp_float(BenchBuffers& b) {
    const std::size_t count = b.xf.size();
    for (std::size_t i = 0; i < count; ++i) b.sf[i] = std::exp(b.xf[i]);
}

void owned_exp_float(BenchBuffers& b) {
    const std::size_t count = b.xf.size();
    for (std::size_t i = 0; i < count; ++i) b.sf[i] = stx_auxlib::math::exp(b.xf[i]);
}

void library_log_double(BenchBuffers& b) {
    const std::size_t count = b.x.size();
    for (std::size_t i = 0; i < count; ++i) b.s[i] = std::log(b.x[i]);
}

void owned_log_double(BenchBuffers& b) {
    const std::size_t count = b.x.size();
    for (std::size_t i = 0; i < count; ++i) b.s[i] = stx_auxlib::math::log(b.x[i]);
}

void library_log_float(BenchBuffers& b) {
    const std::size_t count = b.xf.size();
    for (std::size_t i = 0; i < count; ++i) b.sf[i] = std::log(b.xf[i]);
}

void owned_log_float(BenchBuffers& b) {
    const std::size_t count = b.xf.size();
    for (std::size_t i = 0; i < count; ++i) b.sf[i] = stx_auxlib::math::log(b.xf[i]);
}

// pow takes x from the region and y from a fixed spread (stored in c / cf).
void library_pow_double(BenchBuffers& b) {
    const std::size_t count = b.x.size();
    for (std::size_t i = 0; i < count; ++i) b.s[i] = std::pow(b.x[i], b.c[i]);
}

void owned_pow_double(BenchBuffers& b) {
    const std::size_t count = b.x.size();
    for (std::size_t i = 0; i < count; ++i) b.s[i] = stx_auxlib::math::pow(b.x[i], b.c[i]);
}

void library_pow_float(BenchBuffers& b) {
    const std::size_t count = b.xf.size();
    for (std::size_t i = 0; i < count; ++i) b.sf[i] = std::pow(b.xf[i], b.cf[i]);
}

void owned_pow_float(BenchBuffers& b) {
    const std::size_t count = b.xf.size();
    for (std::size_t i = 0; i < count; ++i) b.sf[i] = stx_auxlib::math::pow(b.xf[i], b.cf[i]);
}

void library_hypot_double(BenchBuffers& b) {
    const std::size_t count = b.x.size();
    for (std::size_t i = 0; i < count; ++i) b.s[i] = std::hypot(b.x[i], b.c[i]);
}

void owned_hypot_double(BenchBuffers& b) {
    const std::size_t count = b.x.size();
    for (std::size_t i = 0; i < count; ++i) b.s[i] = stx_auxlib::math::hypot(b.x[i], b.c[i]);
}

void library_hypot_float(BenchBuffers& b) {
    const std::size_t count = b.xf.size();
    for (std::size_t i = 0; i < count; ++i) b.sf[i] = std::hypot(b.xf[i], b.cf[i]);
}

void owned_hypot_float(BenchBuffers& b) {
    const std::size_t count = b.xf.size();
    for (std::size_t i = 0; i < count; ++i) b.sf[i] = stx_auxlib::math::hypot(b.xf[i], b.cf[i]);
}

using BenchKernel = void (*)(BenchBuffers& buffers);

double best_ns_per_call(const BenchKernel kernel, BenchBuffers& buffers) {
    double best = std::numeric_limits<double>::infinity();
    for (int trial = 0; trial < 15; ++trial) {
        const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        kernel(buffers);
        const std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
        const double ns = std::chrono::duration<double, std::nano>(end - start).count();
        best = std::min(best, ns / static_cast<double>(buffers.x.size()));
    }
    return best;
}

const Region log_bench_regions[] = {
    {"[1e-300, 1e300]", 1e-300, 1e300},
    {"[0.5, 2]", 0.5, 2.0},
    {"1 +- 1e-3", 0.999, 1.001},
};

const Region pow_bench_regions[] = {
    {"x in [0.5, 2]", 0.5, 2.0},
    {"x in [0, 100]", 0.0, 100.0},
};

struct BenchSet final {
    const char* title{nullptr};
    BenchKernel library_double{nullptr};
    BenchKernel owned_double{nullptr};
    BenchKernel library_float{nullptr};
    BenchKernel owned_float{nullptr};
    const Region* regions{nullptr};
    std::size_t region_count{0};
};

// The owner's ceiling: no owned function may take more than twice as long
// as the C library's on the same inputs.
constexpr double speed_ceiling = 2.0;

// Prints one set's table with owned/library ratios; returns the largest
// ratio among gated regions.
double bench_set(const BenchSet& set) {
    const std::size_t count = std::size_t{1} << 20;
    std::printf("\n%s, ns per call, %zu inputs, best of 15\n", set.title, count);
    std::printf("%-16s %10s %10s %7s %10s %10s %7s\n", "region", "std dbl", "owned dbl", "ratio",
                "std flt", "owned flt", "ratio");
    double worst = 0.0;
    for (std::size_t r = 0; r < set.region_count; ++r) {
        const Region& region = set.regions[r];
        BenchBuffers b{};
        Lcg lcg{12345};
        b.x.resize(count);
        b.xf.resize(count);
        for (std::size_t i = 0; i < count; ++i) {
            b.x[i] = region.low + (region.high - region.low) * lcg.unit();
            b.xf[i] = static_cast<float>(b.x[i]);
        }
        b.s.resize(count);
        b.c.resize(count);
        b.sf.resize(count);
        b.cf.resize(count);
        // Second operand for pow, hypot and atan2: in [-8, 8].
        for (std::size_t i = 0; i < count; ++i) {
            b.c[i] = (lcg.unit() - 0.5) * 16.0;
            b.cf[i] = static_cast<float>(b.c[i]);
        }
        const double std_double = best_ns_per_call(set.library_double, b);
        const double our_double = best_ns_per_call(set.owned_double, b);
        const double std_float = best_ns_per_call(set.library_float, b);
        const double our_float = best_ns_per_call(set.owned_float, b);
        const double double_ratio = our_double / std_double;
        const double float_ratio = our_float / std_float;
        std::printf("%-16s %10.3f %10.3f %7.2f %10.3f %10.3f %7.2f%s\n", region.name, std_double,
                    our_double, double_ratio, std_float, our_float, float_ratio,
                    region.gated ? "" : "  (not gated)");
        if (region.gated) worst = std::max(worst, std::max(double_ratio, float_ratio));
    }
    return worst;
}

// Unary kernels by template parameter, so each loop calls its function
// directly.
template <double (*Function)(double)>
void unary_double(BenchBuffers& b) {
    const std::size_t count = b.x.size();
    for (std::size_t i = 0; i < count; ++i) b.s[i] = Function(b.x[i]);
}

template <float (*Function)(float)>
void unary_float(BenchBuffers& b) {
    const std::size_t count = b.xf.size();
    for (std::size_t i = 0; i < count; ++i) b.sf[i] = Function(b.xf[i]);
}

float library_atan_f(const float x) { return std::atan(x); }
float library_asin_f(const float x) { return std::asin(x); }
float library_acos_f(const float x) { return std::acos(x); }
float library_tan_f(const float x) { return std::tan(x); }
float library_cbrt_f(const float x) { return std::cbrt(x); }
float library_sinh_f(const float x) { return std::sinh(x); }
float library_cosh_f(const float x) { return std::cosh(x); }
float library_tanh_f(const float x) { return std::tanh(x); }
float library_asinh_f(const float x) { return std::asinh(x); }
float library_acosh_f(const float x) { return std::acosh(x); }
float library_atanh_f(const float x) { return std::atanh(x); }
float library_log2_f(const float x) { return std::log2(x); }
float library_log10_f(const float x) { return std::log10(x); }

void library_atan2_double(BenchBuffers& b) {
    const std::size_t count = b.x.size();
    for (std::size_t i = 0; i < count; ++i) b.s[i] = std::atan2(b.x[i], b.c[i]);
}

void owned_atan2_double(BenchBuffers& b) {
    const std::size_t count = b.x.size();
    for (std::size_t i = 0; i < count; ++i) b.s[i] = stx_auxlib::math::atan2(b.x[i], b.c[i]);
}

void library_atan2_float(BenchBuffers& b) {
    const std::size_t count = b.xf.size();
    for (std::size_t i = 0; i < count; ++i) b.sf[i] = std::atan2(b.xf[i], b.cf[i]);
}

void owned_atan2_float(BenchBuffers& b) {
    const std::size_t count = b.xf.size();
    for (std::size_t i = 0; i < count; ++i) b.sf[i] = stx_auxlib::math::atan2(b.xf[i], b.cf[i]);
}

const Region signed_ten[] = {{"[-10, 10]", -10.0, 10.0}};
const Region unit_interval[] = {{"[-1, 1]", -1.0, 1.0}};
const Region open_unit[] = {{"[-0.99, 0.99]", -0.99, 0.99}};
const Region tan_region[] = {{"[-1.5, 1.5]", -1.5, 1.5}};
const Region cbrt_region[] = {{"[0, 100]", 0.0, 100.0}};
const Region hyperbolic_region[] = {{"[-5, 5]", -5.0, 5.0}};
const Region tanh_region[] = {{"[-3, 3]", -3.0, 3.0}};
const Region acosh_region[] = {{"[1, 10]", 1.0, 10.0}};
const Region positive_region[] = {{"[0.5, 2]", 0.5, 2.0}, {"[1e-3, 1e3]", 1e-3, 1e3}};

double bench() {
    const BenchSet sets[] = {
        {"sincos (sin + cos pair)", library_double, owned_double, library_float, owned_float,
         double_regions, sizeof(double_regions) / sizeof(double_regions[0])},
        {"exp", library_exp_double, owned_exp_double, library_exp_float, owned_exp_float,
         exp_regions, sizeof(exp_regions) / sizeof(exp_regions[0])},
        {"log", library_log_double, owned_log_double, library_log_float, owned_log_float,
         log_bench_regions, sizeof(log_bench_regions) / sizeof(log_bench_regions[0])},
        {"log2", unary_double<library_log2_d>, unary_double<owned_log2_d>, unary_float<library_log2_f>,
         unary_float<owned_log2_f>, positive_region, 2},
        {"log10", unary_double<library_log10_d>, unary_double<owned_log10_d>,
         unary_float<library_log10_f>, unary_float<owned_log10_f>, positive_region, 2},
        {"pow (y in [-8, 8])", library_pow_double, owned_pow_double, library_pow_float,
         owned_pow_float, pow_bench_regions, sizeof(pow_bench_regions) / sizeof(pow_bench_regions[0])},
        {"hypot (second operand in [-8, 8])", library_hypot_double, owned_hypot_double,
         library_hypot_float, owned_hypot_float, pow_bench_regions,
         sizeof(pow_bench_regions) / sizeof(pow_bench_regions[0])},
        {"atan", unary_double<library_atan_d>, unary_double<owned_atan_d>, unary_float<library_atan_f>,
         unary_float<owned_atan_f>, signed_ten, 1},
        {"atan2 (x in [-8, 8])", library_atan2_double, owned_atan2_double, library_atan2_float,
         owned_atan2_float, signed_ten, 1},
        {"asin", unary_double<library_asin_d>, unary_double<owned_asin_d>, unary_float<library_asin_f>,
         unary_float<owned_asin_f>, unit_interval, 1},
        {"acos", unary_double<library_acos_d>, unary_double<owned_acos_d>, unary_float<library_acos_f>,
         unary_float<owned_acos_f>, unit_interval, 1},
        {"tan", unary_double<library_tan_d>, unary_double<owned_tan_d>, unary_float<library_tan_f>,
         unary_float<owned_tan_f>, tan_region, 1},
        {"cbrt", unary_double<library_cbrt_d>, unary_double<owned_cbrt_d>, unary_float<library_cbrt_f>,
         unary_float<owned_cbrt_f>, cbrt_region, 1},
        {"sinh", unary_double<library_sinh_d>, unary_double<owned_sinh_d>, unary_float<library_sinh_f>,
         unary_float<owned_sinh_f>, hyperbolic_region, 1},
        {"cosh", unary_double<library_cosh_d>, unary_double<owned_cosh_d>, unary_float<library_cosh_f>,
         unary_float<owned_cosh_f>, hyperbolic_region, 1},
        {"tanh", unary_double<library_tanh_d>, unary_double<owned_tanh_d>, unary_float<library_tanh_f>,
         unary_float<owned_tanh_f>, tanh_region, 1},
        {"asinh", unary_double<library_asinh_d>, unary_double<owned_asinh_d>,
         unary_float<library_asinh_f>, unary_float<owned_asinh_f>, signed_ten, 1},
        {"acosh", unary_double<library_acosh_d>, unary_double<owned_acosh_d>,
         unary_float<library_acosh_f>, unary_float<owned_acosh_f>, acosh_region, 1},
        {"atanh", unary_double<library_atanh_d>, unary_double<owned_atanh_d>,
         unary_float<library_atanh_f>, unary_float<owned_atanh_f>, open_unit, 1},
    };
    double worst = 0.0;
    for (const BenchSet& set : sets) worst = std::max(worst, bench_set(set));
    std::printf("\nworst gated ratio %.2f (ceiling %.1f)\n", worst, speed_ceiling);
    return worst;
}

} // namespace

int main(const int argument_count, char** const arguments) {
    const bool exhaustive = argument_count == 2 && std::strcmp(arguments[1], "--exhaustive") == 0;
    if (argument_count == 2 && std::strcmp(arguments[1], "--bench") == 0) {
        (void)bench();
        return 0;
    }
    if (argument_count == 2 && std::strcmp(arguments[1], "--bench-gate") == 0) {
        const double worst = bench();
        if (worst > speed_ceiling) {
            std::fprintf(stderr, "math speed gate failed: %.2fx the C library\n", worst);
            return 1;
        }
        std::puts("math speed gate passed");
        return 0;
    }
    try {
        check_reference();
        special_values();
        determinism();
        sincos_certified_worst_cases();
        double_accuracy(exhaustive ? 20000000 : 200000);
        sincos_hard_reduction(exhaustive ? 20000000 : 200000);
        float_rounding(exhaustive ? 1 : 97);
        exp_check_reference();
        exp_special_values();
        exp_determinism();
        exp_double_accuracy(exhaustive ? 20000000 : 200000);
        exp_double_outside(exhaustive ? 20000000 : 200000);
        exp_float_rounding(exhaustive ? 1 : 97);
        const std::vector<LogKernel> logs = log_kernels();
        log_special_values(logs);
        log_determinism(logs);
        log_double_accuracy(logs, exhaustive ? 20000000 : 100000);
        log_float_rounding(logs, exhaustive ? 1 : 997);
        pow_special_values();
        pow_determinism();
        pow_double_accuracy(exhaustive ? 20000000 : 100000);
        pow_float_rounding(exhaustive ? 100000000 : 200000);
        hypot_special_values();
        hypot_determinism();
        hypot_double_accuracy(exhaustive ? 20000000 : 100000);
        hypot_float_rounding(exhaustive ? 100000000 : 300000);
        inverse_trig_special_values();
        inverse_trig_determinism();
        inverse_trig_double_accuracy(exhaustive ? 20000000 : 100000);
        inverse_trig_float_rounding(exhaustive ? 1 : 997);
        remaining_special_values();
        remaining_determinism();
        remaining_double_accuracy(exhaustive ? 5000000 : 50000);
        remaining_float_rounding(exhaustive ? 1 : 1999);
    } catch (const std::exception& failure) {
        std::fprintf(stderr, "math tests failed: %s\n", failure.what());
        return 1;
    }
    std::puts("math tests passed");
    return 0;
}
