#pragma once

// The single-threaded tree (bbtree tree/root.rs, search.rs, insert.rs and the
// unsync map's tree operations). It owns its pages through its allocator.

#include "stx_btree/detail/batch.hpp"
#include "stx_btree/detail/node.hpp"
#include "stx_btree/detail/node_stack.hpp"
#include "stx_btree/detail/root_page.hpp"
#include "stx_btree/detail/tree_algorithms.hpp"
#include "stx_btree/detail/tree_walk.hpp"
#include "stx_btree/page_allocator.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace stx_btree::detail {

template <typename Shape, typename Allocator>
class UnsyncTree final {
public:
    using Key = typename Shape::KeyType;
    using Value = typename Shape::ValueType;
    using Query = typename Shape::Query;
    using Traits = typename Shape::Traits;
    using Leaf = LeafNode<Shape>;
    using Internal = InternalNode<Shape>;
    using Stack = NodeStack<Shape>;
    using Root = RootPage<Shape>;
    using Algorithms = TreeAlgorithms<Shape, UnsyncTree>;
    using Found = EntryPointers<Key, Value>;

    UnsyncTree() noexcept = default;
    explicit UnsyncTree(const Allocator& allocator) noexcept : page_allocator(allocator) {}
    UnsyncTree(const UnsyncTree&) = delete;
    UnsyncTree& operator=(const UnsyncTree&) = delete;
    UnsyncTree(UnsyncTree&& other) noexcept
        : root_pointer(other.root_pointer), rightmost_leaf(other.rightmost_leaf),
          entry_count(other.entry_count), page_allocator(std::move(other.page_allocator)) {
        other.root_pointer = NodePointer{};
        other.rightmost_leaf = NodePointer{};
        other.entry_count = 0;
    }
    UnsyncTree& operator=(UnsyncTree&& other) noexcept {
        if (this != &other) {
            release_all();
            root_pointer = other.root_pointer;
            rightmost_leaf = other.rightmost_leaf;
            entry_count = other.entry_count;
            page_allocator = std::move(other.page_allocator);
            other.root_pointer = NodePointer{};
            other.rightmost_leaf = NodePointer{};
            other.entry_count = 0;
        }
        return *this;
    }
    ~UnsyncTree() {
        release_all();
    }

    // --- Host interface for TreeAlgorithms ---------------------------------

    [[nodiscard]] NodePointer root() const noexcept {
        return root_pointer;
    }
    [[nodiscard]] Root root_page() const noexcept {
        return Root{root_pointer.page()};
    }
    [[nodiscard]] std::size_t shared_length() const noexcept {
        return root_page().shared_length();
    }
    [[nodiscard]] std::byte* allocate_page(PageKind kind) {
        if (kind == PageKind::leaf) {
            void* page = page_allocator.allocate_page(kind, Shape::leaf.page_bytes, Shape::leaf.page_alignment);
            return static_cast<std::byte*>(page);
        }
        if (kind == PageKind::internal) {
            void* page = page_allocator.allocate_page(kind, Shape::internal.page_bytes, Shape::internal.page_alignment);
            return static_cast<std::byte*>(page);
        }
        void* page = page_allocator.allocate_page(kind, Shape::root.page_bytes, Shape::root.page_alignment);
        return static_cast<std::byte*>(page);
    }
    [[nodiscard]] std::byte* take_page(PageReserve& reserve, PageKind kind) noexcept {
        return reserve.take(kind);
    }
    void release_page(PageKind kind, std::byte* page) noexcept {
        if (kind == PageKind::leaf) {
            page_allocator.deallocate_page(kind, page, Shape::leaf.page_bytes, Shape::leaf.page_alignment);
        } else if (kind == PageKind::internal) {
            page_allocator.deallocate_page(kind, page, Shape::internal.page_bytes, Shape::internal.page_alignment);
        } else {
            page_allocator.deallocate_page(kind, page, Shape::root.page_bytes, Shape::root.page_alignment);
        }
    }

    // Moves the root node out to a reserved page and makes the root an
    // internal node above it (bbtree RootPage::raise).
    NodePointer raise_root(PageReserve& reserve) noexcept {
        const Root root = root_page();
        NodePointer child;
        if (root_pointer.is_leaf()) {
            const Leaf moved = NodeMove<Shape>::move_leaf(Leaf::at(root_pointer), take_page(reserve, PageKind::leaf));
            child = moved.pointer();
        } else {
            const Internal moved = NodeMove<Shape>::move_internal(Internal::at(root_pointer), take_page(reserve, PageKind::internal));
            child = moved.pointer();
        }
        const Internal top = Internal::initialize(root.page, child, root.shared_length());
        root_pointer = top.pointer();
        return root_pointer;
    }

    // Moves an emptied internal root's only child into the root page.
    void lower_root() noexcept {
        const Root root = root_page();
        const NodePointer child = Internal::at(root_pointer).edge(0);
        if (child.is_leaf()) {
            const Leaf moved = NodeMove<Shape>::move_leaf(Leaf::at(child), root.page);
            root_pointer = moved.pointer();
            release_page(PageKind::leaf, child.page());
        } else {
            const Internal moved = NodeMove<Shape>::move_internal(Internal::at(child), root.page);
            root_pointer = moved.pointer();
            release_page(PageKind::internal, child.page());
        }
    }

    // --- Lookups -----------------------------------------------------------

    // Whether a query lacks what every key shares: it then sorts before or
    // after every key, and no node is read (D50).
    [[nodiscard]] bool outside_shared_prefix(const Query& query) const noexcept {
        const Root root = root_page();
        const std::size_t length = root.shared_length();
        if (length == 0) {
            return false;
        }
        const int order = Traits::compare_shared(query, root.shared_bytes(), length);
        return order != 0;
    }

    [[nodiscard]] Found find(const Query& query) const noexcept {
        if (root_pointer.is_null() || outside_shared_prefix(query)) {
            return Found{};
        }
        NodePointer node = root_pointer;
        while (!node.is_leaf()) {
            const Internal internal = Internal::at(node);
            const typename Internal::SearchResult result = internal.search(query);
            if (result.found) {
                const Leaf head_leaf = internal.head_leaf(result.index);
                return Found{.key = &head_leaf.head_key(), .value = &head_leaf.head_value()};
            }
            node = internal.edge(result.index);
        }
        const Leaf leaf = Leaf::at(node);
        const std::size_t index = leaf.find(query);
        if (index == Leaf::absent) {
            return Found{};
        }
        return Found{.key = &leaf.key(index), .value = &leaf.value(index)};
    }

    // --- Insertion ---------------------------------------------------------

    struct InsertOutcome final {
        bool inserted = false;
        // The entry's value: the new one, or the one already there.
        Value* value = nullptr;
    };

    // Inserts key and value unless the key is present; then they are left
    // as they were and the present value is returned. Throws std::bad_alloc
    // before changing anything if a page is needed and none is given.
    InsertOutcome insert(Key&& key, Value&& value) {
        const Query query = Traits::query_of(key);
        if (!rightmost_leaf.is_null()) {
            const Leaf leaf = Leaf::at(rightmost_leaf);
            rightmost_leaf = NodePointer{};
            if (appends_to(leaf, query)) {
                const std::size_t index = leaf.length();
                const InlineWord word = Shape::window_word(query, leaf.prefix_length());
                leaf.insert_fit(index, std::move(key), std::move(value), word);
                rightmost_leaf = leaf.pointer();
                entry_count += 1;
                return InsertOutcome{.inserted = true, .value = &leaf.value(index)};
            }
        }
        if (root_pointer.is_null()) {
            std::byte* page = allocate_page(PageKind::root);
            root_pointer = Root::initialize_empty(page);
        }
        Algorithms::shrink_shared_prefix_for(root_pointer, query);
        NodePointer node = root_pointer;
        while (!node.is_leaf()) {
            const Internal internal = Internal::at(node);
            const typename Internal::SearchResult result = internal.search(query);
            if (result.found) {
                const Leaf head_leaf = internal.head_leaf(result.index);
                return InsertOutcome{.inserted = false, .value = &head_leaf.head_value()};
            }
            node = internal.edge(result.index);
        }
        const Leaf leaf = Leaf::at(node);
        const typename Leaf::SearchResult result = leaf.search(query);
        if (result.found) {
            return InsertOutcome{.inserted = false, .value = &leaf.value(result.index)};
        }
        if (leaf.length() < Leaf::capacity) {
            const InlineWord word = Shape::window_word(query, leaf.prefix_length());
            leaf.insert_fit(result.index, std::move(key), std::move(value), word);
            entry_count += 1;
            return InsertOutcome{.inserted = true, .value = &leaf.value(result.index)};
        }
        return insert_with_stack(std::move(key), std::move(value));
    }

    // A full leaf: search again recording the stack, reserve the split's
    // pages, then split (D37).
    InsertOutcome insert_with_stack(Key&& key, Value&& value) {
        const Query query = Traits::query_of(key);
        Stack stack;
        stack.start_at(root_pointer, true);
        const bool found = stack.search(query);
        if (found) {
            const EntryPointers<Key, Value> entry = entry_at(stack);
            return InsertOutcome{.inserted = false, .value = entry.value};
        }
        const SplitPages pages = Algorithms::pages_for_split(stack);
        PageReserve reserve;
        reserve_pages(reserve, pages);
        Algorithms::insert_splitting(*this, stack, std::move(key), std::move(value), reserve);
        entry_count += 1;
        remember_if_last(stack);
        const Leaf leaf = stack.leaf();
        return InsertOutcome{.inserted = true, .value = &leaf.value(stack.index())};
    }

    // Inserts, or replaces an equal key's value and returns the old one.
    std::optional<Value> insert_replacing(Key&& key, Value&& value) {
        Value replacement = std::move(value);
        InsertOutcome outcome = insert(std::move(key), std::move(replacement));
        if (outcome.inserted) {
            return std::nullopt;
        }
        std::optional<Value> old(std::move(*outcome.value));
        *outcome.value = std::move(replacement);
        return old;
    }

    // Inserts `Count` entries (anything with .key and .value), replacing
    // equal keys' values, with the same result, node for node, as inserting
    // them in order (D53). Each key's lane finds its destination read-only
    // first; then entries apply in input order: a destination whose leaf an
    // earlier entry changed is searched again, a split drops that leaf's
    // destinations and every separator's, and an insertion from the root
    // drops them all.
    template <typename EntryType, std::size_t Count>
    void insert_many(EntryType* entries, std::optional<Value>* results) {
        using Batch = UnsyncBatch<Shape>;
        std::size_t index = 0;
        if (Count < 2 || root_pointer.is_null() || root_pointer.is_leaf() || !BatchCutoff<Shape>::batches(entry_count)) {
            for (index = 0; index < Count; index += 1) {
                results[index] = insert_replacing(std::move(entries[index].key), std::move(entries[index].value));
            }
            return;
        }
        if (!rightmost_leaf.is_null()) {
            const Leaf leaf = Leaf::at(rightmost_leaf);
            const Query first_query = Traits::query_of(entries[0].key);
            if (appends_to(leaf, first_query)) {
                // Ascending keys mostly append: the rest go one by one.
                for (index = 0; index < Count; index += 1) {
                    results[index] = insert_replacing(std::move(entries[index].key), std::move(entries[index].value));
                }
                return;
            }
        }
        rightmost_leaf = NodePointer{};
        Query queries[Count];
        typename Batch::Destination destinations[Count];
        for (index = 0; index < Count; index += 1) {
            queries[index] = Traits::query_of(entries[index].key);
        }
        Batch::find_destinations(root_pointer, queries, destinations, Count);
        bool descents_valid = true;
        for (index = 0; index < Count; index += 1) {
            const typename Batch::Destination destination = destinations[index];
            if (!descents_valid || destination.kind == Batch::DestinationKind::from_root) {
                descents_valid = false;
                results[index] = insert_replacing(std::move(entries[index].key), std::move(entries[index].value));
                continue;
            }
            const Query query = Traits::query_of(entries[index].key);
            if (destination.kind == Batch::DestinationKind::separator) {
                const Leaf head_leaf = Internal{destination.node}.head_leaf(destination.index);
                results[index].emplace(std::move(head_leaf.head_value()));
                head_leaf.head_value() = std::move(entries[index].value);
                continue;
            }
            const Leaf leaf = Leaf{destination.node};
            typename Leaf::SearchResult found{.found = destination.found, .index = destination.index};
            if (leaf.length() != destination.searched_length) {
                found = leaf.search(query);
            }
            if (found.found) {
                results[index].emplace(std::move(leaf.value(found.index)));
                leaf.value(found.index) = std::move(entries[index].value);
                continue;
            }
            if (leaf.length() < Leaf::capacity) {
                const InlineWord word = Shape::window_word(query, leaf.prefix_length());
                leaf.insert_fit(found.index, std::move(entries[index].key), std::move(entries[index].value), word);
                entry_count += 1;
                results[index] = std::nullopt;
                continue;
            }
            results[index] = insert_replacing(std::move(entries[index].key), std::move(entries[index].value));
            std::size_t later = 0;
            for (later = index + 1; later < Count; later += 1) {
                const bool same_leaf = destinations[later].kind == Batch::DestinationKind::leaf && destinations[later].node == leaf.page;
                if (same_leaf || destinations[later].kind == Batch::DestinationKind::separator) {
                    destinations[later].kind = Batch::DestinationKind::from_root;
                }
            }
        }
    }

    // --- Removal -----------------------------------------------------------

    // Removes the query's entry into `removed`; false if absent.
    bool erase(const Query& query, EntryBuffer<Key, Value>& removed) noexcept {
        if (root_pointer.is_null() || outside_shared_prefix(query)) {
            return false;
        }
        rightmost_leaf = NodePointer{};
        Stack stack;
        stack.start_at(root_pointer, true);
        if (!stack.search(query)) {
            return false;
        }
        Algorithms::remove(*this, stack, removed);
        entry_count -= 1;
        return true;
    }

    bool pop_first(EntryBuffer<Key, Value>& removed) noexcept {
        if (entry_count == 0) {
            return false;
        }
        rightmost_leaf = NodePointer{};
        Stack stack;
        stack.start_at(root_pointer, true);
        stack.descend_to_first_leaf();
        Algorithms::remove(*this, stack, removed);
        entry_count -= 1;
        return true;
    }

    bool pop_last(EntryBuffer<Key, Value>& removed) noexcept {
        if (entry_count == 0) {
            return false;
        }
        rightmost_leaf = NodePointer{};
        Stack stack;
        stack.start_at(root_pointer, true);
        stack.descend_to_last_leaf();
        stack.set_index(stack.leaf().length() - 1);
        Algorithms::remove(*this, stack, removed);
        entry_count -= 1;
        return true;
    }

    void clear() noexcept {
        release_all();
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return entry_count;
    }

    // --- Cursors (tree_walk.hpp) --------------------------------------------

    using Cursor = typename TreeWalk<Shape>::Cursor;

    [[nodiscard]] EntryPointers<Key, Value> entry_at_cursor(const Cursor& cursor) const noexcept {
        return TreeWalk<Shape>::entry_at(cursor);
    }
    void first(Cursor& cursor) const noexcept {
        TreeWalk<Shape>::first(root_pointer, cursor);
    }
    void last(Cursor& cursor) const noexcept {
        TreeWalk<Shape>::last(root_pointer, cursor);
    }
    void advance(Cursor& cursor) const noexcept {
        TreeWalk<Shape>::advance(cursor);
    }
    void retreat(Cursor& cursor) const noexcept {
        TreeWalk<Shape>::retreat(cursor);
    }
    void lower_bound(Cursor& cursor, const Query& query) const noexcept {
        TreeWalk<Shape>::lower_bound(root_pointer, cursor, query);
    }

private:
    [[nodiscard]] bool appends_to(Leaf leaf, const Query& query) const noexcept {
        const std::size_t length = leaf.length();
        if (length == 0 || length >= Leaf::capacity) {
            return false;
        }
        const int order = Traits::compare_from(query, leaf.key(length - 1), 0);
        if (order <= 0) {
            return false;
        }
        return Algorithms::has_shared_prefix(root_pointer, query);
    }

    void reserve_pages(PageReserve& reserve, SplitPages pages) {
        try {
            while (reserve.leaf_count < pages.leaves) {
                reserve.leaves[reserve.leaf_count] = allocate_page(PageKind::leaf);
                reserve.leaf_count += 1;
            }
            while (reserve.internal_count < pages.internals) {
                reserve.internals[reserve.internal_count] = allocate_page(PageKind::internal);
                reserve.internal_count += 1;
            }
        } catch (...) {
            while (reserve.leaf_count > 0) {
                release_page(PageKind::leaf, reserve.take(PageKind::leaf));
            }
            while (reserve.internal_count > 0) {
                release_page(PageKind::internal, reserve.take(PageKind::internal));
            }
            throw;
        }
    }

    [[nodiscard]] EntryPointers<Key, Value> entry_at(const Stack& stack) const noexcept {
        if (stack.ends_at_leaf()) {
            const Leaf leaf = stack.leaf();
            return EntryPointers<Key, Value>{.key = &leaf.key(stack.index()), .value = &leaf.value(stack.index())};
        }
        const Leaf head_leaf = stack.internal().head_leaf(stack.index());
        return EntryPointers<Key, Value>{.key = &head_leaf.head_key(), .value = &head_leaf.head_value()};
    }

    // After an insert that split, remembers the leaf when the new key went
    // into the last slot at every level (D45).
    void remember_if_last(const Stack& stack) noexcept {
        const Leaf leaf = stack.leaf();
        if (stack.index() + 1 != leaf.length()) {
            return;
        }
        std::size_t level = 0;
        for (level = 0; level < stack.depth(); level += 1) {
            if (stack.index_at(level) != stack.internal_at(level).length()) {
                return;
            }
        }
        rightmost_leaf = leaf.pointer();
    }

    // Destroys every entry and frees every page.
    void release_all() noexcept {
        if (root_pointer.is_null()) {
            return;
        }
        release_subtree(root_pointer, true);
        root_pointer = NodePointer{};
        rightmost_leaf = NodePointer{};
        entry_count = 0;
    }

    void release_subtree(NodePointer top, bool is_root_page) noexcept {
        Stack stack;
        stack.start_at(top, is_root_page);
        while (true) {
            if (stack.ends_at_leaf()) {
                const Leaf leaf = stack.leaf();
                leaf.destroy_entries();
                if (stack.is_at_top()) {
                    release_page(PageKind::root, leaf.page);
                    return;
                }
                release_page(PageKind::leaf, leaf.page);
                stack.ascend();
                stack.set_index(stack.index() + 1);
                continue;
            }
            const Internal node = stack.internal();
            if (stack.index() <= node.length()) {
                stack.descend();
                continue;
            }
            if (stack.is_at_top()) {
                release_page(PageKind::root, node.page);
                return;
            }
            release_page(PageKind::internal, node.page);
            stack.ascend();
            stack.set_index(stack.index() + 1);
        }
    }

    NodePointer root_pointer;
    NodePointer rightmost_leaf;
    std::size_t entry_count = 0;
    [[no_unique_address]] Allocator page_allocator;
};

} // namespace stx_btree::detail
