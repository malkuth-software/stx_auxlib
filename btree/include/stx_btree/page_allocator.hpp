#pragma once

// Every allocation a map makes is a node page of a kind and a layout known at
// compile time (bbtree D8): leaves, internal nodes and the root page. Pages
// come zeroed, aligned to their size class; the latched map relies on zeroed
// latch words and edges (D54).

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

namespace stx_btree {

enum class PageKind : std::uint8_t { leaf, internal, root };

// The default allocator: the global aligned operator new, zero-filled.
// Allocation failure throws std::bad_alloc; the maps leave their contents
// unchanged when it does.
class GlobalPageAllocator final {
public:
    [[nodiscard]] void* allocate_page(PageKind, std::size_t bytes, std::size_t alignment) const {
        void* page = ::operator new(bytes, std::align_val_t(alignment));
        std::memset(page, 0, bytes);
        return page;
    }

    // The sized form only where the compiler declares it: Clang before 19
    // leaves sized deallocation off by default (found building with Clang 18
    // on Linux), and the unsized aligned form frees the same page.
    void deallocate_page(PageKind, void* page, std::size_t bytes, std::size_t alignment) const noexcept {
#if defined(__cpp_sized_deallocation)
        ::operator delete(page, bytes, std::align_val_t(alignment));
#else
        (void)bytes;
        ::operator delete(page, std::align_val_t(alignment));
#endif
    }

    [[nodiscard]] bool operator==(const GlobalPageAllocator&) const noexcept {
        return true;
    }
};

} // namespace stx_btree
