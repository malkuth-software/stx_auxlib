// pow (declared, with its contracts, in stx_auxlib/math.hpp).
//
// x^y = 2^(y * log2 x) for positive finite x, in one pass with no rounding
// test (the owner's log2 formulation, adopted 2026-11-02). One formulation
// on every tier: the exact products are FMA or Dekker's, with the same bits.
//
//   log2 x = (k + H_i) + s + s_lo
// with x = 2^k z, the log table's bucket i (log_core.hpp: r = z inv_i - 1,
// |r| <= 2^-8, exact as r + r_lo), H_i = -log2(inv_i) cut to a multiple of
// 2^-41 so that k + H_i is exact, s = log1p(r) / ln 2 rounded and s_lo
// carrying its error with mid_i = -log2(inv_i) - H_i. Then
//   p + p_lo = y (k + H_i) + y s + y s_lo
// from two exact products and an exact sum, and 2^(p + p_lo) =
// 2^e 2^(j/256) e^(t ln 2) with q = round(256 p) = 256 e + j and
// t = p - q/256 exact (|t| <= 2^-9), from the exp table and a degree-6
// polynomial.
//
// Error bound (dev/x86/pow_bound.py in the hot-paths record): the absolute
// error of log2 x is below 2^-66.6 |log2 x| (worst in buckets 80 and 81,
// where log2 x is smallest; bucket 80 has inv = 1 and H = 0, so x near 1
// does not cancel), which p carries times |y|: below 2^-56.2 absolute for
// |p| <= 1076; the exp side adds 2^-70.7 relative. In all, 2^-56.7 relative
// before the final rounding: within 0.5 + 0.076 ulp. MPFR measures
// 2^-68.2 and 2^-58.9 for the two (the worst result 0.5027 ulp in 2 x 10^6
// inputs over five regions). Subnormal results round once on the subnormal
// grid (scale_wide), where the same relative error is at most 0.038 of the
// subnormal ulp. Negative x with integer y takes the sign of the integer's
// parity; every other special input follows C99 Annex F.

#include "stx_auxlib/math.hpp"

#include "exp_core.hpp"
#include "log_core.hpp"
#include "log_table128.hpp"

#include <limits>

#include "variant_functions.hpp"

STX_AUXLIB_MATH_TARGET_PUSH

namespace stx_auxlib::math::STX_AUXLIB_MATH_VARIANT {

namespace detail {
namespace {

enum class IntegerKind : std::uint8_t { not_integer, odd, even };

constexpr std::uint64_t mantissa_mask = 0x000fffffffffffffULL;
constexpr std::uint64_t implicit_bit = 0x0010000000000000ULL;

// Whether finite y is an integer and, if so, its parity. |y| >= 2^53 is
// always an even integer; 0 < |y| < 1 is never an integer.
IntegerKind integer_kind(const double y) noexcept {
    if (y == 0.0) return IntegerKind::even;
    const std::uint64_t bits = std::bit_cast<std::uint64_t>(y);
    const std::int64_t exponent = static_cast<std::int64_t>((bits >> 52) & 0x7ff) - exponent_bias;
    if (exponent < 0) return IntegerKind::not_integer;
    if (exponent > 52) return IntegerKind::even;
    const std::uint64_t significand = (bits & mantissa_mask) | implicit_bit;
    const std::uint64_t fraction_bits = 52 - static_cast<std::uint64_t>(exponent);
    const std::uint64_t fraction_mask = (std::uint64_t{1} << fraction_bits) - 1;
    if ((significand & fraction_mask) != 0) return IntegerKind::not_integer;
    const bool odd = ((significand >> fraction_bits) & 1) != 0;
    if (odd) return IntegerKind::odd;
    return IntegerKind::even;
}

// 2^r = 1 + c1 r + c2 r^2 + c3 r^3 + ...: ln2, ln2^2/2, ln2^3/6.
constexpr double pow2_c1 = 0.6931471805599453;
constexpr double pow2_c2 = 0.2402265069591007;
constexpr double pow2_c3 = 0.05550410866482158;

// The pow limits, in log2 units. Above 1024.5 the result is at least
// 2^1024.49 and rounds to infinity; below -1076 it is under 2^-1075.99 and
// rounds to +0. Between them the exponent e = floor(q / 256) lies in
// [-1077, 1024], inside scale_wide's range: e = 1024 scales in two steps
// (an overflow rounds once), and results below 2^-1022 round once on the
// subnormal grid. p is +-infinity (p_error NaN) when y (k + H) overflows;
// the limits decide those before p_error is used.
constexpr double pow_overflow_limit = 1024.5;
constexpr double pow_underflow_limit = -1076.0;

// H_i + mid_i = -log2(inv_i) for each log bucket: H_i a multiple of 2^-41,
// so k + H_i is exact (|k| <= 1075 takes 11 bits above the point, H_i 41
// below), and |mid_i| <= 2^-42.
struct Log2Entry final {
    double head{0.0};
    double mid{0.0};
};

struct Log2Table final {
    Log2Entry values[128];
};

// From the log table's -log(inv) = logc_hi + logc_lo (error ~1e-32) times
// 1/ln 2 as a double-double: the product's error is near 2^-105, against
// 2^-95 for mid's own rounding. Dekker's product, written out because
// two_product is not constexpr; constant evaluation rounds as the run time
// does.
constexpr Log2Table make_log2_table() noexcept {
    Log2Table table{};
    const Split inverse_ln2_parts = veltkamp_split(log_inv_ln2_hi);
    for (int i = 0; i < 128; ++i) {
        const LogEntry& entry = log_table_128[i];
        const Split logc_parts = veltkamp_split(entry.logc_hi);
        const double product = entry.logc_hi * log_inv_ln2_hi;
        const double product_error =
            ((logc_parts.hi * inverse_ln2_parts.hi - product) + logc_parts.hi * inverse_ln2_parts.lo +
             logc_parts.lo * inverse_ln2_parts.hi) +
            logc_parts.lo * inverse_ln2_parts.lo;
        const double low = product_error + (entry.logc_hi * log_inv_ln2_lo + entry.logc_lo * log_inv_ln2_hi);
        // Round to a multiple of 2^-41: |product| * 2^41 < 2^40, so the
        // shift leaves the nearest integer.
        const double scaled = product * 0x1p41 + rounding_shift;
        const double head = (scaled - rounding_shift) * 0x1p-41;
        const double mid = (product - head) + low;  // product - head is exact
        table.values[i] = Log2Entry{head, mid};
    }
    return table;
}

alignas(64) constexpr Log2Table log2_table = make_log2_table();

// x^y for positive finite x (normal or subnormal) and finite nonzero y.
// Modular by design: the log table's offset arithmetic (see log_core.hpp).
STX_AUXLIB_MODULAR double pow_positive(const double x, const double y) noexcept {
    double scaled = x;
    double exponent_adjust = 0.0;
    if (x < 0x1p-1022) {
        scaled = x * 0x1p52;  // subnormal: exact scaling into the normal range
        exponent_adjust = 52.0;
    }
    // log2 x: x = 2^k z, r + r_lo = z inv - 1 exactly.
    const std::uint64_t bits = std::bit_cast<std::uint64_t>(scaled);
    const std::uint64_t offset = bits - log_offset_bits;
    const std::uint64_t index = (offset >> 45) & log_index_mask;
    const std::int64_t k_bits = static_cast<std::int64_t>(offset) >> 52;
    const double k = static_cast<double>(k_bits) - exponent_adjust;
    const double z = std::bit_cast<double>(bits - (offset & exponent_field));
    const LogEntry& entry = log_table_128[index];
    double product = 0.0;
    double r_lo = 0.0;
    two_product_split(entry.inv, log_splits.values[index], z, product, r_lo);
    const double r = product - 1.0;  // exact: product is within 2^-7 of 1
    // log1p(r + r_lo) = a + a_lo: r - r^2/2 exactly, the rest below r^3/3.
    double r_squared = 0.0;
    double r_squared_error = 0.0;
    two_product(r, r, r_squared, r_squared_error);
    const double cubic =
        (r * r_squared) * (kernel_constants.log_3 + r * (kernel_constants.log_4 + r * (kernel_constants.log_5 + r * (kernel_constants.log_6 + r * (kernel_constants.log_7 + r * (kernel_constants.log_8 + r * (kernel_constants.log_9 + r * kernel_constants.log_10)))))));
    double a = 0.0;
    double a_error = 0.0;
    fast_two_sum(r, -(r_squared * 0.5), a, a_error);  // |r| >= r^2/2
    const double r_lo_scaled = r_lo * (1.0 - r * (1.0 - r * (1.0 - r)));
    const double a_lo = a_error + (r_lo_scaled + (cubic - r_squared_error * 0.5));
    // s + s_lo = (a + a_lo) / ln 2 + mid.
    double s = 0.0;
    double s_error = 0.0;
    two_product(a, kernel_constants.inverse_ln2_hi, s, s_error);
    const Log2Entry& log2_entry = log2_table.values[index];
    const double s_lo = s_error + ((a * kernel_constants.inverse_ln2_lo + a_lo * kernel_constants.inverse_ln2_hi) + log2_entry.mid);
    // p + p_lo = y (k + H) + y s + y s_lo.
    const double whole = k + log2_entry.head;  // exact
    double big = 0.0;
    double big_error = 0.0;
    two_product(y, whole, big, big_error);
    double small = 0.0;
    double small_error = 0.0;
    two_product(y, s, small, small_error);
    double p = 0.0;
    double p_error = 0.0;
    two_sum(big, small, p, p_error);
    if (p > pow_overflow_limit) return std::numeric_limits<double>::infinity();
    if (p < pow_underflow_limit) return 0.0;
    const double p_lo = p_error + ((big_error + small_error) + y * s_lo);
    // q = round(256 p) = 256 e + j; t + t_lo = p + p_lo - q/256.
    const double shifted = p * 256.0 + kernel_constants.rounding_shift;
    const std::int64_t q_bits = std::bit_cast<std::int64_t>(shifted) -
                                std::bit_cast<std::int64_t>(kernel_constants.rounding_shift);
    const std::uint64_t j = static_cast<std::uint64_t>(q_bits) & exp_index_mask;
    const std::int64_t exponent = q_bits >> 8;  // floor(q / 256): an arithmetic shift in C++20
    const double t_head = p - (shifted - kernel_constants.rounding_shift) * (1.0 / 256.0);  // exact
    double t = 0.0;
    double t_lo = 0.0;
    two_sum(t_head, p_lo, t, t_lo);
    // u + u_lo = (t + t_lo) ln 2; e^u - 1 - u to u^6/720 (truncation 2^-70.7).
    double u = 0.0;
    double u_error = 0.0;
    two_product(t, kernel_constants.ln2_hi, u, u_error);
    const double u_lo = u_error + (t * kernel_constants.ln2_lo + t_lo * kernel_constants.ln2_hi);
    const double polynomial_tail =
        (u * u) * (exp_2 + u * (kernel_constants.exp_3 + u * (kernel_constants.exp_4 + u * (kernel_constants.exp_5 + u * kernel_constants.exp_6))));
    // 2^(j/256) e^u = T_hi + T_hi u + [T_hi (e^u - 1 - u + u_lo) + T_lo (1 + u)].
    const ExpEntry& power = exp_table_256[j];
    double rotation = 0.0;
    double rotation_error = 0.0;
    two_product(power.hi, u, rotation, rotation_error);
    double head = 0.0;
    double head_error = 0.0;
    fast_two_sum(power.hi, rotation, head, head_error);  // |T| >= 1 > |T u|
    const double tail = head_error + (rotation_error + (power.hi * (polynomial_tail + u_lo) + (power.lo + power.lo * u)));
    const double result = scale_wide(head, tail, exponent);
    return result;
}

// Float pow's cheap kernel: x^y = 2^(y log2 x) in plain double for x
// positive normal and y finite nonzero. log2 x = k + log z / ln 2 from the
// log table (polynomial to r^5: truncation below 2^-50 absolute); p = y
// log2 x; 2^p = 2^(kk) 2^(j/256) 2^r2 with r2 = p - q/256 exact (|r2| <=
// 1/512) and a cubic for 2^r2 (truncation below 2^-42.7 relative). |log2 x|
// errs by at most (1 + |log2 x|) 2^-48 (seven roundings of terms below
// 1 + |log2 x|, with margin), which p carries times |y|, and 2^p turns an
// absolute error d in p into a relative error d ln 2. Returns false when
// |p| > 150 (results beyond float's range) or the rounding is uncertain.
// Modular by design: the log table offset arithmetic (see log_core.hpp).
STX_AUXLIB_MODULAR bool pow_float_cheap(const double x, const double y, float& result) noexcept {
    const std::uint64_t bits = std::bit_cast<std::uint64_t>(x);
    const std::uint64_t offset = bits - log_offset_bits;
    const std::uint64_t index = (offset >> 45) & log_index_mask;
    const std::int64_t k_bits = static_cast<std::int64_t>(offset) >> 52;
    const double z = std::bit_cast<double>(bits - (offset & exponent_field));
    const LogEntry& entry = log_table_128[index];
    const double r = multiply_add(z, entry.inv, -1.0);
    // Estrin's split: two short chains instead of one long one.
    const double r_squared = r * r;
    const double low_terms = multiply_add(r, kernel_constants.log_3, -0.5);
    const double high_terms = multiply_add(r, kernel_constants.log_5, kernel_constants.log_4);
    const double log1p_r = multiply_add(r_squared, multiply_add(r_squared, high_terms, low_terms), r);
    const double log2_x = multiply_add(entry.logc_hi + log1p_r, log_inv_ln2_hi, static_cast<double>(k_bits));
    const double p = y * log2_x;
    if (!(std::fabs(p) <= 150.0)) return false;
    // q = round(256 p), j = q mod 256, kk = floor(q / 256).
    const double shifted = multiply_add(p, 256.0, rounding_shift);
    const std::int64_t q_bits = std::bit_cast<std::int64_t>(shifted) -
                                std::bit_cast<std::int64_t>(rounding_shift);
    const std::uint64_t j = static_cast<std::uint64_t>(q_bits) & 255;
    const std::int64_t kk = q_bits >> 8;
    const double r2 = p - (shifted - rounding_shift) * (1.0 / 256.0);  // exact
    const double two_r2 = multiply_add(r2, multiply_add(r2, multiply_add(r2, pow2_c3, pow2_c2), pow2_c1), 1.0);
    const double value = (exp_table_256[j].hi * two_r2) * power_of_two(kk);
    const double p_error = std::fabs(y) * ((1.0 + std::fabs(log2_x)) * 0x1p-48) + std::fabs(p) * 0x1p-52;
    const double relative = p_error * 0.7 + 0x1p-42;
    const bool certain = float_if_certain(value, value * relative, result);
    return certain;
}

// Inputs where x is zero or infinite (y finite or not, neither NaN).
double pow_zero_or_infinite_base(const double x, const double y) noexcept {
    const double infinity = std::numeric_limits<double>::infinity();
    const bool odd = std::isfinite(y) && integer_kind(y) == IntegerKind::odd;
    const bool negative_base = std::signbit(x);
    double magnitude = 0.0;
    // 0^y is infinite for y < 0; infinity^y is infinite for y > 0.
    if ((x == 0.0) == (y < 0.0)) magnitude = infinity;
    if (negative_base && odd) return -magnitude;
    return magnitude;
}

} // namespace
} // namespace detail

double pow(const double x, const double y) noexcept {
    // The common case in one test: x positive, normal and finite; y finite
    // and nonzero.
    const std::uint64_t x_bits = std::bit_cast<std::uint64_t>(x);
    const std::uint64_t y_magnitude = std::bit_cast<std::uint64_t>(y) & detail::magnitude_mask;
    if (detail::log_fast_path(x_bits) && y_magnitude != 0 && y_magnitude < detail::infinity_bits) {
        const double result = detail::pow_positive(x, y);
        return result;
    }
    if (y == 0.0) return 1.0;
    if (x == 1.0) return 1.0;
    if (x != x || y != y) return x + y;
    const double infinity = std::numeric_limits<double>::infinity();
    if (std::isinf(y)) {
        const double magnitude = std::fabs(x);
        if (magnitude == 1.0) return 1.0;
        if ((magnitude < 1.0) == (y > 0.0)) return 0.0;
        return infinity;
    }
    if (x == 0.0 || std::isinf(x)) return detail::pow_zero_or_infinite_base(x, y);
    if (x > 0.0) return detail::pow_positive(x, y);
    // Negative finite x: defined only for integer y, with the parity's sign.
    const detail::IntegerKind kind = detail::integer_kind(y);
    if (kind == detail::IntegerKind::not_integer) return std::numeric_limits<double>::quiet_NaN();
    const double magnitude = detail::pow_positive(-x, y);
    if (kind == detail::IntegerKind::odd) return -magnitude;
    return magnitude;
}

namespace detail {
namespace {

// The accurate float power: the double result rounded once to float (which
// also rounds float overflow and underflow correctly), out of line so the
// cheap path's tail call needs no stack frame.
STX_AUXLIB_MATH_COLD float pow_float_accurate(const float x, const float y) noexcept {
    const double result = pow(detail::widen(x), detail::widen(y));
    return static_cast<float>(result);
}

} // namespace
} // namespace detail

// Float pow: first 2^(y log2 x) in plain double (pow_float_cheap),
// certified with float_if_certain; otherwise the double result rounded once
// (pow_float_accurate).
float pow(const float x, const float y) noexcept {
    const double xd = detail::widen(x);
    const double yd = detail::widen(y);
    const std::uint64_t x_bits = std::bit_cast<std::uint64_t>(xd);
    const std::uint64_t y_magnitude = std::bit_cast<std::uint64_t>(yd) & detail::magnitude_mask;
    if (detail::log_fast_path(x_bits) && y_magnitude != 0 && y_magnitude < detail::infinity_bits) {
        float fast = 0.0f;
        if (detail::pow_float_cheap(xd, yd, fast)) return fast;
    }
    const float result = detail::pow_float_accurate(x, y);
    return result;
}

} // namespace stx_auxlib::math::STX_AUXLIB_MATH_VARIANT

STX_AUXLIB_MATH_TARGET_POP
