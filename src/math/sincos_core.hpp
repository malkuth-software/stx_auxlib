#pragma once

// The sin/cos core shared by sincos (sincos.cpp) and tan (tan.cpp). Private
// to the library; see kernel_support.hpp for the floating-point rules.
//
// The successor to bfft's bruun_table256_poly3_sincos: sin and cos at the
// nearest of 256 points k*pi/128, rotated by r = x - k*pi/128 (|r| <= pi/256
// plus a rounding margin) through short Taylor polynomials.

#include "constants.hpp"
#include "kernel_support.hpp"
#include "sincos_table256.hpp"

STX_AUXLIB_MATH_TARGET_PUSH

namespace stx_auxlib::math::STX_AUXLIB_MATH_VARIANT::detail {

inline constexpr double sincos_inverse_step = 0x1.45f306dc9c883p+5;  // 128/pi, rounded
inline constexpr double sincos_fast_limit = 0x1p20;        // keeps |q| < 2^26 (exact q * step parts)
inline constexpr double sincos_tiny_limit = 0x1p-27;       // below it, x^3/6 < ulp(x)/2
inline constexpr std::uint64_t sincos_index_mask = 255;
inline constexpr std::uint64_t sincos_tiny_bits = std::bit_cast<std::uint64_t>(sincos_tiny_limit);

inline constexpr double sin_3 = -1.0 / 6.0;
inline constexpr double sin_5 = 1.0 / 120.0;
inline constexpr double sin_7 = -1.0 / 5040.0;
inline constexpr double cos_half = 0.5;  // r^2 * cos_half is exact
inline constexpr double cos_4 = 1.0 / 24.0;
inline constexpr double cos_6 = -1.0 / 720.0;

// The table's sines and cosines split for Dekker's product, built at
// compile time (two_product_split).
struct SinCosSplit final {
    Split s;
    Split c;
};

struct SinCosSplits final {
    SinCosSplit values[256];
};

constexpr SinCosSplits make_sincos_splits() noexcept {
    SinCosSplits table{};
    for (int i = 0; i < 256; ++i) {
        table.values[i].s = veltkamp_split(sincos_table_256[i].s);
        table.values[i].c = veltkamp_split(sincos_table_256[i].c);
    }
    return table;
}

alignas(64) inline constexpr SinCosSplits sincos_splits = make_sincos_splits();

// True when tiny_limit <= |x| <= fast_limit, false for NaN: compares on the
// magnitude bits, which order like the magnitudes.
inline bool sincos_fast_path(const double x) noexcept {
    const std::uint64_t magnitude = std::bit_cast<std::uint64_t>(x) & magnitude_mask;
    const bool result = magnitude >= sincos_tiny_bits &&
                        magnitude <= std::bit_cast<std::uint64_t>(sincos_fast_limit);
    return result;
}

// q, the integer nearest x * 128/pi (to within one, which only widens |r|
// slightly), and q mod 256. Requires |x| <= 2^32 (x * 128/pi < 2^51).
inline double sincos_step(const double x, std::uint64_t& index) noexcept {
    const double shifted = x * kernel_constants.sincos_inverse_step + kernel_constants.rounding_shift;
    // The shift leaves q in the low mantissa bits, two's complement for q < 0.
    index = std::bit_cast<std::uint64_t>(shifted) & sincos_index_mask;
    const double q = shifted - kernel_constants.rounding_shift;
    return q;
}

// r = x - q*pi/128 rounded to double (float kernel). x - q * step_1 is
// exact: both products are exact and the difference is within Sterbenz's
// range.
inline double sincos_remainder(const double x, const double q) noexcept {
    const double r1 = x - q * kernel_constants.sincos_step_1;
    const double r2 = r1 - q * kernel_constants.sincos_step_2;
    const double r = r2 - q * kernel_constants.sincos_step_3;
    return r;
}

// r = x - q*pi/128 as r + r_lo with |r_lo| <= ulp(r)/2 (double kernel). The
// renormalizing two-sum is required: unnormalized, r_lo reaches ~4e-12 at
// |x| = 2^20 and its cross terms with r exceed an ulp of the result
// (measured: 472 ulp).
inline void sincos_remainder_wide(const double x, const double q, double& r,
                                                  double& r_lo) noexcept {
    const double r1 = x - q * kernel_constants.sincos_step_1;
    double r2 = 0.0;
    double r2_error = 0.0;
    two_sum(r1, -(q * kernel_constants.sincos_step_2), r2, r2_error);
    const double low = r2_error - q * kernel_constants.sincos_step_3;
    two_sum(r2, low, r, r_lo);
}

// sin x = s_hi + s_lo and cos x = c_hi + c_lo from the table index and the
// remainder r + r_lo (|r_lo| <= ulp(r)/2), each pair unrounded (~1e-21
// relative).
//
// With S, C the table's sin and cos at k*pi/128 and r = x - k*pi/128:
//   sin x = S + C*r + [C*(sin r - r) + S*(cos r - 1) + S_lo + C_lo*r]
//   cos x = C - S*r + [-S*(sin r - r) + C*(cos r - 1) + C_lo - S_lo*r]
// The leading two terms are summed exactly (two_product, two_sum); the
// bracket is ~r^2 times the result and its rounding stays far below an ulp.
inline void sincos_rotate(const std::uint64_t index, const double r, const double r_lo,
                          double& s_hi, double& s_lo, double& c_hi, double& c_lo) noexcept {
    const double r2 = r * r;
    const double sin_r_minus_r =
        r_lo + r * (r2 * (kernel_constants.sin_3 + r2 * (kernel_constants.sin_5 + r2 * kernel_constants.sin_7)));
    const double cos_r_minus_1 =
        (r2 * r2) * (kernel_constants.cos_4 + r2 * kernel_constants.cos_6) - r2 * cos_half;
    const SinCosEntry& entry = sincos_table_256[index];

    double sin_rotation = 0.0;
    double sin_rotation_error = 0.0;
    const SinCosSplit& parts = sincos_splits.values[index];
    two_product_split(entry.c, parts.c, r, sin_rotation, sin_rotation_error);
    double sin_head = 0.0;
    double sin_head_error = 0.0;
    // |S| >= sin(pi/128) > |C r| except where S = 0: a fast two-sum. The same
    // holds for C against S r below.
    fast_two_sum(entry.s, sin_rotation, sin_head, sin_head_error);
    const double sin_small = entry.c * sin_r_minus_r + entry.s * cos_r_minus_1 +
                             (entry.s_lo + entry.c_lo * r);
    const double sin_tail = sin_head_error + (sin_rotation_error + sin_small);

    double cos_rotation = 0.0;
    double cos_rotation_error = 0.0;
    two_product_split(entry.s, parts.s, r, cos_rotation, cos_rotation_error);
    double cos_head = 0.0;
    double cos_head_error = 0.0;
    fast_two_sum(entry.c, -cos_rotation, cos_head, cos_head_error);
    const double cos_small = entry.c * cos_r_minus_1 - entry.s * sin_r_minus_r +
                             (entry.c_lo - entry.s_lo * r);
    const double cos_tail = cos_head_error + (cos_small - cos_rotation_error);

    s_hi = sin_head;
    s_lo = sin_tail;
    c_hi = cos_head;
    c_lo = cos_tail;
}

// ---- medium range: 2^20 < |x| <= 2^32 ------------------------------------------------
//
// Arguments above 2^32 are refused (NaN): no GUI.Forms use needs them, and
// a phase that grows without bound must reset itself. Up to 2^32, |q| <
// 2^38 and pi/128 in eight 15-bit parts plus a rest (~185 bits) keeps every
// q * part exact; its error against the contract below is proved, not
// sampled (tools/sincos_certificate.py). The fast path's small remainders
// use the same reduction.

inline constexpr double sincos_medium_limit = 0x1p32;
inline constexpr std::uint64_t sincos_medium_low_bits = std::bit_cast<std::uint64_t>(sincos_fast_limit);

// True when fast_limit < |x| <= medium_limit, false for NaN.
inline bool sincos_medium_path(const double x) noexcept {
    const std::uint64_t magnitude = std::bit_cast<std::uint64_t>(x) & magnitude_mask;
    const bool result = magnitude > sincos_medium_low_bits &&
                        magnitude <= std::bit_cast<std::uint64_t>(sincos_medium_limit);
    return result;
}

// r = x - q*pi/128 as r + r_lo with |r_lo| <= ulp(r)/2, for the medium
// range and the fast path's small remainders. x - q * part[0] is exact
// (the certificate checks it by bit count and granularity); each later
// subtraction keeps its rounding error by two-sum, and the errors join the
// low part, whose own roundings are the only ones left besides q * rest.
inline void sincos_remainder_medium(const double x, const double q, double& r,
                                    double& r_lo) noexcept {
    double head = x - q * sincos_medium_step[0];
    double low = 0.0;
    for (int part = 1; part < 8; ++part) {
        double sum = 0.0;
        double sum_error = 0.0;
        two_sum(head, -(q * sincos_medium_step[part]), sum, sum_error);
        head = sum;
        low += sum_error;
    }
    low -= q * sincos_medium_step_rest;
    two_sum(head, low, r, r_lo);
}

// The reduction's contract and the small-remainder path. Proved by
// tools/sincos_certificate.py, which reads this file's constants and
// sincos_table256.hpp's:
//
// - The rotation (sincos_rotate, every rounding and truncation counted by
//   interval error analysis over all 256 indices) is within 0.00110 ulp of
//   sin and cos, and tan's quotient within 0.00536 ulp, beyond the final
//   rounding, for exact r + r_lo. Against the stated bound of 0.51 ulp
//   (math.hpp) that leaves the reduction |delta| <= min(2^-59.86 |r|,
//   2^-68.45) absolute: the first where sin, cos or tan vanishes at r = 0,
//   the second near tan's poles elsewhere.
// - The smallest |x - q pi/128| over every double 2^-27 <= |x| <= 2^32 with
//   q != 0 is 2^-66.49 (computed per binade and residue class over every
//   q, not sampled). Meeting the contract there needs pi/128 to 150 bits.
// - The three-part split above holds pi/128 to ~133 bits and errs by at
//   most 2^-89.88 for |x| <= 2^20: within the contract once |r| >= 2^-30
//   (2^-30 is the smallest power of two for which it is).
// - Below that, the eight-part split of the medium range (~185 bits, q *
//   part exact for |q| < 2^38, which covers |x| <= 2^20 too) meets the
//   contract at every binade's certified minimum.
inline constexpr double sincos_small_remainder_limit = 0x1p-30;

// q and r = x - q pi/128 as r + r_lo for the fast path (2^-27 <= |x| <=
// 2^20), within the contract above: the three-part reduction, redone with
// the eight parts when |r| < sincos_small_remainder_limit (about one input
// in 2^24, where x lies within 2^-30 of a multiple of pi/128).
inline double sincos_reduce_fast(const double x, std::uint64_t& index, double& r, double& r_lo) noexcept {
    const double q = sincos_step(x, index);
    sincos_remainder_wide(x, q, r, r_lo);
    if (std::fabs(r) < sincos_small_remainder_limit) {
        sincos_remainder_medium(x, q, r, r_lo);
    }
    return q;
}

// sincos_rotate for x on the fast path (2^-27 <= |x| <= 2^20).
inline void sincos_wide(const double x, double& s_hi, double& s_lo, double& c_hi,
                        double& c_lo) noexcept {
    std::uint64_t index = 0;
    double r = 0.0;
    double r_lo = 0.0;
    (void)sincos_reduce_fast(x, index, r, r_lo);
    sincos_rotate(index, r, r_lo, s_hi, s_lo, c_hi, c_lo);
}

// sincos_rotate for x in the medium range.
inline void sincos_wide_medium(const double x, double& s_hi, double& s_lo, double& c_hi,
                               double& c_lo) noexcept {
    std::uint64_t index = 0;
    const double q = sincos_step(x, index);
    double r = 0.0;
    double r_lo = 0.0;
    sincos_remainder_medium(x, q, r, r_lo);
    sincos_rotate(index, r, r_lo, s_hi, s_lo, c_hi, c_lo);
}

} // namespace stx_auxlib::math::STX_AUXLIB_MATH_VARIANT::detail

STX_AUXLIB_MATH_TARGET_POP
