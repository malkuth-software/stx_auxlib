#pragma once

// The primitives each memory backend supplies (memory_mimalloc.cpp,
// memory_host.cpp); exactly one is linked. memory.cpp builds the public
// operations on them.
//
// Every primitive is noexcept and reports exhaustion as null. Callers have
// already checked that alignment is a power of two. Blocks are released only
// by release_aligned_block.

#include "stx_auxlib/memory.hpp"

#include <cstddef>

namespace stx_auxlib::memory::backend_detail {

// The backend's identity, reported by memory::backend().
extern const Backend linked_backend;

// `size` bytes at `alignment`, any power of two; size zero is valid.
[[nodiscard]] void* allocate_aligned_block(std::size_t size, std::size_t alignment) noexcept;
void release_aligned_block(void* address) noexcept;

[[nodiscard]] bool owns_block(const void* address) noexcept;

} // namespace stx_auxlib::memory::backend_detail
