// Host entropy on POSIX hosts (include/stx_auxlib/random.hpp). Apple hosts
// and the BSDs use arc4random_buf, a kernel-seeded generator in user space
// that cannot fail and avoids a system call per request (it is what
// std::random_device uses there). Other hosts use getentropy (Linux: glibc
// 2.25 and later, musl), at most 256 bytes per call, which reads the
// kernel's generator. Fails closed: no fallback source.

#include "stx_auxlib/random.hpp"

#include <cstddef>
#include <cstring>
#include <span>

#include <stdlib.h>
#include <sys/random.h>
#include <unistd.h>

namespace stx_auxlib::random {

bool fill_entropy(const std::span<std::byte> output) noexcept {
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    ::arc4random_buf(output.data(), output.size());
    return true;
#else
    constexpr std::size_t largest_request = 256;
    std::size_t filled = 0;
    while (filled < output.size()) {
        const std::size_t remaining = output.size() - filled;
        const std::size_t request = remaining < largest_request ? remaining : largest_request;
        if (::getentropy(output.data() + filled, request) != 0) {
            std::memset(output.data(), 0, output.size());
            return false;
        }
        filled += request;
    }
    return true;
#endif
}

} // namespace stx_auxlib::random
