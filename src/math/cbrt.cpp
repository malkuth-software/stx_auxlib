// cbrt (declared, with its contracts, in stx_auxlib/math.hpp).
//
// |x| = m * 2^(3k) with m in [1, 8) (subnormal x scaled by 2^54 first). A
// 64-entry table of m^(-1/3) start values (0.26% off) and three
// division-free Newton steps reach double precision; y = m r^2 is the root,
// the exact residual m - y^3 (y^3 formed as a double-double) gives the
// correction y_lo = (m - y^3) r^2 / 3, and (y + y_lo) * 2^k is rounded once.

#include "stx_auxlib/math.hpp"

#include "kernel_support.hpp"

namespace stx_auxlib::math {

namespace detail {
namespace {

constexpr std::uint64_t significand_mask = 0x000fffffffffffffULL;
constexpr double four_thirds = 4.0 / 3.0;
constexpr double one_third = 1.0 / 3.0;

// x^(-1/3) for x in [1, 2], by Newton from 1 at compile time (eight steps
// take 26% to the rounding limit). Only a starting value: any error is
// removed by the run-time steps below.
constexpr double reciprocal_cube_root(const double x) {
    double r = 1.0;
    for (int step = 0; step < 8; ++step) r = r * (four_thirds - (x * one_third) * ((r * r) * r));
    return r;
}

// Starting values: (1 + (i + 1/2)/64)^(-1/3) for the 64 buckets of [1, 2),
// within 0.26% of every m1^(-1/3) in their bucket.
struct CbrtStart final {
    double values[64];
};

constexpr CbrtStart make_cbrt_start() {
    CbrtStart table{};
    for (int i = 0; i < 64; ++i) {
        table.values[i] = reciprocal_cube_root(1.0 + (static_cast<double>(i) + 0.5) / 64.0);
    }
    return table;
}

constexpr CbrtStart cbrt_start = make_cbrt_start();
// 2^(-e/3) for the octave remainder e = 0, 1, 2.
constexpr double cbrt_octave[3] = {1.0, 0.79370052598409973738, 0.62996052494743658238};

// m in [1, 8) and k with a = m * 2^(3k), for finite a > 0.
double cbrt_reduce(const double a, std::int64_t& k) noexcept {
    double scaled = a;
    std::int64_t adjust = 0;
    if (a < 0x1p-1022) {
        scaled = a * 0x1p54;  // exact; cbrt(2^54) = 2^18
        adjust = -18;
    }
    const std::uint64_t bits = std::bit_cast<std::uint64_t>(scaled);
    const std::int64_t exponent = static_cast<std::int64_t>(bits >> 52) - exponent_bias;
    // floor(exponent / 3) and the remainder in {0, 1, 2}.
    std::int64_t third = exponent / 3;
    if (exponent < 0 && third * 3 != exponent) third -= 1;
    const std::int64_t remainder = exponent - 3 * third;
    const std::uint64_t m_bits =
        (bits & significand_mask) | (static_cast<std::uint64_t>(remainder + exponent_bias) << 52);
    k = third + adjust;
    const double m = std::bit_cast<double>(m_bits);
    return m;
}

// m^(-1/3) to within 0.26% for m in [1, 8): the bucket's start value times
// 2^(-e/3) for m's octave e.
double cbrt_start_value(const double m) noexcept {
    const std::uint64_t m_bits = std::bit_cast<std::uint64_t>(m);
    const std::uint64_t bucket = (m_bits >> 46) & 63;
    const std::uint64_t octave = (m_bits >> 52) - static_cast<std::uint64_t>(exponent_bias);
    const double result = cbrt_start.values[bucket] * cbrt_octave[octave];
    return result;
}

// m^(1/3) as root (rounded, within ~2 ulp) and its reciprocal-squared
// approximation for the correction. Newton steps on the reciprocal cube
// root, r <- r (4/3 - (m/3) r^3), need no division; from the table's 0.26%
// three steps reach the double rounding limit (2.6e-3 -> 1.4e-5 -> 3.6e-10
// -> 2.6e-19).
double cbrt_root(const double m, double& reciprocal_squared) noexcept {
    double r = cbrt_start_value(m);
    const double third_m = m * one_third;
    for (int step = 0; step < 3; ++step) {
        const double r_cubed = (r * r) * r;
        r = r * (four_thirds - third_m * r_cubed);
    }
    reciprocal_squared = r * r;
    const double root = m * reciprocal_squared;
    return root;
}

// The same three steps with multiply_add, for the certified float path.
double cbrt_root_cheap(const double m) noexcept {
    double r = cbrt_start_value(m);
    const double third_m = m * one_third;
    for (int step = 0; step < 3; ++step) {
        const double r_cubed = (r * r) * r;
        r = r * multiply_add(-third_m, r_cubed, four_thirds);
    }
    const double root = m * (r * r);
    return root;
}

// cbrt(a) = (y + y_lo) * 2^k for finite a > 0: the root and its correction
// from the exact residual m - y^3 (y^3 formed as a double-double), times
// 1/(3 y^2) ~ r^2 / 3.
void cbrt_wide(const double a, double& y, double& y_lo, std::int64_t& k) noexcept {
    const double m = cbrt_reduce(a, k);
    double reciprocal_squared = 0.0;
    const double root = cbrt_root(m, reciprocal_squared);
    double square = 0.0;
    double square_error = 0.0;
    two_product(root, root, square, square_error);
    double cube = 0.0;
    double cube_error = 0.0;
    two_product(square, root, cube, cube_error);
    const double cube_lo = cube_error + square_error * root;
    const double residual = (m - cube) - cube_lo;  // m - cube is exact
    y = root;
    y_lo = residual * (reciprocal_squared * one_third);
}

} // namespace
} // namespace detail

double cbrt(const double x) noexcept {
    // Zeros, infinities and NaN are their own cube roots (x + x quiets NaN
    // and keeps the sign of zero).
    if (x == 0.0 || !std::isfinite(x)) return x + x;
    double y = 0.0;
    double y_lo = 0.0;
    std::int64_t k = 0;
    detail::cbrt_wide(std::fabs(x), y, y_lo, k);
    // (y + y_lo) rounds once; the scaling by 2^k (k >= -358) is exact.
    const double magnitude = (y + y_lo) * detail::power_of_two(k);
    if (std::signbit(x)) return -magnitude;
    return magnitude;
}

// Float: the root alone (within 2^-50 relative, measured; certified against
// 2^-48) when its float rounding is certain, otherwise the corrected
// double-double.
float cbrt(const float value) noexcept {
    const double x = static_cast<double>(value);
    if (x == 0.0 || !std::isfinite(x)) return value + value;
    std::int64_t k = 0;
    const double m = detail::cbrt_reduce(std::fabs(x), k);
    const double root = detail::cbrt_root_cheap(m);
    const double scale = detail::power_of_two(k);
    double approx = root * scale;
    if (std::signbit(x)) approx = -approx;
    float fast = 0.0f;
    if (detail::float_if_certain(approx, std::fabs(approx) * 0x1p-48, fast)) return fast;
    double y = 0.0;
    double y_lo = 0.0;
    detail::cbrt_wide(std::fabs(x), y, y_lo, k);
    double hi = y * scale;
    double lo = y_lo * scale;
    if (std::signbit(x)) {
        hi = -hi;
        lo = -lo;
    }
    const float result = detail::round_wide_to_float(hi, lo);
    return result;
}

} // namespace stx_auxlib::math
