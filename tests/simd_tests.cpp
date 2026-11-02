// Tests of the process's SIMD level (simd.hpp): the detected features are
// consistent, the hardware level matches them, a request made before the
// first selection lowers the level and fixes it, and a request after that
// changes nothing. Prints the detection so CI logs show which tier ran.
//
// Run without STX_SIMD_LEVEL in the environment: the test makes its own
// request and checks the level that results.

#include "stx_auxlib/simd.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

namespace simd = stx_auxlib::simd;

void require(const bool condition, const char* const message) {
    if (condition) return;
    std::fprintf(stderr, "stx_auxlib simd: %s\n", message);
    std::abort();
}

void features_are_consistent() {
    const simd::Features& features = simd::detected_features();
    require(!features.avx2 || features.avx, "AVX2 without AVX state");
    require(!features.fma || features.avx, "FMA without AVX state");
#if defined(__x86_64__)
    require(features.sse2, "an x86-64 CPU without SSE2");
    require(!features.neon, "NEON on x86-64");
#elif defined(__aarch64__)
    require(features.neon, "an arm64 CPU without NEON");
    require(!features.avx && !features.avx2 && !features.fma, "x86 features on arm64");
#endif
    const simd::Level hardware = simd::hardware_level();
    const bool full = features.avx2 && features.fma;
    require((hardware == simd::Level::avx2_fma) == full, "hardware level disagrees with AVX2 and FMA");
#if defined(__x86_64__) || defined(__aarch64__)
    require(hardware != simd::Level::scalar, "a 64-bit desktop CPU without a 128-bit level");
#endif
    std::printf("simd: sse2=%d neon=%d avx=%d avx2=%d fma=%d hardware=%s\n",
                features.sse2 ? 1 : 0, features.neon ? 1 : 0, features.avx ? 1 : 0,
                features.avx2 ? 1 : 0, features.fma ? 1 : 0, simd::level_name(hardware));
}

void names() {
    require(std::strcmp(simd::level_name(simd::Level::scalar), "scalar") == 0, "scalar name");
    require(std::strcmp(simd::level_name(simd::Level::avx2_fma), "avx2-fma-256") == 0, "avx2 name");
    const char* const vector = simd::level_name(simd::Level::vector128);
    require(std::strcmp(vector, "sse2-128") == 0 || std::strcmp(vector, "neon-128") == 0,
            "vector128 name");
}

void request_fixes_the_level() {
    require(std::getenv("STX_SIMD_LEVEL") == nullptr, "run without STX_SIMD_LEVEL");
    require(simd::request_level(simd::Level::scalar), "a request before selection was refused");
    const simd::Level first = simd::selected_level();
    require(first == simd::Level::scalar, "the request did not lower the level");
    require(!simd::request_level(simd::Level::avx2_fma), "a request after selection was accepted");
    require(simd::selected_level() == first, "the selected level changed");
}

} // namespace

int main() {
    features_are_consistent();
    names();
    request_fixes_the_level();
    std::printf("stx_auxlib simd tests passed\n");
    return 0;
}
