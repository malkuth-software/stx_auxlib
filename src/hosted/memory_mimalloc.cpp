// The mimalloc memory backend (third_party/mimalloc, compiled as
// stx_auxlib_mimalloc). mimalloc accepts every pointer it allocated in
// mi_free, aligned or not, and returns a unique block for size zero.

#include "memory_backend.hpp"

// Sanitizers track the host allocator only; a sanitizer build that linked
// this backend would stop checking the C++ heap (CMakeLists.txt selects the
// host backend for them).
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#error "the mimalloc backend is not admitted in sanitizer builds"
#endif
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#error "the mimalloc backend is not admitted in sanitizer builds"
#endif
#endif

#include <mimalloc.h>

#include <cstddef>

namespace stx_auxlib::memory::backend_detail {

const Backend linked_backend = Backend::mimalloc;

void* allocate_block(const std::size_t size) noexcept {
    void* const result = mi_malloc(size);
    return result;
}

void release_block(void* const address) noexcept { mi_free(address); }

void* allocate_aligned_block(const std::size_t size, const std::size_t alignment) noexcept {
    void* const result = mi_malloc_aligned(size, alignment);
    return result;
}

void release_aligned_block(void* const address) noexcept { mi_free(address); }

bool owns_block(const void* const address) noexcept {
    const bool result = mi_any_heap_contains(address);
    return result;
}

} // namespace stx_auxlib::memory::backend_detail
