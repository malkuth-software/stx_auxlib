// sin and cos (declared, with their contracts, in stx_auxlib/math.hpp). The
// core is in sincos_core.hpp.

#include "stx_auxlib/math.hpp"

#include "sincos_core.hpp"

#include <limits>

namespace stx_auxlib::math {

namespace detail {
namespace {

// The rounded remainder r and table index for the float kernel, for
// 2^-27 <= |x| <= 2^32.
double float_remainder(const double x, std::uint64_t& index) noexcept {
    const double q = sincos_step(x, index);
    if (sincos_fast_path(x)) {
        const double r = sincos_remainder(x, q);
        return r;
    }
    double r = 0.0;
    double r_lo = 0.0;
    sincos_remainder_medium(x, q, r, r_lo);
    return r;
}

} // namespace
} // namespace detail

// sin and cos of a float (contract in stx_auxlib/math.hpp). Double
// intermediate: the polynomials leave ~1e-17 of truncation error, far below
// float rounding.
void sincos(const float angle, float& s, float& c) noexcept {
    const double x = detail::widen(angle);
    if (!detail::sincos_fast_path(x) && !detail::sincos_medium_path(x)) {
        if (std::fabs(x) < detail::sincos_tiny_limit) {
            s = angle;
            c = 1.0f;
        } else {
            // |x| > 2^32, infinities and NaN: refused (NaN propagates).
            const float refused = x != x ? angle + angle : std::numeric_limits<float>::quiet_NaN();
            s = refused;
            c = refused;
        }
        return;
    }
    std::uint64_t index = 0;
    const double r = detail::float_remainder(x, index);
    const double r2 = r * r;
    const double sin_r = r + r * (r2 * (detail::sin_3 + r2 * detail::sin_5));
    const double cos_r_minus_1 =
        (r2 * r2) * (detail::cos_4 + r2 * detail::cos_6) - r2 * detail::cos_half;
    const detail::SinCosEntry& entry = detail::sincos_table_256[index];
    const double sin_correction = entry.s * cos_r_minus_1 + entry.c * sin_r;
    const double cos_correction = entry.c * cos_r_minus_1 - entry.s * sin_r;
    s = static_cast<float>(entry.s + sin_correction);
    c = static_cast<float>(entry.c + cos_correction);
}

// sin and cos of a double (contract in stx_auxlib/math.hpp).
void sincos(const double x, double& s, double& c) noexcept {
    double sin_head = 0.0;
    double sin_tail = 0.0;
    double cos_head = 0.0;
    double cos_tail = 0.0;
    if (detail::sincos_fast_path(x)) {
        detail::sincos_wide(x, sin_head, sin_tail, cos_head, cos_tail);
    } else if (detail::sincos_medium_path(x)) {
        detail::sincos_wide_medium(x, sin_head, sin_tail, cos_head, cos_tail);
    } else if (std::fabs(x) < detail::sincos_tiny_limit) {
        s = x;
        c = 1.0;
        return;
    } else {
        // |x| > 2^32, infinities and NaN: refused (NaN propagates).
        const double refused = x != x ? x + x : std::numeric_limits<double>::quiet_NaN();
        s = refused;
        c = refused;
        return;
    }
    s = sin_head + sin_tail;
    c = cos_head + cos_tail;
}

} // namespace stx_auxlib::math
