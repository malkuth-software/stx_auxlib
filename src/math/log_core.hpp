#pragma once

// The log core shared by log, log2, log10 (log.cpp) and pow (pow.cpp).
// Private to the library; see kernel_support.hpp for the floating-point
// rules.
//
// x = 2^k * z with z in [0.6873, 1.3745) (subnormal x scaled by 2^52
// first). One of 128 buckets gives inv ~ 1/z and logc = -log(inv), so that
//   log z = logc + log1p(r),   r = z * inv - 1,   |r| <= 1/256,
// exactly, for the rounded inv. r is formed exactly as r + r_lo
// (two_product; z * inv - 1 is exact by Sterbenz), r - r^2/2 exactly, and
// the rest of log1p(r) from a Taylor polynomial truncated at r^9/9 ~ 2e-23.

#include "../checked.hpp"
#include "constants.hpp"
#include "kernel_support.hpp"
#include "log_table128.hpp"

STX_AUXLIB_MATH_TARGET_PUSH

namespace stx_auxlib::math::STX_AUXLIB_MATH_VARIANT::detail {

inline constexpr std::uint64_t smallest_normal_bits = 0x0010000000000000ULL;
inline constexpr std::uint64_t infinity_bits = 0x7ff0000000000000ULL;
inline constexpr std::uint64_t exponent_field = std::uint64_t{0xfff} << 52;
inline constexpr std::uint64_t log_index_mask = 127;
inline constexpr double two_to_52 = 0x1p52;

inline constexpr double log_3 = 1.0 / 3.0;
inline constexpr double log_4 = -1.0 / 4.0;
inline constexpr double log_5 = 1.0 / 5.0;
inline constexpr double log_6 = -1.0 / 6.0;
inline constexpr double log_7 = 1.0 / 7.0;
inline constexpr double log_8 = -1.0 / 8.0;

// The buckets' inverses split for Dekker's product, built at compile time
// (two_product_split).
struct LogSplits final {
    Split values[128];
};

constexpr LogSplits make_log_splits() noexcept {
    LogSplits table{};
    for (int i = 0; i < 128; ++i) table.values[i] = veltkamp_split(log_table_128[i].inv);
    return table;
}

alignas(64) inline constexpr LogSplits log_splits = make_log_splits();

// True for positive normal finite x (false for zero, subnormals, negatives,
// infinities and NaN).
inline bool log_fast_path(const std::uint64_t bits) noexcept {
    const bool result = bits >= smallest_normal_bits && bits < infinity_bits;
    return result;
}

// log z = logz_head + logz_tail for the z of `bits`, and its k. `bits` is a
// positive normal double; `exponent_adjust` is subtracted from k (52 for a
// subnormal input scaled by 2^52).
// Modular by design: offset = bits - log_offset_bits wraps (as unsigned
// 64-bit arithmetic) for z below the table's base, and its arithmetic shift
// then yields k < 0; bits - (offset & exponent_field) undoes it modulo 2^64.
STX_AUXLIB_MODULAR inline void log_z(const std::uint64_t bits, const double exponent_adjust, double& k, double& logz_head,
           double& logz_tail) noexcept {
    const std::uint64_t offset = bits - log_offset_bits;
    const std::uint64_t index = (offset >> 45) & log_index_mask;
    // The exponent field of the offset is k (two's complement, modulo 4096):
    // removing it from the input's bits leaves z.
    const std::int64_t k_bits = static_cast<std::int64_t>(offset) >> 52;
    k = static_cast<double>(k_bits) - exponent_adjust;
    const double z = std::bit_cast<double>(bits - (offset & exponent_field));
    const LogEntry& entry = log_table_128[index];
    double product = 0.0;
    double r_lo = 0.0;
    two_product_split(entry.inv, log_splits.values[index], z, product, r_lo);
    const double r = product - 1.0;  // exact: product is within 2^-7 of 1
    // log1p(r + r_lo) = r - r^2/2 + [r^3 * polynomial(r) + r_lo / (1 + r)].
    // r^2/2 can reach 0.4% of the result, so it is formed exactly; the
    // remaining polynomial is below r^2/3 of the result and its rounding
    // stays under 1e-5 ulp. r_lo is the rounding error of z * inv, up to
    // 1.1e-16 in absolute terms, so 1/(1 + r) needs terms to r^3: r_lo * r^2
    // alone reaches 0.004 ulp of a result near 0.002.
    double r_squared = 0.0;
    double r_squared_error = 0.0;
    two_product(r, r, r_squared, r_squared_error);
    const double half_r_squared = r_squared * 0.5;  // exact
    const double cubic =
        (r * r_squared) * (kernel_constants.log_3 + r * (kernel_constants.log_4 + r * (kernel_constants.log_5 + r * (kernel_constants.log_6 + r * (kernel_constants.log_7 + r * kernel_constants.log_8)))));
    // Fast two-sums: |logc| >= |r| in every bucket (logc = 0 in the one
    // holding 1), and |logc + r| >= r^2/2.
    double sum = 0.0;
    double sum_error = 0.0;
    fast_two_sum(entry.logc_hi, r, sum, sum_error);
    double head_error = 0.0;
    fast_two_sum(sum, -half_r_squared, logz_head, head_error);
    const double r_lo_scaled = r_lo * (1.0 - r * (1.0 - r * (1.0 - r)));
    const double small = (entry.logc_lo + r_lo_scaled) + (cubic - r_squared_error * 0.5);
    logz_tail = (sum_error + head_error) + small;
}

// The base of a logarithm, as the constants of
//   log_b x = k * per_octave_hi + log z * inverse + [k * per_octave_lo]
// where per_octave = log_b 2 and inverse = 1/ln b (exactly 1 for ln). The
// four values sit at kernel_constants.log_bases[first .. first + 3] (in that
// order), loaded rather than rebuilt per call; `scaled` stays a constant so
// each base's code is specialised.
struct LogBase final {
    std::size_t first{0};
    bool scaled{false};  // false: inverse is exactly 1
};

inline constexpr LogBase natural{0, false};
inline constexpr LogBase binary{4, true};
inline constexpr LogBase decimal{8, true};

inline double log_combine(const LogBase& base, const double k, const double logz_head,
                   const double logz_tail) noexcept {
    const double per_octave_hi = kernel_constants.log_bases[base.first];
    const double per_octave_lo = kernel_constants.log_bases[base.first + 1];
    const double inverse_hi = kernel_constants.log_bases[base.first + 2];
    const double inverse_lo = kernel_constants.log_bases[base.first + 3];
    double main = logz_head;
    double main_tail = logz_tail;
    if (base.scaled) {
        double product = 0.0;
        double product_error = 0.0;
        two_product(logz_head, inverse_hi, product, product_error);
        main = product;
        main_tail = product_error + (logz_head * inverse_lo + logz_tail * inverse_hi);
    }
    // k * per_octave_hi is exact (|k| < 2^11, 42-bit head) and, for k != 0,
    // larger than |log z * inverse| <= 0.35: a fast two-sum.
    double head = 0.0;
    double head_error = 0.0;
    fast_two_sum(k * per_octave_hi, main, head, head_error);
    const double result = head + (head_error + (main_tail + k * per_octave_lo));
    return result;
}

inline constexpr double log_9 = 1.0 / 9.0;
inline constexpr double log_10 = -1.0 / 10.0;

// A cheap logarithm for float results (and float pow): the same reduction
// in plain double with a degree-5 polynomial and a relative error bound.
// `value` is log_b x and `error` a rigorous bound on |value - log_b x|;
// callers certify the float rounding against it (float_if_certain) or fall
// back to the accurate path. Requires log_fast_path(bits).
//
// Error terms: z * inv rounds by at most 2^-53 * 1.008 absolute (exact in
// the bucket holding 1, where inv = 1), and log1p carries it through
// 1/(1 + r) <= 1.004; the polynomial truncation is below |r|^6 / 5.9; the
// eight remaining operations each round by at most 2^-53 of a partial sum
// no larger than |k ln2| + |logc| + |r|; logc_lo (dropped) is below 2^-53
// of |logc|. A base other than e scales all of it by 1/ln b < 1.45 and adds
// one rounding of the product.
// Modular by design: offset = bits - log_offset_bits wraps (as unsigned
// 64-bit arithmetic) for z below the table's base, and its arithmetic shift
// then yields k < 0; bits - (offset & exponent_field) undoes it modulo 2^64.
STX_AUXLIB_MODULAR inline void log_cheap(const LogBase& base, const std::uint64_t bits, double& value,
                      double& error) noexcept {
    const std::uint64_t offset = bits - log_offset_bits;
    const std::uint64_t index = (offset >> 45) & log_index_mask;
    const std::int64_t k_bits = static_cast<std::int64_t>(offset) >> 52;
    const double k = static_cast<double>(k_bits);
    const double z = std::bit_cast<double>(bits - (offset & exponent_field));
    const LogEntry& entry = log_table_128[index];
    const double r = z * entry.inv - 1.0;
    const double polynomial = (r * r) * (-0.5 + r * (kernel_constants.log_3 + r * (kernel_constants.log_4 + r * kernel_constants.log_5)));
    // k * log_b 2 with both parts: the 42-bit head alone leaves k * 2e-14.
    const double per_octave_hi = kernel_constants.log_bases[base.first];
    const double per_octave_lo = kernel_constants.log_bases[base.first + 1];
    const double octaves = k * per_octave_hi + k * per_octave_lo;
    const double logz = entry.logc_hi + (r + polynomial);
    if (base.scaled) {
        value = octaves + logz * kernel_constants.log_bases[base.first + 2];
    } else {
        value = octaves + logz;
    }
    // The terms above sum to at most 2^-51 + 2^-52 (|k| + |value|) +
    // 0.17 |r|^6. Outside the bucket holding 1, |value| >= log(1 + 1/512)
    // and |value| >= 0.33 |k|, so that is below 2^-41 |value|; inside it
    // (k = 0, inv = 1, r exact) the error is relative: 2^-52 of |value| plus
    // the truncation, |r|^5 / 5 <= 2^-42 of |value|. 2^-40 of |value| covers
    // every case with room; a scaled base doubles it.
    const double relative = base.scaled ? 0x1p-39 : 0x1p-40;
    error = std::fabs(value) * relative;
}

// log x = head + tail from log_z (~2^-69 relative): pow's first attempt,
// which pow accepts only when its rounding test passes.
inline void log_natural_plain(const std::uint64_t bits, const double exponent_adjust, double& head,
                              double& tail) noexcept {
    double k = 0.0;
    double logz_head = 0.0;
    double logz_tail = 0.0;
    log_z(bits, exponent_adjust, k, logz_head, logz_tail);
    double head_error = 0.0;
    // k * log_ln2_hi is exact and dominates |log z| <= 0.35 (or k = 0).
    fast_two_sum(k * kernel_constants.log_ln2_hi, logz_head, head, head_error);
    tail = head_error + (logz_tail + k * kernel_constants.log_ln2_lo);
}

// 1/3 as a double-double (1/3 - double(1/3) = 2^-54 / 3).
inline constexpr double one_third_hi = 1.0 / 3.0;
inline constexpr double one_third_lo = 0x1.5555555555555p-56;

// log x = head + tail to ~1e-24 relative, for pow, where an error in log x
// is multiplied by |y * log x| (up to 709). As log_z, but with two more
// polynomial terms, and r^3 * (1/3 - r/4 + ...) formed as a double-double
// that joins the head exactly; log_z's ~1e-21 suffices for log alone.
// Modular by design: offset = bits - log_offset_bits wraps (as unsigned
// 64-bit arithmetic) for z below the table's base, and its arithmetic shift
// then yields k < 0; bits - (offset & exponent_field) undoes it modulo 2^64.
STX_AUXLIB_MODULAR inline void log_natural_wide(const std::uint64_t bits, const double exponent_adjust, double& head,
                             double& tail) noexcept {
    const std::uint64_t offset = bits - log_offset_bits;
    const std::uint64_t index = (offset >> 45) & log_index_mask;
    const std::int64_t k_bits = static_cast<std::int64_t>(offset) >> 52;
    const double k = static_cast<double>(k_bits) - exponent_adjust;
    const double z = std::bit_cast<double>(bits - (offset & exponent_field));
    const LogEntry& entry = log_table_128[index];
    double product = 0.0;
    double r_lo = 0.0;
    two_product_split(entry.inv, log_splits.values[index], z, product, r_lo);
    const double r = product - 1.0;  // exact
    double r_squared = 0.0;
    double r_squared_error = 0.0;
    two_product(r, r, r_squared, r_squared_error);
    const double half_r_squared = r_squared * 0.5;  // exact
    // cubic = r^3 * (1/3 + r * rest) as cubic_hi + cubic_lo.
    // Two more terms than log_z: truncation r^11/11 ~ 3e-28, against log_z's
    // r^9/9 ~ 2e-23 (5e-21 relative to a result near 0.004).
    const double rest =
        kernel_constants.log_4 + r * (kernel_constants.log_5 + r * (kernel_constants.log_6 + r * (kernel_constants.log_7 + r * (kernel_constants.log_8 + r * (kernel_constants.log_9 + r * kernel_constants.log_10)))));
    double r_rest = 0.0;
    double r_rest_error = 0.0;
    two_product(r, rest, r_rest, r_rest_error);
    double factor = 0.0;
    double factor_error = 0.0;
    two_sum(kernel_constants.one_third_hi, r_rest, factor, factor_error);
    const double factor_lo = factor_error + (r_rest_error + kernel_constants.one_third_lo);
    double r_cubed = 0.0;
    double r_cubed_error = 0.0;
    two_product(r, r_squared, r_cubed, r_cubed_error);
    const double r_cubed_lo = r_cubed_error + r * r_squared_error;
    double cubic_hi = 0.0;
    double cubic_error = 0.0;
    two_product(r_cubed, factor, cubic_hi, cubic_error);
    const double cubic_lo = cubic_error + (r_cubed * factor_lo + r_cubed_lo * factor);
    // Head: k*ln2_hi + logc_hi + r - r^2/2 + cubic_hi, each sum exact.
    double sum = 0.0;
    double sum_error = 0.0;
    two_sum(entry.logc_hi, r, sum, sum_error);
    double sum2 = 0.0;
    double sum2_error = 0.0;
    two_sum(sum, -half_r_squared, sum2, sum2_error);
    double logz = 0.0;
    double logz_error = 0.0;
    two_sum(sum2, cubic_hi, logz, logz_error);
    double head_error = 0.0;
    two_sum(k * kernel_constants.log_ln2_hi, logz, head, head_error);  // k * kernel_constants.log_ln2_hi is exact
    const double r_lo_scaled = r_lo * (1.0 - r * (1.0 - r * (1.0 - r)));
    const double small = (entry.logc_lo + r_lo_scaled) + (cubic_lo - r_squared_error * 0.5);
    tail = ((sum_error + sum2_error) + (logz_error + head_error)) + (small + k * kernel_constants.log_ln2_lo);
}

} // namespace stx_auxlib::math::STX_AUXLIB_MATH_VARIANT::detail

STX_AUXLIB_MATH_TARGET_POP
