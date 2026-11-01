// stx_auxlib::memory over the linked backend (memory_backend.hpp).

#include "stx_auxlib/memory.hpp"

#include "memory_backend.hpp"

#include <bit>
#include <cstddef>

namespace stx_auxlib::memory {

Backend backend() noexcept {
    const Backend result = backend_detail::linked_backend;
    return result;
}

// Every block from allocate is an aligned block, whatever its alignment, so
// release needs no record of how it was requested.
void* allocate(const std::size_t size, const std::size_t alignment) noexcept {
    if (!std::has_single_bit(alignment)) return nullptr;
    void* const result = backend_detail::allocate_aligned_block(size, alignment);
    return result;
}

void release(void* const address) noexcept { backend_detail::release_aligned_block(address); }

bool in_backend_heap(const void* const address) noexcept {
    const bool result = backend_detail::owns_block(address);
    return result;
}

} // namespace stx_auxlib::memory
