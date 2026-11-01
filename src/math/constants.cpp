// The definition of kernel_constants (constants.hpp): copies of each
// kernel's constexpr constants, kept in their own translation unit so the
// kernels load them instead of rebuilding them.

#include "constants.hpp"

#include "exp_core.hpp"
#include "log_core.hpp"
#include "sincos_core.hpp"

namespace stx_auxlib::math::detail {

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
    one_third_hi,
    one_third_lo,
    {log_ln2_hi, log_ln2_lo, 1.0, 0.0,
     1.0, 0.0, log_inv_ln2_hi, log_inv_ln2_lo,
     log_log10_2_hi, log_log10_2_lo, log_inv_ln10_hi, log_inv_ln10_lo},
};

} // namespace stx_auxlib::math::detail
