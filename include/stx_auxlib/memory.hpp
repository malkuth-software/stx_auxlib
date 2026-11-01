#pragma once

// The program's one allocator. Every C++ allocation (operator new and
// delete in all their forms, and so every standard container and string)
// reaches it when the program links stx_auxlib::global_allocator, and code
// that needs raw storage calls allocate and release here instead of malloc
// and free.
//
// Backend: mimalloc (third_party/mimalloc, MIT) by default. Builds with
// address or thread sanitizers use the host's malloc instead and leave
// operator new and delete to the toolchain, so the sanitizers still see and
// check every heap block; backend() says which is linked.
//
// One allocator per process: the allocator's state is not shared between
// images. Link stx_auxlib into the executable, or into exactly one shared
// library that every other image uses; two copies in one process would each
// receive blocks the other allocated. Memory obtained from another
// allocator (a C library's malloc, a host API) is released by that
// allocator, never here.

#include <cstddef>
#include <cstdint>

namespace stx_auxlib::memory {

enum class Backend : std::uint8_t {
    mimalloc = 0,
    host = 1,
};

// The backend this program is linked with.
[[nodiscard]] Backend backend() noexcept;

// Allocates `size` bytes aligned to `alignment`, which must be a power of
// two. A size of zero yields a unique non-null block. Returns null when
// alignment is not a power of two, when the size cannot be represented with
// the alignment's bookkeeping, or when memory is exhausted. The block's
// contents are indeterminate. Never throws and never calls the new-handler.
[[nodiscard]] void* allocate(std::size_t size, std::size_t alignment) noexcept;

// Releases a block from allocate; null is a no-op. A block from anywhere
// else, or released twice, is undefined behaviour.
void release(void* address) noexcept;

// True when `address` lies in memory the mimalloc backend manages: a block
// from allocate or, with stx_auxlib::global_allocator linked, from operator
// new. Always false with the host backend, which cannot answer. For tests
// and diagnostics; it does not validate a pointer for release.
[[nodiscard]] bool in_backend_heap(const void* address) noexcept;

} // namespace stx_auxlib::memory
