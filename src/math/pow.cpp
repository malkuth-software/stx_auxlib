// pow (declared, with its contracts, in stx_auxlib/math.hpp).
//
// x^y = e^(y * log x) for positive finite x: log x as a double-double
// (log_core.hpp: first the plain core, ~2^-69 relative, and the precise
// one, ~1e-24, only when a rounding test cannot certify the plain result),
// y * log x formed exactly as p + p_lo
// (two_product), and e^(p + p_lo) from the exp core with full-range scaling
// (exp_core.hpp). An error e in log x becomes |y * log x| * e in the result,
// at most 709 * 1e-21 ~ 0.006 ulp. Negative x with integer y takes the sign
// of the integer's parity; every other special input follows C99 Annex F.

#include "stx_auxlib/math.hpp"

#include "exp_core.hpp"
#include "log_core.hpp"

#include <limits>

namespace stx_auxlib::math {

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

constexpr double pow_overflow_limit = 709.79;   // e^p above it is infinite
constexpr double pow_underflow_limit = -746.0;  // e^p below it rounds to +0

// Bound on the relative error of e^(y log x) built from log_natural_plain:
// log_z is within ~2^-69 of log x (measured 1.2e-5 ulp), which the product
// with y turns into |p| * 2^-69 absolute in p; the exp core adds ~2^-72.
// The test below uses (|p| + 1) * 2^-66, a factor 8 of margin.
constexpr double pow_plain_error = 0x1p-66;

// True when head + tail (head in [0.5, 4), the unscaled exp core result)
// carries a relative error below `relative` and still rounds to the same
// double as the exact value: the exact value cannot reach a rounding
// midpoint. Only for normal results (exponent in [-1021, 1022]).
bool rounds_safely(const double head, const double tail, const std::int64_t exponent,
                   const double relative) noexcept {
    const std::uint64_t normal_offset = static_cast<std::uint64_t>(exponent + 1021);
    if (normal_offset > 2043) return false;
    double rounded = 0.0;
    double residual = 0.0;
    two_sum(head, tail, rounded, residual);
    const std::uint64_t bits = std::bit_cast<std::uint64_t>(rounded);
    const double gap_up = std::bit_cast<double>(bits + 1) - rounded;
    const double gap_down = rounded - std::bit_cast<double>(bits - 1);
    const double half_gap = std::fmin(gap_up, gap_down) * 0.5;
    const bool result = std::fabs(residual) + relative * rounded < half_gap;
    return result;
}

// x^y for positive finite x (normal or subnormal) and finite nonzero y.
// First from the plain log (2.4 ns); when the rounding test cannot certify
// that result, again from the precise log (4.5 ns). A certified plain result
// is the correctly rounded value, which the precise path also returns
// there, so the bits do not depend on which path ran.
double pow_positive(const double x, const double y) noexcept {
    double scaled = x;
    double exponent_adjust = 0.0;
    if (x < 0x1p-1022) {
        scaled = x * 0x1p52;  // subnormal: exact scaling into the normal range
        exponent_adjust = 52.0;
    }
    const std::uint64_t bits = std::bit_cast<std::uint64_t>(scaled);
    double log_head = 0.0;
    double log_tail = 0.0;
    log_natural_plain(bits, exponent_adjust, log_head, log_tail);
    double p = 0.0;
    double p_error = 0.0;
    two_product(y, log_head, p, p_error);
    // p is +-infinity (and p_error NaN) when y * log x overflows; the limits
    // below decide those before p_lo is used.
    if (p > pow_overflow_limit) return std::numeric_limits<double>::infinity();
    if (p < pow_underflow_limit) return 0.0;
    double p_lo = p_error + y * log_tail;
    double head = 0.0;
    double tail = 0.0;
    std::int64_t exponent = 0;
    exp_core(p, p_lo, head, tail, exponent);
    if (rounds_safely(head, tail, exponent, (std::fabs(p) + 1.0) * pow_plain_error)) {
        const double result = scale_wide(head, tail, exponent);
        return result;
    }
    log_natural_wide(bits, exponent_adjust, log_head, log_tail);
    two_product(y, log_head, p, p_error);
    if (p > pow_overflow_limit) return std::numeric_limits<double>::infinity();
    if (p < pow_underflow_limit) return 0.0;
    p_lo = p_error + y * log_tail;
    exp_core(p, p_lo, head, tail, exponent);
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

} // namespace stx_auxlib::math
