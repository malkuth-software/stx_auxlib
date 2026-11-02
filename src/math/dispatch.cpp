// The public math functions (stx_auxlib/math.hpp): each calls the kernel
// variant (variant.hpp) the process's SIMD level selects
// (stx_auxlib/simd.hpp). Generated once by hand from math.hpp's list; keep
// the four lists below (table fields, the two variant tables, the resolvers
// and the public functions) in math.hpp's order.
//
// On x86-64 the selection is made once: `active` starts at a table of
// resolvers, the first call through any of them selects the level, stores
// the chosen variant's table and forwards; every later call loads the
// pointer and calls the kernel, with no CPU check. Two threads making their
// first calls together both store the same table. Elsewhere the one native
// variant is called directly.

#include "stx_auxlib/math.hpp"

#if defined(STX_AUXLIB_MATH_DISPATCH_X86)

#include "stx_auxlib/simd.hpp"

#include <atomic>

#define STX_AUXLIB_MATH_DECLARE_VARIANT v128
#include "variant_functions.hpp"
#undef STX_AUXLIB_MATH_DECLARE_VARIANT
#define STX_AUXLIB_MATH_DECLARE_VARIANT avx2_fma
#include "variant_functions.hpp"
#undef STX_AUXLIB_MATH_DECLARE_VARIANT

namespace stx_auxlib::math {

namespace {

// One variant's entry points, in math.hpp's order (f: float, d: double).
struct Kernels final {
    void (*sincos_f)(float, float&, float&) noexcept = nullptr;
    void (*sincos_d)(double, double&, double&) noexcept = nullptr;
    float (*exp_f)(float) noexcept = nullptr;
    double (*exp_d)(double) noexcept = nullptr;
    double (*log_d)(double) noexcept = nullptr;
    float (*log_f)(float) noexcept = nullptr;
    double (*log2_d)(double) noexcept = nullptr;
    float (*log2_f)(float) noexcept = nullptr;
    double (*log10_d)(double) noexcept = nullptr;
    float (*log10_f)(float) noexcept = nullptr;
    double (*pow_d)(double, double) noexcept = nullptr;
    float (*pow_f)(float, float) noexcept = nullptr;
    double (*hypot_d)(double, double) noexcept = nullptr;
    float (*hypot_f)(float, float) noexcept = nullptr;
    double (*atan_d)(double) noexcept = nullptr;
    float (*atan_f)(float) noexcept = nullptr;
    double (*atan2_d)(double, double) noexcept = nullptr;
    float (*atan2_f)(float, float) noexcept = nullptr;
    double (*asin_d)(double) noexcept = nullptr;
    float (*asin_f)(float) noexcept = nullptr;
    double (*acos_d)(double) noexcept = nullptr;
    float (*acos_f)(float) noexcept = nullptr;
    double (*tan_d)(double) noexcept = nullptr;
    float (*tan_f)(float) noexcept = nullptr;
    double (*cbrt_d)(double) noexcept = nullptr;
    float (*cbrt_f)(float) noexcept = nullptr;
    double (*sinh_d)(double) noexcept = nullptr;
    float (*sinh_f)(float) noexcept = nullptr;
    double (*cosh_d)(double) noexcept = nullptr;
    float (*cosh_f)(float) noexcept = nullptr;
    double (*tanh_d)(double) noexcept = nullptr;
    float (*tanh_f)(float) noexcept = nullptr;
    double (*asinh_d)(double) noexcept = nullptr;
    float (*asinh_f)(float) noexcept = nullptr;
    double (*acosh_d)(double) noexcept = nullptr;
    float (*acosh_f)(float) noexcept = nullptr;
    double (*atanh_d)(double) noexcept = nullptr;
    float (*atanh_f)(float) noexcept = nullptr;
};

constexpr Kernels v128_kernels = {
    v128::sincos,
    v128::sincos,
    v128::exp,
    v128::exp,
    v128::log,
    v128::log,
    v128::log2,
    v128::log2,
    v128::log10,
    v128::log10,
    v128::pow,
    v128::pow,
    v128::hypot,
    v128::hypot,
    v128::atan,
    v128::atan,
    v128::atan2,
    v128::atan2,
    v128::asin,
    v128::asin,
    v128::acos,
    v128::acos,
    v128::tan,
    v128::tan,
    v128::cbrt,
    v128::cbrt,
    v128::sinh,
    v128::sinh,
    v128::cosh,
    v128::cosh,
    v128::tanh,
    v128::tanh,
    v128::asinh,
    v128::asinh,
    v128::acosh,
    v128::acosh,
    v128::atanh,
    v128::atanh,
};

constexpr Kernels avx2_fma_kernels = {
    avx2_fma::sincos,
    avx2_fma::sincos,
    avx2_fma::exp,
    avx2_fma::exp,
    avx2_fma::log,
    avx2_fma::log,
    avx2_fma::log2,
    avx2_fma::log2,
    avx2_fma::log10,
    avx2_fma::log10,
    avx2_fma::pow,
    avx2_fma::pow,
    avx2_fma::hypot,
    avx2_fma::hypot,
    avx2_fma::atan,
    avx2_fma::atan,
    avx2_fma::atan2,
    avx2_fma::atan2,
    avx2_fma::asin,
    avx2_fma::asin,
    avx2_fma::acos,
    avx2_fma::acos,
    avx2_fma::tan,
    avx2_fma::tan,
    avx2_fma::cbrt,
    avx2_fma::cbrt,
    avx2_fma::sinh,
    avx2_fma::sinh,
    avx2_fma::cosh,
    avx2_fma::cosh,
    avx2_fma::tanh,
    avx2_fma::tanh,
    avx2_fma::asinh,
    avx2_fma::asinh,
    avx2_fma::acosh,
    avx2_fma::acosh,
    avx2_fma::atanh,
    avx2_fma::atanh,
};

const Kernels* select_kernels() noexcept {
    const simd::Level level = simd::selected_level();
    if (level == simd::Level::avx2_fma) {
        return &avx2_fma_kernels;
    }
    return &v128_kernels;
}

const Kernels* resolve() noexcept;

void resolve_sincos_f(const float angle, float& s, float& c) noexcept {
    (*(*resolve()).sincos_f)(angle, s, c);
}

void resolve_sincos_d(const double x, double& s, double& c) noexcept {
    (*(*resolve()).sincos_d)(x, s, c);
}

float resolve_exp_f(const float x) noexcept {
    const float result = (*(*resolve()).exp_f)(x);
    return result;
}

double resolve_exp_d(const double x) noexcept {
    const double result = (*(*resolve()).exp_d)(x);
    return result;
}

double resolve_log_d(const double x) noexcept {
    const double result = (*(*resolve()).log_d)(x);
    return result;
}

float resolve_log_f(const float x) noexcept {
    const float result = (*(*resolve()).log_f)(x);
    return result;
}

double resolve_log2_d(const double x) noexcept {
    const double result = (*(*resolve()).log2_d)(x);
    return result;
}

float resolve_log2_f(const float x) noexcept {
    const float result = (*(*resolve()).log2_f)(x);
    return result;
}

double resolve_log10_d(const double x) noexcept {
    const double result = (*(*resolve()).log10_d)(x);
    return result;
}

float resolve_log10_f(const float x) noexcept {
    const float result = (*(*resolve()).log10_f)(x);
    return result;
}

double resolve_pow_d(const double x, const double y) noexcept {
    const double result = (*(*resolve()).pow_d)(x, y);
    return result;
}

float resolve_pow_f(const float x, const float y) noexcept {
    const float result = (*(*resolve()).pow_f)(x, y);
    return result;
}

double resolve_hypot_d(const double x, const double y) noexcept {
    const double result = (*(*resolve()).hypot_d)(x, y);
    return result;
}

float resolve_hypot_f(const float x, const float y) noexcept {
    const float result = (*(*resolve()).hypot_f)(x, y);
    return result;
}

double resolve_atan_d(const double x) noexcept {
    const double result = (*(*resolve()).atan_d)(x);
    return result;
}

float resolve_atan_f(const float x) noexcept {
    const float result = (*(*resolve()).atan_f)(x);
    return result;
}

double resolve_atan2_d(const double y, const double x) noexcept {
    const double result = (*(*resolve()).atan2_d)(y, x);
    return result;
}

float resolve_atan2_f(const float y, const float x) noexcept {
    const float result = (*(*resolve()).atan2_f)(y, x);
    return result;
}

double resolve_asin_d(const double x) noexcept {
    const double result = (*(*resolve()).asin_d)(x);
    return result;
}

float resolve_asin_f(const float x) noexcept {
    const float result = (*(*resolve()).asin_f)(x);
    return result;
}

double resolve_acos_d(const double x) noexcept {
    const double result = (*(*resolve()).acos_d)(x);
    return result;
}

float resolve_acos_f(const float x) noexcept {
    const float result = (*(*resolve()).acos_f)(x);
    return result;
}

double resolve_tan_d(const double x) noexcept {
    const double result = (*(*resolve()).tan_d)(x);
    return result;
}

float resolve_tan_f(const float x) noexcept {
    const float result = (*(*resolve()).tan_f)(x);
    return result;
}

double resolve_cbrt_d(const double x) noexcept {
    const double result = (*(*resolve()).cbrt_d)(x);
    return result;
}

float resolve_cbrt_f(const float x) noexcept {
    const float result = (*(*resolve()).cbrt_f)(x);
    return result;
}

double resolve_sinh_d(const double x) noexcept {
    const double result = (*(*resolve()).sinh_d)(x);
    return result;
}

float resolve_sinh_f(const float x) noexcept {
    const float result = (*(*resolve()).sinh_f)(x);
    return result;
}

double resolve_cosh_d(const double x) noexcept {
    const double result = (*(*resolve()).cosh_d)(x);
    return result;
}

float resolve_cosh_f(const float x) noexcept {
    const float result = (*(*resolve()).cosh_f)(x);
    return result;
}

double resolve_tanh_d(const double x) noexcept {
    const double result = (*(*resolve()).tanh_d)(x);
    return result;
}

float resolve_tanh_f(const float x) noexcept {
    const float result = (*(*resolve()).tanh_f)(x);
    return result;
}

double resolve_asinh_d(const double x) noexcept {
    const double result = (*(*resolve()).asinh_d)(x);
    return result;
}

float resolve_asinh_f(const float x) noexcept {
    const float result = (*(*resolve()).asinh_f)(x);
    return result;
}

double resolve_acosh_d(const double x) noexcept {
    const double result = (*(*resolve()).acosh_d)(x);
    return result;
}

float resolve_acosh_f(const float x) noexcept {
    const float result = (*(*resolve()).acosh_f)(x);
    return result;
}

double resolve_atanh_d(const double x) noexcept {
    const double result = (*(*resolve()).atanh_d)(x);
    return result;
}

float resolve_atanh_f(const float x) noexcept {
    const float result = (*(*resolve()).atanh_f)(x);
    return result;
}

constexpr Kernels resolving_kernels = {
    resolve_sincos_f,
    resolve_sincos_d,
    resolve_exp_f,
    resolve_exp_d,
    resolve_log_d,
    resolve_log_f,
    resolve_log2_d,
    resolve_log2_f,
    resolve_log10_d,
    resolve_log10_f,
    resolve_pow_d,
    resolve_pow_f,
    resolve_hypot_d,
    resolve_hypot_f,
    resolve_atan_d,
    resolve_atan_f,
    resolve_atan2_d,
    resolve_atan2_f,
    resolve_asin_d,
    resolve_asin_f,
    resolve_acos_d,
    resolve_acos_f,
    resolve_tan_d,
    resolve_tan_f,
    resolve_cbrt_d,
    resolve_cbrt_f,
    resolve_sinh_d,
    resolve_sinh_f,
    resolve_cosh_d,
    resolve_cosh_f,
    resolve_tanh_d,
    resolve_tanh_f,
    resolve_asinh_d,
    resolve_asinh_f,
    resolve_acosh_d,
    resolve_acosh_f,
    resolve_atanh_d,
    resolve_atanh_f,
};

std::atomic<const Kernels*> active{&resolving_kernels};

const Kernels* resolve() noexcept {
    const Kernels* selected = select_kernels();
    active.store(selected, std::memory_order_relaxed);
    return selected;
}

} // namespace

void sincos(const float angle, float& s, float& c) noexcept {
    (*(*active.load(std::memory_order_relaxed)).sincos_f)(angle, s, c);
}

void sincos(const double x, double& s, double& c) noexcept {
    (*(*active.load(std::memory_order_relaxed)).sincos_d)(x, s, c);
}

float exp(const float x) noexcept {
    const float result = (*(*active.load(std::memory_order_relaxed)).exp_f)(x);
    return result;
}

double exp(const double x) noexcept {
    const double result = (*(*active.load(std::memory_order_relaxed)).exp_d)(x);
    return result;
}

double log(const double x) noexcept {
    const double result = (*(*active.load(std::memory_order_relaxed)).log_d)(x);
    return result;
}

float log(const float x) noexcept {
    const float result = (*(*active.load(std::memory_order_relaxed)).log_f)(x);
    return result;
}

double log2(const double x) noexcept {
    const double result = (*(*active.load(std::memory_order_relaxed)).log2_d)(x);
    return result;
}

float log2(const float x) noexcept {
    const float result = (*(*active.load(std::memory_order_relaxed)).log2_f)(x);
    return result;
}

double log10(const double x) noexcept {
    const double result = (*(*active.load(std::memory_order_relaxed)).log10_d)(x);
    return result;
}

float log10(const float x) noexcept {
    const float result = (*(*active.load(std::memory_order_relaxed)).log10_f)(x);
    return result;
}

double pow(const double x, const double y) noexcept {
    const double result = (*(*active.load(std::memory_order_relaxed)).pow_d)(x, y);
    return result;
}

float pow(const float x, const float y) noexcept {
    const float result = (*(*active.load(std::memory_order_relaxed)).pow_f)(x, y);
    return result;
}

double hypot(const double x, const double y) noexcept {
    const double result = (*(*active.load(std::memory_order_relaxed)).hypot_d)(x, y);
    return result;
}

float hypot(const float x, const float y) noexcept {
    const float result = (*(*active.load(std::memory_order_relaxed)).hypot_f)(x, y);
    return result;
}

double atan(const double x) noexcept {
    const double result = (*(*active.load(std::memory_order_relaxed)).atan_d)(x);
    return result;
}

float atan(const float x) noexcept {
    const float result = (*(*active.load(std::memory_order_relaxed)).atan_f)(x);
    return result;
}

double atan2(const double y, const double x) noexcept {
    const double result = (*(*active.load(std::memory_order_relaxed)).atan2_d)(y, x);
    return result;
}

float atan2(const float y, const float x) noexcept {
    const float result = (*(*active.load(std::memory_order_relaxed)).atan2_f)(y, x);
    return result;
}

double asin(const double x) noexcept {
    const double result = (*(*active.load(std::memory_order_relaxed)).asin_d)(x);
    return result;
}

float asin(const float x) noexcept {
    const float result = (*(*active.load(std::memory_order_relaxed)).asin_f)(x);
    return result;
}

double acos(const double x) noexcept {
    const double result = (*(*active.load(std::memory_order_relaxed)).acos_d)(x);
    return result;
}

float acos(const float x) noexcept {
    const float result = (*(*active.load(std::memory_order_relaxed)).acos_f)(x);
    return result;
}

double tan(const double x) noexcept {
    const double result = (*(*active.load(std::memory_order_relaxed)).tan_d)(x);
    return result;
}

float tan(const float x) noexcept {
    const float result = (*(*active.load(std::memory_order_relaxed)).tan_f)(x);
    return result;
}

double cbrt(const double x) noexcept {
    const double result = (*(*active.load(std::memory_order_relaxed)).cbrt_d)(x);
    return result;
}

float cbrt(const float x) noexcept {
    const float result = (*(*active.load(std::memory_order_relaxed)).cbrt_f)(x);
    return result;
}

double sinh(const double x) noexcept {
    const double result = (*(*active.load(std::memory_order_relaxed)).sinh_d)(x);
    return result;
}

float sinh(const float x) noexcept {
    const float result = (*(*active.load(std::memory_order_relaxed)).sinh_f)(x);
    return result;
}

double cosh(const double x) noexcept {
    const double result = (*(*active.load(std::memory_order_relaxed)).cosh_d)(x);
    return result;
}

float cosh(const float x) noexcept {
    const float result = (*(*active.load(std::memory_order_relaxed)).cosh_f)(x);
    return result;
}

double tanh(const double x) noexcept {
    const double result = (*(*active.load(std::memory_order_relaxed)).tanh_d)(x);
    return result;
}

float tanh(const float x) noexcept {
    const float result = (*(*active.load(std::memory_order_relaxed)).tanh_f)(x);
    return result;
}

double asinh(const double x) noexcept {
    const double result = (*(*active.load(std::memory_order_relaxed)).asinh_d)(x);
    return result;
}

float asinh(const float x) noexcept {
    const float result = (*(*active.load(std::memory_order_relaxed)).asinh_f)(x);
    return result;
}

double acosh(const double x) noexcept {
    const double result = (*(*active.load(std::memory_order_relaxed)).acosh_d)(x);
    return result;
}

float acosh(const float x) noexcept {
    const float result = (*(*active.load(std::memory_order_relaxed)).acosh_f)(x);
    return result;
}

double atanh(const double x) noexcept {
    const double result = (*(*active.load(std::memory_order_relaxed)).atanh_d)(x);
    return result;
}

float atanh(const float x) noexcept {
    const float result = (*(*active.load(std::memory_order_relaxed)).atanh_f)(x);
    return result;
}

} // namespace stx_auxlib::math

#else

#define STX_AUXLIB_MATH_DECLARE_VARIANT native
#include "variant_functions.hpp"
#undef STX_AUXLIB_MATH_DECLARE_VARIANT

namespace stx_auxlib::math {

void sincos(const float angle, float& s, float& c) noexcept {
    native::sincos(angle, s, c);
}

void sincos(const double x, double& s, double& c) noexcept {
    native::sincos(x, s, c);
}

float exp(const float x) noexcept {
    const float result = native::exp(x);
    return result;
}

double exp(const double x) noexcept {
    const double result = native::exp(x);
    return result;
}

double log(const double x) noexcept {
    const double result = native::log(x);
    return result;
}

float log(const float x) noexcept {
    const float result = native::log(x);
    return result;
}

double log2(const double x) noexcept {
    const double result = native::log2(x);
    return result;
}

float log2(const float x) noexcept {
    const float result = native::log2(x);
    return result;
}

double log10(const double x) noexcept {
    const double result = native::log10(x);
    return result;
}

float log10(const float x) noexcept {
    const float result = native::log10(x);
    return result;
}

double pow(const double x, const double y) noexcept {
    const double result = native::pow(x, y);
    return result;
}

float pow(const float x, const float y) noexcept {
    const float result = native::pow(x, y);
    return result;
}

double hypot(const double x, const double y) noexcept {
    const double result = native::hypot(x, y);
    return result;
}

float hypot(const float x, const float y) noexcept {
    const float result = native::hypot(x, y);
    return result;
}

double atan(const double x) noexcept {
    const double result = native::atan(x);
    return result;
}

float atan(const float x) noexcept {
    const float result = native::atan(x);
    return result;
}

double atan2(const double y, const double x) noexcept {
    const double result = native::atan2(y, x);
    return result;
}

float atan2(const float y, const float x) noexcept {
    const float result = native::atan2(y, x);
    return result;
}

double asin(const double x) noexcept {
    const double result = native::asin(x);
    return result;
}

float asin(const float x) noexcept {
    const float result = native::asin(x);
    return result;
}

double acos(const double x) noexcept {
    const double result = native::acos(x);
    return result;
}

float acos(const float x) noexcept {
    const float result = native::acos(x);
    return result;
}

double tan(const double x) noexcept {
    const double result = native::tan(x);
    return result;
}

float tan(const float x) noexcept {
    const float result = native::tan(x);
    return result;
}

double cbrt(const double x) noexcept {
    const double result = native::cbrt(x);
    return result;
}

float cbrt(const float x) noexcept {
    const float result = native::cbrt(x);
    return result;
}

double sinh(const double x) noexcept {
    const double result = native::sinh(x);
    return result;
}

float sinh(const float x) noexcept {
    const float result = native::sinh(x);
    return result;
}

double cosh(const double x) noexcept {
    const double result = native::cosh(x);
    return result;
}

float cosh(const float x) noexcept {
    const float result = native::cosh(x);
    return result;
}

double tanh(const double x) noexcept {
    const double result = native::tanh(x);
    return result;
}

float tanh(const float x) noexcept {
    const float result = native::tanh(x);
    return result;
}

double asinh(const double x) noexcept {
    const double result = native::asinh(x);
    return result;
}

float asinh(const float x) noexcept {
    const float result = native::asinh(x);
    return result;
}

double acosh(const double x) noexcept {
    const double result = native::acosh(x);
    return result;
}

float acosh(const float x) noexcept {
    const float result = native::acosh(x);
    return result;
}

double atanh(const double x) noexcept {
    const double result = native::atanh(x);
    return result;
}

float atanh(const float x) noexcept {
    const float result = native::atanh(x);
    return result;
}

} // namespace stx_auxlib::math

#endif
