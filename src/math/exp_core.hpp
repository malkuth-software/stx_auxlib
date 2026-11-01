#pragma once

// The exp core shared by exp (exp.cpp) and pow (pow.cpp). Private to the
// library; see kernel_support.hpp for the floating-point rules.
//
// e^x = 2^k * 2^(j/256) * e^r with x = (256k + j) * ln2/256 + r,
// |r| <= ln2/512 plus a rounding margin: the table holds 2^(j/256) as hi + lo,
// e^r comes from a short Taylor polynomial, and 2^k is applied exactly.

#include "exp_table256.hpp"
#include "constants.hpp"
#include "kernel_support.hpp"

namespace stx_auxlib::math::detail {

inline constexpr double exp_inverse_step = 0x1.71547652b82fep+8;  // 256/ln2, rounded
// 708 keeps k in [-1022, 1021], so 2^k and the result are normal doubles.
inline constexpr double exp_fast_limit = 708.0;
inline constexpr std::uint64_t exp_fast_bits = std::bit_cast<std::uint64_t>(exp_fast_limit);
inline constexpr std::uint64_t exp_index_mask = 255;
// Above it e^x rounds to infinity (ln(DBL_MAX) = 709.78271289338400).
inline constexpr double exp_overflow_limit = 709.79;
// Below it e^x < 2^-1075 rounds to +0 (ln(2^-1075) = -745.13321910194122).
inline constexpr double exp_underflow_limit = -746.0;

inline constexpr double exp_2 = 0.5;
inline constexpr double exp_3 = 1.0 / 6.0;
inline constexpr double exp_4 = 1.0 / 24.0;
inline constexpr double exp_5 = 1.0 / 120.0;
inline constexpr double exp_6 = 1.0 / 720.0;

// True when |x| <= exp_fast_limit, false for NaN.
inline bool exp_fast_path(const double x) noexcept {
    const std::uint64_t magnitude = std::bit_cast<std::uint64_t>(x) & magnitude_mask;
    const bool result = magnitude <= exp_fast_bits;
    return result;
}

// q = 256k + j, the integer nearest x * 256/ln2 (to within one), as a
// double, with j and k. Requires |x| <= 746 (|q| < 2^19).
inline double exp_step(const double x, std::uint64_t& index,
                                       std::int64_t& exponent) noexcept {
    const double shifted = x * kernel_constants.exp_inverse_step + kernel_constants.rounding_shift;
    // The shift leaves q in the low mantissa bits: their difference from the
    // shift's own bits is q, negative values included.
    const std::int64_t q_bits = std::bit_cast<std::int64_t>(shifted) -
                                std::bit_cast<std::int64_t>(kernel_constants.rounding_shift);
    index = static_cast<std::uint64_t>(q_bits) & exp_index_mask;
    exponent = q_bits >> 8;  // floor(q / 256): an arithmetic shift in C++20
    const double q = shifted - kernel_constants.rounding_shift;
    return q;
}

// e^(x + x_lo) = (head + tail) * 2^exponent for |x| <= 746, with head + tail
// carrying ~1e-21 relative error and |tail| below an ulp of head. x_lo is a
// low part below ulp(x) (0 for exp itself; pow passes y * log x as x + x_lo).
//
// With T = 2^(j/256) = T_hi + T_lo and r = x - q*ln2/256 as r + r_lo:
//   e^x / 2^k = T_hi + T_hi*r + [T_hi*(e^r - 1 - r + r_lo) + T_lo*(1 + r)]
// The leading two terms are summed exactly (two_product, two_sum); the
// bracket is ~r^2 times the result.
inline void exp_core(const double x, const double x_lo, double& head, double& tail,
                     std::int64_t& exponent) noexcept {
    std::uint64_t index = 0;
    const double q = exp_step(x, index, exponent);
    const double r1 = x - q * kernel_constants.exp_step_1;
    double r2 = 0.0;
    double r2_error = 0.0;
    two_sum(r1, -(q * kernel_constants.exp_step_2), r2, r2_error);
    // x_lo joins the low part; the renormalization below keeps |r_lo| under
    // ulp(r)/2, which the polynomial's omitted r * r_lo term needs.
    const double low = (r2_error - q * kernel_constants.exp_step_3) + x_lo;
    double r = 0.0;
    double r_lo = 0.0;
    two_sum(r2, low, r, r_lo);
    // e^r - 1 - r to ~1e-21 (r^7/5040 truncation).
    const double polynomial_tail =
        (r * r) * (exp_2 + r * (kernel_constants.exp_3 + r * (kernel_constants.exp_4 + r * (kernel_constants.exp_5 + r * kernel_constants.exp_6))));
    const ExpEntry& entry = exp_table_256[index];

    double rotation = 0.0;
    double rotation_error = 0.0;
    two_product(entry.hi, r, rotation, rotation_error);
    double head_error = 0.0;
    fast_two_sum(entry.hi, rotation, head, head_error);  // |T| >= 1 > |T r|
    const double small = entry.hi * (polynomial_tail + r_lo) + (entry.lo + entry.lo * r);
    tail = head_error + (rotation_error + small);
}

// e^a and e^-a together for 0 <= a <= 37, each as an unnormalized
// double-double value (normal throughout), for sinh and cosh. One reduction
// and one polynomial serve both: with a = q ln2/256 + r and q = 256k + j,
//   e^-a = 2^(-k) 2^(-j/256) e^(-r),   2^(-j/256) = T[256 - j] / 2 (j > 0),
// from the same table, and e^(-r) - 1 + r = even(r) - odd(r) of the same
// polynomial, so no division is needed (computing 1/e^a cost a division and
// a double-double correction).
inline void exp_pair(const double a, Wide& plus, Wide& minus) noexcept {
    std::uint64_t index = 0;
    std::int64_t exponent = 0;
    const double q = exp_step(a, index, exponent);
    const double r1 = a - q * kernel_constants.exp_step_1;
    double r2 = 0.0;
    double r2_error = 0.0;
    two_sum(r1, -(q * kernel_constants.exp_step_2), r2, r2_error);
    const double low = r2_error - q * kernel_constants.exp_step_3;
    double r = 0.0;
    double r_lo = 0.0;
    two_sum(r2, low, r, r_lo);
    const double r_squared = r * r;
    // e^r - 1 - r = even + odd; e^-r - 1 + r = even - odd (to r^6).
    const double even = r_squared * (exp_2 + r_squared * (kernel_constants.exp_4 + r_squared * kernel_constants.exp_6));
    const double odd = (r_squared * r) * (kernel_constants.exp_3 + r_squared * kernel_constants.exp_5);

    const ExpEntry& up = exp_table_256[index];
    double up_rotation = 0.0;
    double up_rotation_error = 0.0;
    two_product(up.hi, r, up_rotation, up_rotation_error);
    double up_head = 0.0;
    double up_head_error = 0.0;
    fast_two_sum(up.hi, up_rotation, up_head, up_head_error);  // |T| >= 1 > |T r|
    const double up_small = up.hi * ((even + odd) + r_lo) + (up.lo + up.lo * r);
    const double up_tail = up_head_error + (up_rotation_error + up_small);
    const double up_scale = power_of_two(exponent);
    plus = Wide{up_head * up_scale, up_tail * up_scale};

    // j = 0 maps to itself with exponent -k; otherwise to 256 - j with -k - 1.
    const bool whole = index == 0;
    const std::uint64_t down_index = whole ? 0 : 256 - index;
    const std::int64_t down_exponent = whole ? -exponent : -exponent - 1;
    const ExpEntry& down = exp_table_256[down_index];
    double down_rotation = 0.0;
    double down_rotation_error = 0.0;
    two_product(down.hi, -r, down_rotation, down_rotation_error);
    double down_head = 0.0;
    double down_head_error = 0.0;
    fast_two_sum(down.hi, down_rotation, down_head, down_head_error);
    const double down_small = down.hi * ((even - odd) - r_lo) + (down.lo - down.lo * r);
    const double down_tail = down_head_error + (down_rotation_error + down_small);
    const double down_scale = power_of_two(down_exponent);
    minus = Wide{down_head * down_scale, down_tail * down_scale};
}

// e^x for |x| <= 708 in plain double, within ~2^-51 relative: the float
// kernel (exhaustively checked correctly rounded after one rounding to
// float) and float pow's cheap path.
inline double exp_cheap(const double x) noexcept {
    std::uint64_t index = 0;
    std::int64_t exponent = 0;
    const double q = exp_step(x, index, exponent);
    const double r1 = x - q * kernel_constants.exp_step_1;
    const double r2 = r1 - q * kernel_constants.exp_step_2;
    const double r = r2 - q * kernel_constants.exp_step_3;
    // e^r - 1 to ~4e-17 (r^5/120 truncation).
    const double e_r_minus_1 = r + (r * r) * (exp_2 + r * (kernel_constants.exp_3 + r * kernel_constants.exp_4));
    const ExpEntry& entry = exp_table_256[index];
    const double y = entry.hi + (entry.hi * e_r_minus_1 + entry.lo);
    const double result = y * power_of_two(exponent);
    return result;
}

} // namespace stx_auxlib::math::detail
