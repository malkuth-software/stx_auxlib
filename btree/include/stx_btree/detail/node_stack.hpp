#pragma once

// Node stacks (bbtree node_stack.rs, D24): the nodes from a top node down to
// a position in one of them, and the index taken at each. No node points to
// its parent (D11); going up, splitting, rebalancing and fitting prefixes
// all read the stack. The top is the root, or in a latched tree the topmost
// node an operation holds.
//
// A stack is built in place and never copied by value in the hot paths: a
// copy reads it with wide loads right after the narrow stores that wrote it
// (D23, D24).

#include "stx_btree/detail/node.hpp"

#include <cstddef>
#include <cstdint>

namespace stx_btree::detail {

template <typename Shape>
class NodeStack final {
public:
    using Leaf = LeafNode<Shape>;
    using Internal = InternalNode<Shape>;
    using Key = typename Shape::KeyType;
    using Query = typename Shape::Query;

    // Levels [0, levels) are live; slots past them are left unwritten until
    // used, so making a stack writes 16 bytes rather than its 240 (D24).
    std::uintptr_t nodes[node_stack_levels];
    std::uint16_t indexes[node_stack_levels];
    std::size_t levels = 0;
    // Whether nodes[0] is the tree's root, which may raise and lower.
    bool top_is_root = true;

    void start_at(NodePointer top, bool is_root) noexcept {
        nodes[0] = top.tagged;
        indexes[0] = 0;
        levels = 1;
        top_is_root = is_root;
    }
    void detach() noexcept {
        levels = 0;
    }
    // Appends a level below the current bottom, for a stack built along a
    // path a descent kept (bbtree NodeStackMut::along).
    void push(NodePointer node, std::size_t index) noexcept {
        nodes[levels] = node.tagged;
        indexes[levels] = static_cast<std::uint16_t>(index);
        levels += 1;
    }
    [[nodiscard]] bool is_attached() const noexcept {
        return levels != 0;
    }
    [[nodiscard]] std::size_t depth() const noexcept {
        return levels - 1;
    }
    [[nodiscard]] bool is_at_top() const noexcept {
        return levels == 1;
    }
    [[nodiscard]] NodePointer node() const noexcept {
        return NodePointer{nodes[levels - 1]};
    }
    [[nodiscard]] NodePointer node_at(std::size_t level) const noexcept {
        return NodePointer{nodes[level]};
    }
    [[nodiscard]] bool ends_at_leaf() const noexcept {
        return node().is_leaf();
    }
    [[nodiscard]] Leaf leaf() const noexcept {
        return Leaf::at(node());
    }
    [[nodiscard]] Internal internal() const noexcept {
        return Internal::at(node());
    }
    [[nodiscard]] Internal internal_at(std::size_t level) const noexcept {
        return Internal::at(NodePointer{nodes[level]});
    }
    [[nodiscard]] std::size_t index() const noexcept {
        return indexes[levels - 1];
    }
    [[nodiscard]] std::size_t index_at(std::size_t level) const noexcept {
        return indexes[level];
    }
    void set_index(std::size_t index) noexcept {
        indexes[levels - 1] = static_cast<std::uint16_t>(index);
    }
    void set_index_at(std::size_t level, std::size_t index) noexcept {
        indexes[level] = static_cast<std::uint16_t>(index);
    }
    void replace_node(NodePointer pointer) noexcept {
        nodes[levels - 1] = pointer.tagged;
    }

    // Goes up to the parent; returns it.
    Internal ascend() noexcept {
        levels -= 1;
        return internal();
    }

    // Goes down the current node's edge at the current index.
    void descend() noexcept {
        const NodePointer child = internal().edge(index());
        nodes[levels] = child.tagged;
        indexes[levels] = 0;
        levels += 1;
    }

    // Down first edges (or last edges) to a leaf, ending at its first edge
    // (or past its last key).
    void descend_to_first_leaf() noexcept {
        while (!ends_at_leaf()) {
            set_index(0);
            descend();
        }
        set_index(0);
    }
    void descend_to_last_leaf() noexcept {
        while (!ends_at_leaf()) {
            set_index(internal().length());
            descend();
        }
        set_index(leaf().length());
    }

    // The fences of the current node: the nearest separators left and right
    // of the path, from the ancestors on the stack. A missing one is the
    // tree's border, whose fence is what every key shares.
    struct Fences final {
        const Key* low = nullptr;
        const Key* high = nullptr;
    };

    [[nodiscard]] Fences fences() const noexcept {
        Fences result;
        std::size_t level = depth();
        while (level > 0) {
            level -= 1;
            const Internal node = internal_at(level);
            const std::size_t index = index_at(level);
            if (result.low == nullptr && index > 0) {
                result.low = &node.separator_key(index - 1);
            }
            if (result.high == nullptr && index < node.length()) {
                result.high = &node.separator_key(index);
            }
            if (result.low != nullptr && result.high != nullptr) {
                break;
            }
        }
        return result;
    }

    // Searches down from the current node for the query, which has the
    // tree's shared prefix. Ends at the separator equal to it (found, at an
    // internal node), or at the leaf position of an equal key (found) or of
    // its insertion (not found).
    [[nodiscard]] bool search(const Query& query) noexcept {
        while (!ends_at_leaf()) {
            const Internal node = internal();
            const typename Internal::SearchResult result = node.search(query);
            set_index(result.index);
            if (result.found) {
                return true;
            }
            descend();
        }
        const typename Leaf::SearchResult result = leaf().search(query);
        set_index(result.index);
        return result.found;
    }
};

} // namespace stx_btree::detail
