// Replacements for every replaceable global allocation function, routing
// operator new and delete to stx_auxlib's backend (memory_backend.hpp).
//
// Built as stx_auxlib_global_allocator, which only an executable links: the
// replacement is a decision for the whole program (memory.hpp, "One
// allocator per process").
//
// The semantics are the standard's: a throwing form calls the new-handler
// and retries until it allocates, and throws std::bad_alloc when no handler
// is installed; a nothrow form returns null where the throwing form would
// throw; array forms forward to the scalar forms; sized deletes ignore the
// size. A zero-byte request returns a unique non-null block.

#include "memory_backend.hpp"

#include <cstddef>
#include <new>

namespace {

void* allocate_or_throw(const std::size_t size) {
    for (;;) {
        void* const block = stx_auxlib::memory::backend_detail::allocate_block(size);
        if (block != nullptr) return block;
        const std::new_handler handler = std::get_new_handler();
        if (handler == nullptr) throw std::bad_alloc();
        handler();
    }
}

void* allocate_aligned_or_throw(const std::size_t size, const std::align_val_t alignment) {
    const std::size_t bytes = static_cast<std::size_t>(alignment);
    for (;;) {
        void* const block =
            stx_auxlib::memory::backend_detail::allocate_aligned_block(size, bytes);
        if (block != nullptr) return block;
        const std::new_handler handler = std::get_new_handler();
        if (handler == nullptr) throw std::bad_alloc();
        handler();
    }
}

void* allocate_or_null(const std::size_t size) noexcept {
    try {
        void* const block = allocate_or_throw(size);
        return block;
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
}

void* allocate_aligned_or_null(const std::size_t size, const std::align_val_t alignment) noexcept {
    try {
        void* const block = allocate_aligned_or_throw(size, alignment);
        return block;
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
}

void release(void* const address) noexcept {
    stx_auxlib::memory::backend_detail::release_block(address);
}

void release_aligned(void* const address) noexcept {
    stx_auxlib::memory::backend_detail::release_aligned_block(address);
}

} // namespace

void* operator new(const std::size_t size) { return allocate_or_throw(size); }
void* operator new[](const std::size_t size) { return allocate_or_throw(size); }
void* operator new(const std::size_t size, const std::nothrow_t&) noexcept {
    return allocate_or_null(size);
}
void* operator new[](const std::size_t size, const std::nothrow_t&) noexcept {
    return allocate_or_null(size);
}
void* operator new(const std::size_t size, const std::align_val_t alignment) {
    return allocate_aligned_or_throw(size, alignment);
}
void* operator new[](const std::size_t size, const std::align_val_t alignment) {
    return allocate_aligned_or_throw(size, alignment);
}
void* operator new(const std::size_t size, const std::align_val_t alignment,
                   const std::nothrow_t&) noexcept {
    return allocate_aligned_or_null(size, alignment);
}
void* operator new[](const std::size_t size, const std::align_val_t alignment,
                     const std::nothrow_t&) noexcept {
    return allocate_aligned_or_null(size, alignment);
}

void operator delete(void* const address) noexcept { release(address); }
void operator delete[](void* const address) noexcept { release(address); }
void operator delete(void* const address, const std::size_t) noexcept { release(address); }
void operator delete[](void* const address, const std::size_t) noexcept { release(address); }
void operator delete(void* const address, const std::nothrow_t&) noexcept { release(address); }
void operator delete[](void* const address, const std::nothrow_t&) noexcept {
    release(address);
}
void operator delete(void* const address, const std::align_val_t) noexcept {
    release_aligned(address);
}
void operator delete[](void* const address, const std::align_val_t) noexcept {
    release_aligned(address);
}
void operator delete(void* const address, const std::size_t, const std::align_val_t) noexcept {
    release_aligned(address);
}
void operator delete[](void* const address, const std::size_t, const std::align_val_t) noexcept {
    release_aligned(address);
}
void operator delete(void* const address, const std::align_val_t,
                     const std::nothrow_t&) noexcept {
    release_aligned(address);
}
void operator delete[](void* const address, const std::align_val_t,
                       const std::nothrow_t&) noexcept {
    release_aligned(address);
}
