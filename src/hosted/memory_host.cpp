// The host memory backend: the C library's malloc and free. Sanitizer builds
// select it so the sanitizers see every heap block.
//
// Aligned blocks over-allocate and keep the malloc result in the pointer-
// sized slot just below the aligned address:
//
//   base ... [slot: base] [aligned block of `size` bytes] ... base + total
//
// total = size + alignment - 1 + sizeof(void*), checked for overflow. The
// slot may be unaligned for alignments below a pointer's, so it is read and
// written with memcpy.

#include "memory_backend.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace stx_auxlib::memory::backend_detail {

const Backend linked_backend = Backend::host;

void* allocate_block(const std::size_t size) noexcept {
    // malloc(0) may return null; a C++ allocation of zero bytes may not.
    const std::size_t requested = size == 0 ? 1 : size;
    void* const result = std::malloc(requested);
    return result;
}

void release_block(void* const address) noexcept { std::free(address); }

void* allocate_aligned_block(const std::size_t size, const std::size_t alignment) noexcept {
    constexpr std::size_t slot = sizeof(void*);
    const std::size_t padding = alignment - 1 + slot;
    if (size > std::numeric_limits<std::size_t>::max() - padding) return nullptr;
    const std::size_t total = size + padding;
    void* const base = std::malloc(total);
    if (base == nullptr) return nullptr;
    std::byte* const bytes = static_cast<std::byte*>(base);
    const std::uintptr_t first_usable = reinterpret_cast<std::uintptr_t>(bytes + slot);
    const std::size_t misalignment = static_cast<std::size_t>(first_usable & (alignment - 1));
    const std::size_t shift = misalignment == 0 ? 0 : alignment - misalignment;
    std::byte* const aligned = bytes + slot + shift;
    std::memcpy(aligned - slot, &base, slot);
    return aligned;
}

void release_aligned_block(void* const address) noexcept {
    if (address == nullptr) return;
    void* base = nullptr;
    const std::byte* const aligned = static_cast<const std::byte*>(address);
    std::memcpy(&base, aligned - sizeof(void*), sizeof(void*));
    std::free(base);
}

bool owns_block(const void* const) noexcept { return false; }

} // namespace stx_auxlib::memory::backend_detail
