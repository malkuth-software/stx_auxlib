#pragma once

// Ordered walks over a tree that nothing else changes meanwhile: the unsync
// map's iterators and a shared map's owner (bbtree walk.rs, D23). A
// position is a leaf on a node stack and an entry slot in it: 0 the head's,
// i + 1 key i's. Crossing to the next leaf visits ancestors only; no key is
// compared.

#include "stx_btree/detail/node.hpp"
#include "stx_btree/detail/node_stack.hpp"
#include "stx_btree/detail/root_page.hpp"

#include <cstddef>

namespace stx_btree::detail {

// An entry found in place: its key and value, valid until the tree changes.
template <typename Key, typename Value>
struct EntryPointers final {
    Key* key = nullptr;
    Value* value = nullptr;
};

template <typename Shape>
class TreeWalk final {
public:
    using Key = typename Shape::KeyType;
    using Value = typename Shape::ValueType;
    using Query = typename Shape::Query;
    using Traits = typename Shape::Traits;
    using Leaf = LeafNode<Shape>;
    using Internal = InternalNode<Shape>;
    using Stack = NodeStack<Shape>;

    struct Cursor final {
        Stack stack;
        std::size_t slot = 0;
    };

    // Whether the tree under `root` (null for none) holds no entry.
    [[nodiscard]] static bool is_empty(NodePointer root) noexcept {
        if (root.is_null()) {
            return true;
        }
        return root.is_leaf() && Leaf::at(root).length() == 0;
    }

    [[nodiscard]] static EntryPointers<Key, Value> entry_at(const Cursor& cursor) noexcept {
        const Leaf leaf = cursor.stack.leaf();
        if (cursor.slot == 0) {
            return EntryPointers<Key, Value>{.key = &leaf.head_key(), .value = &leaf.head_value()};
        }
        return EntryPointers<Key, Value>{.key = &leaf.key(cursor.slot - 1), .value = &leaf.value(cursor.slot - 1)};
    }

    static void first(NodePointer root, Cursor& cursor) noexcept {
        if (is_empty(root)) {
            cursor.stack.detach();
            return;
        }
        cursor.stack.start_at(root, true);
        cursor.stack.descend_to_first_leaf();
        cursor.slot = cursor.stack.leaf().has_head() ? 0 : 1;
    }

    static void last(NodePointer root, Cursor& cursor) noexcept {
        if (is_empty(root)) {
            cursor.stack.detach();
            return;
        }
        cursor.stack.start_at(root, true);
        cursor.stack.descend_to_last_leaf();
        cursor.slot = cursor.stack.leaf().length();
    }

    // The next entry: within the leaf, or the next leaf's head.
    static void advance(Cursor& cursor) noexcept {
        const Leaf leaf = cursor.stack.leaf();
        if (cursor.slot < leaf.length()) {
            cursor.slot += 1;
            return;
        }
        Stack& stack = cursor.stack;
        while (!stack.is_at_top()) {
            const Internal parent = stack.ascend();
            const std::size_t edge = stack.index();
            if (edge < parent.length()) {
                stack.set_index(edge + 1);
                stack.descend();
                stack.descend_to_first_leaf();
                cursor.slot = 0;
                return;
            }
        }
        stack.detach();
    }

    static void retreat(Cursor& cursor) noexcept {
        const Leaf leaf = cursor.stack.leaf();
        const std::size_t first_slot = leaf.has_head() ? 0 : 1;
        if (cursor.slot > first_slot) {
            cursor.slot -= 1;
            return;
        }
        Stack& stack = cursor.stack;
        while (!stack.is_at_top()) {
            stack.ascend();
            const std::size_t edge = stack.index();
            if (edge > 0) {
                stack.set_index(edge - 1);
                stack.descend();
                stack.descend_to_last_leaf();
                cursor.slot = stack.leaf().length();
                return;
            }
        }
        stack.detach();
    }

    // The first entry not below the query.
    static void lower_bound(NodePointer root, Cursor& cursor, const Query& query) noexcept {
        if (is_empty(root)) {
            cursor.stack.detach();
            return;
        }
        const RootPage<Shape> root_page = RootPage<Shape>{root.page()};
        const std::size_t length = root_page.shared_length();
        if (length != 0) {
            const int order = Traits::compare_shared(query, root_page.shared_bytes(), length);
            if (order < 0) {
                first(root, cursor);
                return;
            }
            if (order > 0) {
                cursor.stack.detach();
                return;
            }
        }
        cursor.stack.start_at(root, true);
        const bool found = cursor.stack.search(query);
        if (found && !cursor.stack.ends_at_leaf()) {
            cursor.stack.set_index(cursor.stack.index() + 1);
            cursor.stack.descend();
            cursor.stack.descend_to_first_leaf();
            cursor.slot = 0;
            return;
        }
        const std::size_t index = cursor.stack.index();
        const Leaf leaf = cursor.stack.leaf();
        if (index < leaf.length()) {
            cursor.slot = index + 1;
            return;
        }
        cursor.slot = leaf.length();
        if (cursor.slot == 0 && !leaf.has_head()) {
            cursor.stack.detach();
            return;
        }
        advance(cursor);
    }
};

} // namespace stx_btree::detail
