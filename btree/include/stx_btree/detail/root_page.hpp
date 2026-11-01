#pragma once

// The root page (bbtree root_page.rs, D50, D52): room for a node of either
// kind, then the tree's shared prefix, the leading encoding bytes every key
// shares, which stand in for the root's missing fences. A tree keeps one root
// page for life: raising the tree moves the root node out to a fresh page and
// makes the root internal above it; lowering moves the only child back in.

#include "stx_btree/detail/node.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace stx_btree::detail {

// A copy of the shared prefix, for searches and refits.
struct SharedPrefixBytes final {
    std::size_t length = 0;
    std::uint8_t bytes[shared_prefix_capacity] = {};
};

template <typename Shape>
class RootPage final {
public:
    using Leaf = LeafNode<Shape>;
    using Internal = InternalNode<Shape>;
    using Traits = typename Shape::Traits;
    using Query = typename Shape::Query;
    using Key = typename Shape::KeyType;
    using Value = typename Shape::ValueType;

    std::byte* page = nullptr;

    [[nodiscard]] std::uint8_t* shared_length_byte() const noexcept {
        return reinterpret_cast<std::uint8_t*>(page + Shape::root.shared_prefix_offset);
    }
    [[nodiscard]] std::uint8_t* shared_bytes() const noexcept {
        return shared_length_byte() + 1;
    }
    [[nodiscard]] std::size_t shared_length() const noexcept {
        if constexpr (Shape::latched) {
            const std::atomic_ref<std::uint8_t> field(*shared_length_byte());
            return field.load(std::memory_order_relaxed);
        } else {
            return *shared_length_byte();
        }
    }
    void set_shared_length(std::size_t length) const noexcept {
        const std::uint8_t narrow = static_cast<std::uint8_t>(length);
        if constexpr (Shape::latched) {
            const std::atomic_ref<std::uint8_t> field(*shared_length_byte());
            field.store(narrow, std::memory_order_relaxed);
        } else {
            *shared_length_byte() = narrow;
        }
    }

    // Copies the shared prefix out, as raw bytes (a latched reader validates
    // the copy with the root's version).
    void copy_shared_prefix(SharedPrefixBytes* out) const noexcept {
        const std::size_t length = shared_length();
        const std::size_t bounded = length < shared_prefix_capacity ? length : shared_prefix_capacity;
        (*out).length = bounded;
        std::memcpy((*out).bytes, shared_bytes(), bounded);
    }

    // Starts the shared prefix from a tree's first key.
    void start_shared_prefix(const Query& query) const noexcept {
        const std::size_t length = Traits::leading_bytes(query, shared_bytes(), shared_prefix_capacity);
        set_shared_length(length);
    }

    // The window of the shared bytes at `offset`, before they shrink to it.
    [[nodiscard]] Window shared_window_at(std::size_t offset) const noexcept {
        const std::size_t length = shared_length();
        const InlineWord word = ByteEncoding::window(shared_bytes(), length, offset);
        return Window{.prefix_length = offset, .word = word};
    }

    // Initializes a zeroed page as an empty leaf root with no shared prefix.
    static NodePointer initialize_empty(std::byte* page) noexcept {
        const Leaf leaf = Leaf::initialize(page, 0);
        const RootPage root = RootPage{page};
        root.set_shared_length(0);
        return leaf.pointer();
    }
};

// Whole-node moves between pages, used to raise and lower the tree. Keys
// and values are relocated, not copied byte for byte; a latched page keeps
// its own latch word (D52, D54).
template <typename Shape>
class NodeMove final {
public:
    using Leaf = LeafNode<Shape>;
    using Internal = InternalNode<Shape>;
    using Key = typename Shape::KeyType;
    using Value = typename Shape::ValueType;

    static void copy_fixed_bytes(std::byte* source, std::byte* destination, std::size_t end) noexcept {
        if constexpr (Shape::latched) {
            std::memcpy(destination, source, latch_word_offset);
            std::memcpy(destination + Shape::header_bytes, source + Shape::header_bytes, end - Shape::header_bytes);
        } else {
            std::memcpy(destination, source, end);
        }
    }

    [[nodiscard]] static Leaf move_leaf(Leaf source, std::byte* destination) noexcept {
        const Leaf target = Leaf{destination};
        copy_fixed_bytes(source.page, destination, Shape::leaf.entries_offset);
        const std::size_t length = source.length();
        if (source.has_head()) {
            source.move_entries(0, target, 0, 1);
        }
        source.move_entries(1, target, 1, length);
        return target;
    }

    [[nodiscard]] static Internal move_internal(Internal source, std::byte* destination) noexcept {
        const Internal target = Internal{destination};
        const std::size_t length = source.length();
        copy_fixed_bytes(source.page, destination, Shape::internal.edges_offset);
        source.move_edges(0, target, 0, length + 1);
        return target;
    }
};

} // namespace stx_btree::detail
