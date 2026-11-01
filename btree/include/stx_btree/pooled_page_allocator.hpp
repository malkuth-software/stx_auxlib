#pragma once

// A page allocator that pools pages by size class (bbtree's `Allocator`
// trait anticipates one, D8). Each class carves its pages in address order
// from chunks that grow from 64 KiB to 2 MiB, and keeps freed pages on a free
// list for reuse. The system allocator's aligned path places 1 KiB pages
// aligned to 1 KiB with gaps between them (glibc: 1.55 GB of address space
// for 0.76 GB of pages at 10M entries); packed pages took the unsync map's
// u64 inserts at 1M from 1.33x to 0.94x bbtree's time on Linux x86-64
// (docs/MEASUREMENTS.md).
//
// Ownership: every copy of a PooledPageAllocator shares one PagePool through
// a std::shared_ptr; the pool and its chunks live until the last copy goes,
// so a map owns its pool, and maps built from copies of one allocator share
// it. Pages return to the pool, never to the system, until the pool ends.
// Copying (also on move: there is no move constructor) keeps both copies
// usable.
//
// Threads: take and give_back hold the pool's mutex. The shared map calls
// them only when its own free lists are empty (D54), about once per leaf
// split, so the lock is off the operation path.
//
// Pages come zeroed, as GlobalPageAllocator's do; the latched map relies on
// it (D54).

#include "stx_btree/page_allocator.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include "stx_auxlib/threads.hpp"
#include <new>
#include <vector>

namespace stx_btree {

class PagePool final {
public:
    static constexpr std::size_t first_chunk_bytes = static_cast<std::size_t>(64) << 10;
    static constexpr std::size_t largest_chunk_bytes = static_cast<std::size_t>(2) << 20;

    // What the pool holds, for tests and measurement.
    struct Usage final {
        std::size_t chunk_bytes = 0;  // reserved from the system
        std::size_t pages_out = 0;    // handed out and not given back
        std::size_t pages_free = 0;   // on the free lists
    };

    PagePool() = default;
    PagePool(const PagePool&) = delete;
    PagePool& operator=(const PagePool&) = delete;

    ~PagePool() {
        std::size_t index = 0;
        for (index = 0; index < chunks.size(); index += 1) {
            ::operator delete(chunks[index].start, std::align_val_t(chunks[index].alignment));
        }
    }

    // A page of `bytes` aligned to `alignment` (a power of two), not zeroed.
    // Throws std::bad_alloc when the system has no chunk to give.
    [[nodiscard]] void* take(std::size_t bytes, std::size_t alignment) {
        const stx_auxlib::ScopedLock guard(mutex);
        SizeClass& size_class = class_for(bytes, alignment);
        if (size_class.free_top != nullptr) {
            std::byte* const page = size_class.free_top;
            size_class.free_top = read_link(page);
            size_class.free_count -= 1;
            size_class.out_count += 1;
            return page;
        }
        const std::size_t step = round_up(bytes, alignment);
        if (size_class.end - size_class.next < static_cast<std::ptrdiff_t>(step)) {
            add_chunk(size_class, step);
        }
        std::byte* const page = size_class.next;
        size_class.next += step;
        size_class.out_count += 1;
        return page;
    }

    // Takes back a page from take(bytes, alignment) of this pool.
    void give_back(void* page, std::size_t bytes, std::size_t alignment) noexcept {
        const stx_auxlib::ScopedLock guard(mutex);
        SizeClass* size_class = find_class(bytes, alignment);
        std::byte* const freed = static_cast<std::byte*>(page);
        write_link(freed, (*size_class).free_top);
        (*size_class).free_top = freed;
        (*size_class).free_count += 1;
        (*size_class).out_count -= 1;
    }

    [[nodiscard]] Usage usage() {
        const stx_auxlib::ScopedLock guard(mutex);
        Usage total;
        std::size_t index = 0;
        for (index = 0; index < chunks.size(); index += 1) {
            total.chunk_bytes += chunks[index].bytes;
        }
        for (index = 0; index < class_count; index += 1) {
            total.pages_out += classes[index].out_count;
            total.pages_free += classes[index].free_count;
        }
        return total;
    }

private:
    // A map uses three classes (leaf, internal, root pages); room for a few
    // maps with different layouts sharing one pool.
    static constexpr std::size_t class_capacity = 16;

    struct SizeClass final {
        std::size_t bytes = 0;
        std::size_t alignment = 0;
        std::byte* next = nullptr;      // the next uncarved page of the current chunk
        std::byte* end = nullptr;       // the current chunk's end
        std::byte* free_top = nullptr;  // freed pages, linked through their first bytes
        std::size_t next_chunk_bytes = first_chunk_bytes;
        std::size_t free_count = 0;
        std::size_t out_count = 0;
    };

    struct Chunk final {
        std::byte* start;
        std::size_t bytes;
        std::size_t alignment;
    };

    [[nodiscard]] static std::size_t round_up(std::size_t bytes, std::size_t alignment) noexcept {
        return (bytes + alignment - 1) / alignment * alignment;
    }

    [[nodiscard]] static std::byte* read_link(std::byte* page) noexcept {
        std::byte* next = nullptr;
        std::memcpy(&next, page, sizeof(next));
        return next;
    }

    static void write_link(std::byte* page, std::byte* next) noexcept {
        std::memcpy(page, &next, sizeof(next));
    }

    [[nodiscard]] SizeClass* find_class(std::size_t bytes, std::size_t alignment) noexcept {
        std::size_t index = 0;
        for (index = 0; index < class_count; index += 1) {
            if (classes[index].bytes == bytes && classes[index].alignment == alignment) {
                return &classes[index];
            }
        }
        return nullptr;
    }

    [[nodiscard]] SizeClass& class_for(std::size_t bytes, std::size_t alignment) {
        SizeClass* found = find_class(bytes, alignment);
        if (found != nullptr) {
            return *found;
        }
        if (class_count == class_capacity) {
            throw std::bad_alloc();
        }
        classes[class_count] = SizeClass{.bytes = bytes,
                                         .alignment = alignment,
                                         .next = nullptr,
                                         .end = nullptr,
                                         .free_top = nullptr,
                                         .next_chunk_bytes = first_chunk_bytes,
                                         .free_count = 0,
                                         .out_count = 0};
        class_count += 1;
        return classes[class_count - 1];
    }

    // A fresh chunk for the class: the next size in its doubling, at least
    // one page; the rest of the old chunk, less than a page, is left unused.
    void add_chunk(SizeClass& size_class, std::size_t step) {
        std::size_t bytes = size_class.next_chunk_bytes;
        if (bytes < step) {
            bytes = step;
        }
        bytes = round_up(bytes, step);
        chunks.reserve(chunks.size() + 1);
        std::byte* const start = static_cast<std::byte*>(::operator new(bytes, std::align_val_t(size_class.alignment)));
        chunks.push_back(Chunk{.start = start, .bytes = bytes, .alignment = size_class.alignment});
        size_class.next = start;
        size_class.end = start + bytes;
        if (size_class.next_chunk_bytes < largest_chunk_bytes) {
            size_class.next_chunk_bytes *= 2;
        }
    }

    // stx_auxlib adaptation: the pool lock is stx_auxlib's Mutex.
    stx_auxlib::Mutex mutex;
    SizeClass classes[class_capacity];
    std::size_t class_count = 0;
    std::vector<Chunk> chunks;
};

class PooledPageAllocator final {
public:
    PooledPageAllocator() : pool(std::make_shared<PagePool>()) {}

    // Shares `shared_pool` (not null) with other allocators and their maps.
    explicit PooledPageAllocator(std::shared_ptr<PagePool> shared_pool) noexcept : pool(std::move(shared_pool)) {}

    PooledPageAllocator(const PooledPageAllocator&) = default;
    PooledPageAllocator& operator=(const PooledPageAllocator&) = default;
    ~PooledPageAllocator() = default;

    [[nodiscard]] void* allocate_page(PageKind, std::size_t bytes, std::size_t alignment) const {
        void* page = (*pool).take(bytes, alignment);
        std::memset(page, 0, bytes);
        return page;
    }

    void deallocate_page(PageKind, void* page, std::size_t bytes, std::size_t alignment) const noexcept {
        (*pool).give_back(page, bytes, alignment);
    }

    [[nodiscard]] PagePool& page_pool() const noexcept {
        return *pool;
    }

    [[nodiscard]] bool operator==(const PooledPageAllocator& other) const noexcept {
        return pool == other.pool;
    }

private:
    std::shared_ptr<PagePool> pool;
};

} // namespace stx_btree
