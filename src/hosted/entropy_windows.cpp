// Host entropy on Windows (include/stx_auxlib/random.hpp): BCryptGenRandom
// with the system-preferred generator. Fails closed: no fallback source.

#include "stx_auxlib/random.hpp"

#include <cstddef>
#include <cstring>
#include <span>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

namespace stx_auxlib::random {

bool fill_entropy(const std::span<std::byte> output) noexcept {
    // BCryptGenRandom takes a ULONG count; larger requests go in pieces.
    constexpr std::size_t largest_request = std::size_t{1} << 30;
    std::size_t filled = 0;
    while (filled < output.size()) {
        const std::size_t remaining = output.size() - filled;
        const std::size_t request = remaining < largest_request ? remaining : largest_request;
        const NTSTATUS status =
            ::BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(output.data() + filled),
                              static_cast<ULONG>(request), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (status < 0) {
            std::memset(output.data(), 0, output.size());
            return false;
        }
        filled += request;
    }
    return true;
}

} // namespace stx_auxlib::random
