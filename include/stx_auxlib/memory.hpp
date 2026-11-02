#pragma once

// stx_auxlib's allocator, used by name: code that wants its storage from
// stx_auxlib (substantial or runtime-sized buffers held by explicit owners:
// heap arrays, arenas, pools) calls allocate and release here. Nothing is
// redirected to it: operator new and delete, malloc and free stay the
// toolchain's, in this program and in every module it loads.
//
// Ownership boundary (docs/ALLOCATOR_OWNERSHIP.md): only our code allocates
// from this allocator, and only our code releases to it. A block from here
// is never handed to free, operator delete or a foreign library's release
// call, and a block from anywhere else is never passed to release. Memory a
// foreign library or host API returns goes back through the release call its
// contract names.
//
// Backend: mimalloc (third_party/mimalloc, MIT) by default. Builds with
// address or thread sanitizers use the host's malloc instead, so the
// sanitizers still see and check every heap block; backend() says which is
// linked. The backend's state is private to the image stx_auxlib is linked
// into, so a block is released in the image that allocated it.
//
// Check mode: with STX_AUXLIB_ALLOCATION_TAGS (on in Debug and sanitizer
// builds by default), release fails the process with a message when given a
// block that is not from allocate or was already released, and a foreign
// free of a block from allocate traps. tags_checked() says whether this
// build checks.

#include <cstddef>
#include <cstdint>

namespace stx_auxlib::memory {

enum class Backend : std::uint8_t {
    mimalloc = 0,
    host = 1,
};

// The backend this program is linked with.
[[nodiscard]] Backend backend() noexcept;

// True when this build tags its blocks and checks them on release.
[[nodiscard]] bool tags_checked() noexcept;

// Allocates `size` bytes aligned to `alignment`, which must be a power of
// two. A size of zero yields a unique non-null block. Returns null when
// alignment is not a power of two, when the size cannot be represented with
// the alignment's bookkeeping, or when memory is exhausted. The block's
// contents are indeterminate. Never throws and never calls the new-handler.
[[nodiscard]] void* allocate(std::size_t size, std::size_t alignment) noexcept;

// Releases a block from allocate; null is a no-op. A block from anywhere
// else, or released twice, fails the process in the check mode and is
// undefined behaviour otherwise.
void release(void* address) noexcept;

// True when `address` lies in memory the mimalloc backend manages, as every
// block from allocate does with that backend. Always false with the host
// backend, which cannot answer. For tests and diagnostics; it does not
// validate a pointer for release.
[[nodiscard]] bool in_backend_heap(const void* address) noexcept;

} // namespace stx_auxlib::memory
