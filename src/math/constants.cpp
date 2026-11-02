// The definition of kernel_constants (constants.hpp): copies of each
// kernel's constexpr constants, kept in their own translation unit so the
// kernels load them instead of rebuilding them.

#include "constants.hpp"

#include "exp_core.hpp"
#include "log_core.hpp"
#include "sincos_core.hpp"

namespace stx_auxlib::math::detail {

// The constexpr originals, as this translation unit's variant compiled them
// (the values are the same in every variant).
using namespace ::stx_auxlib::math::STX_AUXLIB_MATH_VARIANT::detail;

alignas(64) extern const KernelConstants kernel_constants = {
    rounding_shift,
    sincos_inverse_step,
    sincos_step_1,
    sincos_step_2,
    sincos_step_3,
    sin_3,
    sin_5,
    sin_7,
    cos_4,
    cos_6,
    tan_3,
    tan_5,
    tan_7,
    tan_9,
    tan_11,
    exp_inverse_step,
    exp_step_1,
    exp_step_2,
    exp_step_3,
    exp_3,
    exp_4,
    exp_5,
    exp_6,
    log_3,
    log_4,
    log_5,
    log_6,
    log_7,
    log_8,
    log_9,
    log_10,
    log_ln2_hi,
    log_ln2_lo,
    log_inv_ln2_hi,
    log_inv_ln2_lo,
    0x1.62e42fefa39efp-1,
    0x1.abc9e3b39803fp-56,
    {log_ln2_hi, log_ln2_lo, 1.0, 0.0,
     1.0, 0.0, log_inv_ln2_hi, log_inv_ln2_lo,
     log_log10_2_hi, log_log10_2_lo, log_inv_ln10_hi, log_inv_ln10_lo},
};

} // namespace stx_auxlib::math::detail
