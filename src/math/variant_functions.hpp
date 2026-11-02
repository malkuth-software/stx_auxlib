// The math functions one kernel variant defines (variant.hpp), in its
// namespace: the public set of stx_auxlib/math.hpp. Included without a guard,
// once per variant: by each kernel source for its own variant, and by
// dispatch.cpp once for every variant it calls, with
// STX_AUXLIB_MATH_DECLARE_VARIANT naming the namespace.

#if defined(STX_AUXLIB_MATH_DECLARE_VARIANT)
namespace stx_auxlib::math::STX_AUXLIB_MATH_DECLARE_VARIANT {
#else
namespace stx_auxlib::math::STX_AUXLIB_MATH_VARIANT {
#endif

void sincos(float angle, float& s, float& c) noexcept;
void sincos(double x, double& s, double& c) noexcept;
float exp(float x) noexcept;
double exp(double x) noexcept;
double log(double x) noexcept;
float log(float x) noexcept;
double log2(double x) noexcept;
float log2(float x) noexcept;
double log10(double x) noexcept;
float log10(float x) noexcept;
double pow(double x, double y) noexcept;
float pow(float x, float y) noexcept;
double hypot(double x, double y) noexcept;
float hypot(float x, float y) noexcept;
double atan(double x) noexcept;
float atan(float x) noexcept;
double atan2(double y, double x) noexcept;
float atan2(float y, float x) noexcept;
double asin(double x) noexcept;
float asin(float x) noexcept;
double acos(double x) noexcept;
float acos(float x) noexcept;
double tan(double x) noexcept;
float tan(float x) noexcept;
double cbrt(double x) noexcept;
float cbrt(float x) noexcept;
double sinh(double x) noexcept;
float sinh(float x) noexcept;
double cosh(double x) noexcept;
float cosh(float x) noexcept;
double tanh(double x) noexcept;
float tanh(float x) noexcept;
double asinh(double x) noexcept;
float asinh(float x) noexcept;
double acosh(double x) noexcept;
float acosh(float x) noexcept;
double atanh(double x) noexcept;
float atanh(float x) noexcept;

} // namespace stx_auxlib::math::<variant>
