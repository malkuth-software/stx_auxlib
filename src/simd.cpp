// The process's SIMD level (include/stx_auxlib/simd.hpp): CPU detection on
// x86-64 with CPUID and XGETBV, NEON as the arm64 baseline, and the
// once-per-process selection with its overrides.

#include "stx_auxlib/simd.hpp"

#include "stx_auxlib/environment.hpp"

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <string>

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#include <cpuid.h>
#define STX_AUXLIB_SIMD_X86_64 1
#endif

namespace stx_auxlib::simd {

namespace {

#if defined(STX_AUXLIB_SIMD_X86_64)
// CPUID leaf 1, ECX and EDX bits; leaf 7 subleaf 0, EBX bit.
constexpr unsigned int leaf1_edx_sse2 = 1U << 26U;
constexpr unsigned int leaf1_ecx_fma = 1U << 12U;
constexpr unsigned int leaf1_ecx_osxsave = 1U << 27U;
constexpr unsigned int leaf1_ecx_avx = 1U << 28U;
constexpr unsigned int leaf7_ebx_avx2 = 1U << 5U;
// XCR0: the operating system saves the SSE (bit 1) and AVX (bit 2) state.
constexpr std::uint64_t xcr0_sse_and_avx = 0x6U;

// XCR0, read only after CPUID reports OSXSAVE (XGETBV faults otherwise).
std::uint64_t read_xcr0() noexcept {
    unsigned int low = 0;
    unsigned int high = 0;
    __asm__ volatile("xgetbv" : "=a"(low), "=d"(high) : "c"(0U));
    const std::uint64_t result = (static_cast<std::uint64_t>(high) << 32U) | low;
    return result;
}

Features detect() noexcept {
    Features features;
    unsigned int eax = 0;
    unsigned int ebx = 0;
    unsigned int ecx = 0;
    unsigned int edx = 0;
    if (__get_cpuid(1U, &eax, &ebx, &ecx, &edx) == 0) {
        return features;
    }
    features.sse2 = (edx & leaf1_edx_sse2) != 0U;
    const bool osxsave = (ecx & leaf1_ecx_osxsave) != 0U;
    const bool avx_state = osxsave && (read_xcr0() & xcr0_sse_and_avx) == xcr0_sse_and_avx;
    features.avx = avx_state && (ecx & leaf1_ecx_avx) != 0U;
    // FMA3 uses the AVX registers, so it needs the same saved state.
    features.fma = features.avx && (ecx & leaf1_ecx_fma) != 0U;
    unsigned int leaf7_eax = 0;
    unsigned int leaf7_ebx = 0;
    unsigned int leaf7_ecx = 0;
    unsigned int leaf7_edx = 0;
    if (__get_cpuid_count(7U, 0U, &leaf7_eax, &leaf7_ebx, &leaf7_ecx, &leaf7_edx) != 0) {
        features.avx2 = features.avx && (leaf7_ebx & leaf7_ebx_avx2) != 0U;
    }
    return features;
}
#else
Features detect() noexcept {
    Features features;
#if defined(__aarch64__) || defined(_M_ARM64)
    // Advanced SIMD is part of the AArch64 baseline.
    features.neon = true;
#endif
    return features;
}
#endif

Level level_of(const Features& features) noexcept {
#if defined(STX_AUXLIB_SIMD_X86_64)
    if (features.avx2 && features.fma) {
        return Level::avx2_fma;
    }
    if (features.sse2) {
        return Level::vector128;
    }
#else
    if (features.neon) {
        return Level::vector128;
    }
#endif
    return Level::scalar;
}

// The selection state, one atomic word so that a request and the selection
// cannot disagree: `unset`, a request (requested_offset + level) or the
// selected level itself (0 to 2). A request can replace a request; nothing
// replaces a selection.
constexpr int unset = -1;
constexpr int requested_offset = 16;

std::atomic<int> selection{unset};

bool is_selected(const int state) noexcept {
    const bool result = state >= static_cast<int>(Level::scalar) && state <= static_cast<int>(Level::avx2_fma);
    return result;
}

bool is_requested(const int state) noexcept {
    const bool result = state >= requested_offset + static_cast<int>(Level::scalar) &&
                        state <= requested_offset + static_cast<int>(Level::avx2_fma);
    return result;
}

// STX_SIMD_LEVEL, read through the library's environment service, which
// serializes with stx_auxlib::set_variable (the host's getenv does not).
// The limit refuses any longer value before it is copied, so the value stays
// in the string's inline storage; the Windows backend's short conversion
// buffers can still allocate, and failing to is fatal here (noexcept), as
// for any exhaustion of the interface's own structures.
int level_from_environment() noexcept {
    constexpr std::size_t value_limit = 15;
    std::string value;
    const EnvironmentStatus status = read_variable("STX_SIMD_LEVEL", value_limit, value);
    if (status != EnvironmentStatus::ok) {
        return unset;
    }
    if (value == "scalar") {
        return static_cast<int>(Level::scalar);
    }
    if (value == "vector128") {
        return static_cast<int>(Level::vector128);
    }
    if (value == "avx2_fma") {
        return static_cast<int>(Level::avx2_fma);
    }
    return unset;
}

} // namespace

const Features& detected_features() noexcept {
    // Initialised once, thread-safely, by the first caller.
    static const Features detected = detect();
    return detected;
}

Level hardware_level() noexcept {
    const Level result = level_of(detected_features());
    return result;
}

Level selected_level() noexcept {
    int state = selection.load(std::memory_order_acquire);
    // Loops only when another thread changed the state in between: a newer
    // request is honoured, a selection is adopted.
    while (!is_selected(state)) {
        const int hardware = static_cast<int>(hardware_level());
        const int wanted = is_requested(state) ? state - requested_offset : level_from_environment();
        const int chosen = (wanted == unset || wanted > hardware) ? hardware : wanted;
        if (selection.compare_exchange_weak(state, chosen, std::memory_order_acq_rel,
                                            std::memory_order_acquire)) {
            return static_cast<Level>(chosen);
        }
    }
    return static_cast<Level>(state);
}

bool request_level(const Level level) noexcept {
    const int value = static_cast<int>(level);
    if (!is_selected(value)) {
        return false;  // not a Level enumerator
    }
    int state = selection.load(std::memory_order_acquire);
    while (!is_selected(state)) {
        if (selection.compare_exchange_weak(state, requested_offset + value, std::memory_order_acq_rel,
                                            std::memory_order_acquire)) {
            return true;
        }
    }
    return false;
}

const char* level_name(const Level level) noexcept {
    switch (level) {
    case Level::scalar:
        return "scalar";
    case Level::vector128:
#if defined(__aarch64__) || defined(_M_ARM64)
        return "neon-128";
#else
        return "sse2-128";
#endif
    case Level::avx2_fma:
        return "avx2-fma-256";
    }
    return "scalar";
}

} // namespace stx_auxlib::simd
