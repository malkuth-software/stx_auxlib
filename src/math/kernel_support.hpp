#pragma once

// Shared pieces of the transcendental kernels in src/math/. Private to the
// library: these files compile only with stx_auxlib's own flags, which
// switch floating-point contraction off (-ffp-contract=off, /fp:precise),
// so every operation is rounded as written. The pragma repeats that for
// clang; GCC does not implement it (and warns), so the flag governs there.

#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#endif

#include <bit>
#include <cmath>
#include <cstdint>

// Marks an accurate fallback: kept out of line (and out of the hot code's
// layout) so that the fast path calling it in tail position needs no stack
// frame. Inlined, the fallback made every call save registers it rarely
// used (measured: ~2 ns of a 4 ns float log).
#if defined(__GNUC__) || defined(__clang__)
#define STX_AUXLIB_MATH_COLD __attribute__((noinline, cold))
#else
#define STX_AUXLIB_MATH_COLD
#endif

namespace stx_auxlib::math::detail {

constexpr std::uint64_t magnitude_mask = 0x7fffffffffffffffULL;
// x + rounding_shift - rounding_shift rounds x to an integer for |x| < 2^51,
// leaving that integer in the low mantissa bits of the sum.
constexpr double rounding_shift = 0x1.8p52;
constexpr std::int64_t exponent_bias = 1023;

// Sum and its exact rounding error (Knuth's two-sum).
inline void two_sum(const double a, const double b, double& sum,
                                    double& error) noexcept {
    sum = a + b;
    const double b_part = sum - a;
    const double a_part = sum - b_part;
    error = (a - a_part) + (b - b_part);
}

// Sum and its exact rounding error when |a| >= |b| or a == 0 (Dekker's fast
// two-sum, three operations instead of six). Callers state why the order
// holds; with a == 0 it returns (b, 0).
inline void fast_two_sum(const double a, const double b, double& sum, double& error) noexcept {
    sum = a + b;
    error = b - (sum - a);
}

// a * b + c for certified cheap paths only: a fused multiply-add where the
// hardware has one (one rounding, shorter dependency chains), a multiply
// and an add otherwise (two roundings). The value can differ between those
// builds, but cheap paths return a result only after float_if_certain or
// rounds_safely proves it the correctly rounded one -- the same value in
// either build -- and their error bounds already allow two roundings. Never
// use it where a result is returned uncertified. STX_AUXLIB_MATH_DEKKER
// selects the multiply-and-add form, so the Dekker test build checks that
// both produce the same bits.
inline double multiply_add(const double a, const double b, const double c) noexcept {
#if !defined(STX_AUXLIB_MATH_DEKKER) && \
    (defined(__FP_FAST_FMA) || defined(__ARM_FEATURE_FMA) || defined(__FMA__))
    const double result = std::fma(a, b, c);
#else
    const double result = a * b + c;
#endif
    return result;
}

// Product and its exact rounding error. Exact either way, so the fma and
// Dekker forms return the same bits; STX_AUXLIB_MATH_DEKKER forces Dekker's
// (tests compare the two). Requires |a|, |b| <= 2^995 (no split overflow).
inline void two_product(const double a, const double b, double& product,
                                        double& error) noexcept {
    product = a * b;
#if !defined(STX_AUXLIB_MATH_DEKKER) && \
    (defined(__FP_FAST_FMA) || defined(__ARM_FEATURE_FMA) || defined(__FMA__))
    error = std::fma(a, b, -product);
#else
    // Veltkamp split into 26-bit halves.
    constexpr double split = 134217729.0;  // 2^27 + 1
    const double a_scaled = split * a;
    const double a_hi = a_scaled - (a_scaled - a);
    const double a_lo = a - a_hi;
    const double b_scaled = split * b;
    const double b_hi = b_scaled - (b_scaled - b);
    const double b_lo = b - b_hi;
    error = ((a_hi * b_hi - product) + a_hi * b_lo + a_lo * b_hi) + a_lo * b_lo;
#endif
}

// 2^exponent for exponent in [-1022, 1023].
inline double power_of_two(const std::int64_t exponent) noexcept {
    const std::uint64_t bits = static_cast<std::uint64_t>(exponent + exponent_bias) << 52;
    const double result = std::bit_cast<double>(bits);
    return result;
}

// A double-double value hi + lo.
struct Wide final {
    double hi{0.0};
    double lo{0.0};
};

// (n_hi + n_lo) / (d_hi + d_lo) as a double-double: the double quotient and
// one correction from the exact residual (n_hi - q * d_hi is exact because
// q * d_hi is within an ulp of n_hi).
inline Wide divide(const double n_hi, const double n_lo, const double d_hi, const double d_lo) noexcept {
    const double q = n_hi / d_hi;
    double product = 0.0;
    double product_error = 0.0;
    two_product(q, d_hi, product, product_error);
    const double residual = (((n_hi - product) - product_error) + n_lo) - q * d_lo;
    const Wide result{q, residual / d_hi};
    return result;
}

inline constexpr double smallest_normal = 0x1p-1022;

// value_hi + value_lo rounded once to float (round to nearest, ties to
// even), for a finite sum. The pair need not be normalized (kernels hand
// over head + tail with tails up to ~1e-7 of the head); it is renormalized
// first. Then round-to-odd: the exact value lies between hi and its
// neighbour on lo's side, and choosing whichever of the two has an odd last
// bit gives a double that rounds to the same float as the exact value
// (double has more than float precision + 1 bits), so one cast finishes.
// Rounding hi to float and then adding lo could round twice.
inline float round_wide_to_float(const double value_hi, const double value_lo) noexcept {
    double hi = 0.0;
    double lo = 0.0;
    two_sum(value_hi, value_lo, hi, lo);
    // Branch-free: the adjustment depends on the end of the kernel's
    // dependency chain, and a branch on it mispredicts half the time; each
    // miss then waits for the whole chain (measured: +28 ns for cbrt).
    // adjust is 1 when hi is even and lo nonzero; the step goes toward lo,
    // which increases the magnitude when lo has hi's sign. hi is even and
    // nonzero whenever adjust is 1 (lo != 0 requires hi != 0), so neither
    // the increment nor the decrement can wrap.
    const std::uint64_t bits = std::bit_cast<std::uint64_t>(hi);
    const std::uint64_t inexact = static_cast<std::uint64_t>(lo != 0.0);
    const std::uint64_t adjust = inexact & ((bits & 1) ^ 1);
    const std::uint64_t outward = static_cast<std::uint64_t>((lo > 0.0) == (hi > 0.0));
    const std::uint64_t odd_bits = (bits + (adjust & outward)) - (adjust & (outward ^ 1));
    const float result = static_cast<float>(std::bit_cast<double>(odd_bits));
    return result;
}

// The float nearest `approx`, when the exact value -- within `error`
// (absolute) of approx -- is certain to round to that same float. Float
// kernels compute a cheap double result, certify it here, and fall back to
// their accurate double-double path only when this fails.
//
// Rounding is monotonic, so when both ends of [approx - error, approx +
// error] round to the same float, every value between them does too. The
// ends are themselves rounded to double, which can pull each inward by half
// a double ulp; 2^-52 of |approx| more covers that. The float bits are
// compared, so +0 and -0 differ; infinities (from either end overflowing)
// make the ends differ unless both overflow, which callers rule out by
// their own range tests.
inline bool float_if_certain(const double approx, const double error, float& result) noexcept {
    const double slack = error + std::fabs(approx) * 0x1p-52;
    const float low = static_cast<float>(approx - slack);
    const float high = static_cast<float>(approx + slack);
    if (std::bit_cast<std::uint32_t>(low) != std::bit_cast<std::uint32_t>(high)) return false;
    result = low;
    return true;
}

// (head + tail) * 2^exponent rounded once, for head in [0.5, 4) with |tail|
// below an ulp of head and exponent in [-1080, 1024]: overflow to infinity,
// normal results, and subnormal results rounded once on the subnormal grid.
inline double scale_wide(const double head, const double tail, const std::int64_t exponent) noexcept {
    // The common case first, as one unsigned compare (exponent in
    // [-1021, 1022]): 2^exponent is normal and y * 2^exponent is exact or
    // overflows correctly. Testing the exponent's sign first mispredicted
    // half the time for results on both sides of 1 (measured: +4 ns).
    const std::uint64_t normal_offset = static_cast<std::uint64_t>(exponent + 1021);
    if (normal_offset <= 2043) {
        const double y = head + tail;
        const double result = y * power_of_two(exponent);
        return result;
    }
    if (exponent > 0) {
        // exponent reaches 1024 here: scale in two steps, so a result above
        // DBL_MAX rounds to infinity once, from the correctly rounded y.
        const double y = head + tail;
        const double result = (y * power_of_two(exponent - 1)) * 2.0;
        return result;
    }
    if (exponent > -1022) {
        const double y = head + tail;
        const double result = y * power_of_two(exponent);
        return result;
    }
    // The result is below 2^-1022 or within a factor of four of it. Round
    // once on the subnormal grid: in units of 2^-1022 the value is P + Q, and
    // when it is below 1, adding 1 puts its rounding at 2^-52, which is
    // exactly the subnormal ulp 2^-1074. head * 2^(exponent + 1022) is exact
    // (exponent >= -1080).
    const double scale = power_of_two(exponent + 1022);
    const double p = head * scale;
    const double q = tail * scale;
    double u = 0.0;
    double u_error = 0.0;
    two_sum(1.0, p, u, u_error);
    const double w = u + (u_error + q);
    if (w >= 2.0) {
        // Normal after all (P + Q >= 1): the scaling is exact.
        const double y = head + tail;
        const double result = y * power_of_two(exponent);
        return result;
    }
    const double result = (w - 1.0) * smallest_normal;  // both steps exact
    return result;
}

} // namespace stx_auxlib::math::detail
