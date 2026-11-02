#pragma once

// The floating-point constants the hot paths of the kernels read, gathered
// in one object defined in constants.cpp. Defined in another translation
// unit, they are loaded from memory (in pairs) instead of being rebuilt from
// immediates on every call: AArch64 compilers materialise a double constant
// with up to four integer moves and a register transfer, and pow alone
// rebuilt ~100 of them per call (measured: 189 of its 484 instructions).
// The values are copies of the constexpr definitions next to each kernel,
// so the results are unchanged; link-time optimisation may fold them back.

namespace stx_auxlib::math::detail {

struct KernelConstants final {
    double rounding_shift;
    double sincos_inverse_step;
    double sincos_step_1;
    double sincos_step_2;
    double sincos_step_3;
    double sin_3;
    double sin_5;
    double sin_7;
    double cos_4;
    double cos_6;
    double exp_inverse_step;
    double exp_step_1;
    double exp_step_2;
    double exp_step_3;
    double exp_3;
    double exp_4;
    double exp_5;
    double exp_6;
    double log_3;
    double log_4;
    double log_5;
    double log_6;
    double log_7;
    double log_8;
    double log_9;
    double log_10;
    double log_ln2_hi;
    double log_ln2_lo;
    // pow: 1/ln 2 and ln 2 as double-doubles (full-precision heads).
    double inverse_ln2_hi;
    double inverse_ln2_lo;
    double ln2_hi;
    double ln2_lo;
    // log_core.hpp's LogBase quads: natural, binary, decimal.
    double log_bases[12];
};

// Hidden: internal to the library, so references are direct. With default
// visibility, MinGW targets read the object's address through a .refptr
// pointer first, one extra load on every call (x86-64 review, 2026-11-01).
#if defined(__GNUC__) || defined(__clang__)
extern const KernelConstants kernel_constants __attribute__((visibility("hidden")));
#else
extern const KernelConstants kernel_constants;
#endif

} // namespace stx_auxlib::math::detail
