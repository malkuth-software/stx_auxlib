#pragma once

// Private checked arithmetic and the one marker for intended modular
// arithmetic, shared by the library's sources.
//
// Every wraparound in stx_auxlib is either impossible by construction or
// marked: the integer-sanitizer build (CMakeLists.txt, STX_AUXLIB_INTEGER_CHECKS)
// halts on any unsigned wrap, signed overflow or value-changing conversion
// outside a function marked STX_AUXLIB_MODULAR. Only kernels whose
// definition is arithmetic modulo 2^n (ChaCha, SplitMix64, xoshiro) carry
// the mark.

#include <cstddef>
#include <limits>

#if defined(__clang__)
#define STX_AUXLIB_MODULAR                                                                    \
    __attribute__((no_sanitize("unsigned-integer-overflow", "unsigned-shift-base",          \
                               "implicit-integer-truncation")))
#else
#define STX_AUXLIB_MODULAR
#endif

namespace stx_auxlib::checked {

// left + right, or the largest size_t when that would wrap.
[[nodiscard]] constexpr std::size_t add_saturating(const std::size_t left, const std::size_t right) noexcept {
    if (left > std::numeric_limits<std::size_t>::max() - right) return std::numeric_limits<std::size_t>::max();
    const std::size_t result = left + right;
    return result;
}

// left * right, or the largest size_t when that would wrap.
[[nodiscard]] constexpr std::size_t multiply_saturating(const std::size_t left,
                                                        const std::size_t right) noexcept {
    if (left != 0 && right > std::numeric_limits<std::size_t>::max() / left)
        return std::numeric_limits<std::size_t>::max();
    const std::size_t result = left * right;
    return result;
}

} // namespace stx_auxlib::checked
