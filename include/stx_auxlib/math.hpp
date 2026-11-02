#pragma once

// Owned transcendental functions. GUI.Forms and its consumers call these
// instead of <cmath>'s, whose results differ between C libraries.
//
// Contract shared by every function here:
// - Results are the same bits on every architecture and compiler. The
//   definitions live in src/math/ and compile only with stx_auxlib's own
//   flags, which switch floating-point contraction off; a consumer's flags
//   (fused multiply-add, -ffp-contract=fast) never reach them. The one exact
//   product they use is exact whether formed by a hardware fma or by
//   Dekker's split.
// - Accuracy is stated per function, measured against the double-double
//   references in tests/reference/wide.hpp (tests/math_tests.cpp).
// - Rare inputs leave the fast path through one compare. Every function
//   owns every input; none returns the C++ library's result.

namespace stx_auxlib::math {

// Angles for sin, cos and tan are supported up to |x| = 2^32 radians;
// larger angles, infinities and NaN are refused with NaN. No GUI.Forms use
// needs more (a full turn is 6.28): a phase that grows without bound must
// reset itself or use its own reduction.
//
// sin and cos of a float, correctly rounded for every float with
// 2^-27 <= |x| <= 2^20 (exhaustively checked, 788,529,154 floats); below
// 2^-27 sin = x and cos = 1, also correctly rounded.
void sincos(float angle, float& s, float& c) noexcept;

// sin and cos of a double, within 0.51 ulp for every double 2^-27 <= |x| <=
// 2^32: proved, not sampled (tools/sincos_certificate.py: the worst-case
// remainders of every binade, the rotation's interval error analysis and
// the reduction's exact error bound). The largest error measured over 120
// million sampled inputs is 0.500415 ulp; about 1 in 18,000 results is not
// the correctly rounded double. Below 2^-27 sin = x and cos = 1 (correctly
// rounded, sign of zero kept).
void sincos(double x, double& s, double& c) noexcept;

// e^x for a float, correctly rounded for every float (exhaustively checked,
// 4,278,190,080 finite floats),
// overflow to infinity and underflow to the correctly rounded subnormal or
// zero included. NaN returns NaN, -infinity returns +0. Owns every input.
[[nodiscard]] float exp(float x) noexcept;

// e^x for a double, within 0.500001 ulp (100 million inputs measured; about
// 1 in 6 million results is not the correctly rounded double). Owns every
// input: above ln(DBL_MAX) it returns +infinity; subnormal results are
// rounded once on the subnormal grid; below ln(2^-1075) it returns +0; NaN
// returns NaN and -infinity returns +0.
[[nodiscard]] double exp(double x) noexcept;

// Natural, binary and decimal logarithms. Float: correctly rounded for every
// positive finite float (exhaustively checked, 2,139,095,039 floats each).
// Double: within 0.500012 ulp (240 million inputs measured per function;
// about 1 in 24 million results is not the correctly rounded double).
// Own every input: +0 and -0 give -infinity, negative x and -infinity give
// NaN, +infinity gives +infinity, NaN gives NaN; subnormal x is handled
// exactly. log(1) = +0, log2(2^k) = k and log10(10^n) = n (for the exactly
// representable powers) are exact.
[[nodiscard]] double log(double x) noexcept;
[[nodiscard]] float log(float x) noexcept;
[[nodiscard]] double log2(double x) noexcept;
[[nodiscard]] float log2(float x) noexcept;
[[nodiscard]] double log10(double x) noexcept;
[[nodiscard]] float log10(float x) noexcept;

// x^y. Owns every input, following C99 Annex F: pow(x, +-0) = 1 and
// pow(1, y) = 1 even for NaN; negative x is defined for integer y only (NaN
// otherwise) and takes the sign of the integer's parity; zero and infinite
// x and infinite y give the Annex F zeros and infinities. Exactly
// representable results (2^n, 3^2, 10^n for n <= 22, x^1, ...) are exact.
// Double: within 0.499996 ulp over 500,000 sampled inputs across the range,
// subnormal results correctly rounded. Float: correctly rounded at every
// sampled pair (the double result rounded once).
[[nodiscard]] double pow(double x, double y) noexcept;
[[nodiscard]] float pow(float x, float y) noexcept;

// sqrt(x^2 + y^2) without intermediate overflow or underflow. Owns every
// input: an infinite operand gives +infinity even when the other is NaN
// (Annex F), otherwise NaN gives NaN; hypot(x, +-0) = |x|. Accuracy: see
// tests/math_tests.cpp.
[[nodiscard]] double hypot(double x, double y) noexcept;
[[nodiscard]] float hypot(float x, float y) noexcept;

// Inverse trigonometric functions. Own every input, following C99 Annex F:
// atan(+-infinity) = +-pi/2; atan2's zeros, infinities and signs as Annex F
// lists them (atan2(+-0, -0) = +-pi, atan2(+-0, +0) = +-0, ...); asin and
// acos are NaN outside [-1, 1]; acos(1) = +0; signs of zero are kept.
// Accuracy: see tests/math_tests.cpp.
[[nodiscard]] double atan(double x) noexcept;
[[nodiscard]] float atan(float x) noexcept;
[[nodiscard]] double atan2(double y, double x) noexcept;
[[nodiscard]] float atan2(float y, float x) noexcept;
[[nodiscard]] double asin(double x) noexcept;
[[nodiscard]] float asin(float x) noexcept;
[[nodiscard]] double acos(double x) noexcept;
[[nodiscard]] float acos(float x) noexcept;

// tan x for |x| <= 2^32 (refused above, as for sincos). Below 2^-27
// tan x = x (sign of zero kept). Double: within 0.51 ulp for every double
// 2^-27 <= |x| <= 2^32, proved as for sincos (measured worst 0.500154 ulp
// over the sampled regions). Float: correctly rounded, checked exhaustively
// against the test reference for |x| <= 2^20, sampled above.
[[nodiscard]] double tan(double x) noexcept;
[[nodiscard]] float tan(float x) noexcept;

// Cube root. Owns every input: zeros, infinities and NaN are their own cube
// roots; exact cubes (8, 27, 2^(3k), ...) give exact roots.
[[nodiscard]] double cbrt(double x) noexcept;
[[nodiscard]] float cbrt(float x) noexcept;

// Hyperbolic functions and their inverses. Own every input: sinh and cosh
// overflow to infinity, tanh saturates at +-1, acosh(1) = +0, atanh(+-1) =
// +-infinity, NaN outside acosh's [1, inf) and atanh's [-1, 1], signs of
// zero kept. Accuracy: see tests/math_tests.cpp.
[[nodiscard]] double sinh(double x) noexcept;
[[nodiscard]] float sinh(float x) noexcept;
[[nodiscard]] double cosh(double x) noexcept;
[[nodiscard]] float cosh(float x) noexcept;
[[nodiscard]] double tanh(double x) noexcept;
[[nodiscard]] float tanh(float x) noexcept;
[[nodiscard]] double asinh(double x) noexcept;
[[nodiscard]] float asinh(float x) noexcept;
[[nodiscard]] double acosh(double x) noexcept;
[[nodiscard]] float acosh(float x) noexcept;
[[nodiscard]] double atanh(double x) noexcept;
[[nodiscard]] float atanh(float x) noexcept;

} // namespace stx_auxlib::math
