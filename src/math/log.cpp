// log, log2 and log10 (declared, with their contracts, in
// stx_auxlib/math.hpp). The core is in log_core.hpp.

#include "stx_auxlib/math.hpp"

#include "log_core.hpp"

#include <limits>

namespace stx_auxlib::math {

namespace detail {
namespace {

// The logarithm of every double the fast path rejects. The base is a
// template parameter so each public function compiles with its constants
// folded (a run-time base cost ~2 ns a call).
template <const LogBase& base>
double log_outside(const double x) noexcept {
    if (x != x) return x + x;
    if (x == 0.0) return -std::numeric_limits<double>::infinity();
    if (x < 0.0) return std::numeric_limits<double>::quiet_NaN();
    if (x == std::numeric_limits<double>::infinity()) return x;
    // Positive subnormal: scale into the normal range exactly.
    const std::uint64_t bits = std::bit_cast<std::uint64_t>(x * two_to_52);
    double k = 0.0;
    double head = 0.0;
    double tail = 0.0;
    log_z(bits, 52.0, k, head, tail);
    const double result = log_combine(base, k, head, tail);
    return result;
}

template <const LogBase& base>
double log_any(const double x) noexcept {
    const std::uint64_t bits = std::bit_cast<std::uint64_t>(x);
    if (!log_fast_path(bits)) return log_outside<base>(x);
    double k = 0.0;
    double head = 0.0;
    double tail = 0.0;
    log_z(bits, 0.0, k, head, tail);
    const double result = log_combine(base, k, head, tail);
    return result;
}

// Float inputs are normal doubles (or zero, negative, infinite, NaN): the
// cheap path when its rounding is certain, otherwise the double result
// rounded once to float (exhaustively checked to be correctly rounded).
// The accurate float logarithm: the double result rounded once (exhaustively
// checked correctly rounded), out of line for the fast path's tail call.
template <const LogBase& base>
STX_AUXLIB_MATH_COLD float log_float_accurate(const float x) noexcept {
    const double result = log_any<base>(detail::widen(x));
    return static_cast<float>(result);
}

template <const LogBase& base>
float log_any(const float x) noexcept {
    const std::uint64_t bits = std::bit_cast<std::uint64_t>(detail::widen(x));
    if (log_fast_path(bits)) {
        double value = 0.0;
        double error = 0.0;
        log_cheap(base, bits, value, error);
        float fast = 0.0f;
        if (float_if_certain(value, error, fast)) return fast;
    }
    const float result = log_float_accurate<base>(x);
    return result;
}

} // namespace
} // namespace detail

double log(const double x) noexcept {
    const double result = detail::log_any<detail::natural>(x);
    return result;
}

float log(const float x) noexcept {
    const float result = detail::log_any<detail::natural>(x);
    return result;
}

double log2(const double x) noexcept {
    const double result = detail::log_any<detail::binary>(x);
    return result;
}

float log2(const float x) noexcept {
    const float result = detail::log_any<detail::binary>(x);
    return result;
}

double log10(const double x) noexcept {
    const double result = detail::log_any<detail::decimal>(x);
    return result;
}

float log10(const float x) noexcept {
    const float result = detail::log_any<detail::decimal>(x);
    return result;
}

} // namespace stx_auxlib::math
