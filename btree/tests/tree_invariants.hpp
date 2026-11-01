#pragma once

// A test-only checker of every structural invariant (bbtree
// tree/invariants.rs): depths, fills, heads, sorted keys, windows at exact
// prefixes, padding past each length, hash tags and the shared prefix.

#include "stx_btree/detail/node.hpp"
#include "stx_btree/detail/root_page.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace stx_btree_tests {

template <typename Shape>
class TreeInvariants final {
public:
    using Key = typename Shape::KeyType;
    using Traits = typename Shape::Traits;
    using Leaf = stx_btree::detail::LeafNode<Shape>;
    using Internal = stx_btree::detail::InternalNode<Shape>;
    using NodePointer = stx_btree::detail::NodePointer;
    using Root = stx_btree::detail::RootPage<Shape>;

    struct Report final {
        std::size_t entries = 0;
        std::size_t leaves = 0;
        std::size_t internals = 0;
        std::size_t inexact_prefixes = 0;
    };

    static void fail(const char* what) {
        std::fprintf(stderr, "invariant failed: %s\n", what);
        std::abort();
    }

    static void expect(bool condition, const char* what) {
        if (!condition) {
            fail(what);
        }
    }

    // Checks the tree under `root` (null for an empty tree).
    Report check(NodePointer root, std::size_t expected_entries) {
        Report report;
        if (root.is_null()) {
            expect(expected_entries == 0, "an empty tree has no entries");
            return report;
        }
        const Root root_page = Root{root.page()};
        shared_length = root_page.shared_length();
        shared_bytes = root_page.shared_bytes();
        leaf_depth = static_cast<std::size_t>(-1);
        keys_in_order.clear();
        check_node(root, nullptr, nullptr, 0, true, 0, report);
        expect(report.entries == expected_entries, "the entry count matches the tree");
        std::size_t index = 0;
        for (index = 1; index < keys_in_order.size(); index += 1) {
            const int order = Traits::compare_from(Traits::query_of(*keys_in_order[index - 1]), *keys_in_order[index], 0);
            expect(order < 0, "keys ascend strictly");
        }
        for (index = 0; index < keys_in_order.size(); index += 1) {
            const std::size_t shared = Traits::shared_length(Traits::query_of(*keys_in_order[index]), shared_bytes, shared_length);
            expect(shared == shared_length, "every key has the shared prefix");
        }
        return report;
    }

private:
    std::size_t expected_prefix(const Key* low, const Key* high, std::size_t parent_prefix) {
        std::size_t fitted = shared_length;
        if (low != nullptr && high != nullptr) {
            fitted = Traits::common_prefix_length(*low, *high);
        }
        return fitted > parent_prefix ? fitted : parent_prefix;
    }

    void check_node(NodePointer node, const Key* low, const Key* high, std::size_t depth, bool is_root,
                           std::size_t parent_prefix, Report& report) {
        if (node.is_leaf()) {
            check_leaf(Leaf::at(node), low, high, depth, is_root, parent_prefix, report);
            return;
        }
        const Internal internal = Internal::at(node);
        report.internals += 1;
        const std::size_t length = internal.length();
        expect(length <= Internal::capacity, "an internal node within capacity");
        if (!is_root) {
            expect(length >= Internal::minimum, "a non-root internal node keeps its minimum");
        } else {
            expect(length >= 1, "an internal root has a separator");
        }
        const std::size_t prefix = internal.prefix_length();
        expect(prefix >= parent_prefix || is_root, "a child's prefix is at least its parent's");
        const std::size_t wanted = expected_prefix(low, high, parent_prefix);
        expect(prefix <= wanted, "a prefix is within what the fences share");
        bool children_allow = true;
        const bool leaf_children = internal.has_leaf_children();
        std::size_t index = 0;
        for (index = 0; index <= length; index += 1) {
            const NodePointer child = internal.edge(index);
            expect(child.is_leaf() == leaf_children, "siblings are of one kind");
            const std::size_t child_prefix = stx_btree::detail::HeaderAccess<Shape::latched>::prefix_length(child.page());
            if (child_prefix < wanted) {
                children_allow = false;
            }
        }
        if (prefix != wanted && children_allow) {
            report.inexact_prefixes += 1;
        }
        for (index = 0; index < length; index += 1) {
            const Key& separator = internal.separator_key(index);
            const InlineWordCheck window = InlineWordCheck::of(Traits::window(Traits::query_of(separator), prefix), Internal::slot_shift);
            expect(internal.inline_keys()[index] == static_cast<typename Internal::Slot>(window.slot), "a separator's window");
            const std::size_t shared = Traits::shared_length(Traits::query_of(separator), shared_bytes, shared_length);
            expect(shared == shared_length, "a separator has the shared prefix");
        }
        for (index = length; index < Internal::capacity; index += 1) {
            expect(internal.inline_keys()[index] == Internal::vacant_slot, "an internal node pads with the greatest key");
        }
        for (index = 0; index <= length; index += 1) {
            const Key* child_low = index > 0 ? &internal.separator_key(index - 1) : low;
            const Key* child_high = index < length ? &internal.separator_key(index) : high;
            check_node(internal.edge(index), child_low, child_high, depth + 1, false, prefix, report);
        }
    }

    struct InlineWordCheck final {
        std::uint64_t slot = 0;
        static InlineWordCheck of(std::uint64_t word, unsigned shift) {
            return InlineWordCheck{word >> shift};
        }
    };

    void check_leaf(Leaf leaf, const Key* low, const Key* high, std::size_t depth, bool is_root,
                           std::size_t parent_prefix, Report& report) {
        report.leaves += 1;
        if (leaf_depth == static_cast<std::size_t>(-1)) {
            leaf_depth = depth;
        }
        expect(depth == leaf_depth, "every leaf at one depth");
        const std::size_t length = leaf.length();
        expect(length <= Leaf::capacity, "a leaf within capacity");
        if (!is_root) {
            expect(length >= Leaf::minimum, "a non-root leaf keeps its minimum");
        }
        expect(leaf.has_head() == (low != nullptr), "a leaf has a head exactly when a separator is left of it");
        const std::size_t prefix = leaf.prefix_length();
        expect(prefix >= parent_prefix || is_root, "a leaf's prefix is at least its parent's");
        const std::size_t wanted = expected_prefix(low, high, parent_prefix);
        expect(prefix <= wanted, "a leaf's prefix is within what its fences share");
        if (prefix != wanted && !leaf.keys_differ_past_prefix()) {
            report.inexact_prefixes += 1;
        }
        if (leaf.has_head()) {
            expect(&leaf.head_key() == low, "a separator's key is its head leaf's head");
            keys_in_order.push_back(&leaf.head_key());
            report.entries += 1;
        }
        std::size_t index = 0;
        for (index = 0; index < length; index += 1) {
            const Key& key = leaf.key(index);
            const std::uint64_t word = Traits::window(Traits::query_of(key), prefix);
            expect(leaf.inline_keys()[index] == Leaf::slot_of(word), "a key's window at its leaf's prefix");
            if constexpr (Shape::leaf.has_hash_tags) {
                expect(leaf.hash_tags()[index] == Traits::hash_tag(Traits::query_of(key)), "a key's hash tag");
            }
            if (low != nullptr) {
                expect(Traits::compare_from(Traits::query_of(*low), key, 0) < 0, "keys lie above the low fence");
            }
            if (high != nullptr) {
                expect(Traits::compare_from(Traits::query_of(key), *high, 0) < 0, "keys lie below the high fence");
            }
            keys_in_order.push_back(&key);
            report.entries += 1;
        }
        for (index = length; index < Leaf::capacity; index += 1) {
            expect(leaf.inline_keys()[index] == Leaf::vacant_slot, "a leaf pads with the greatest key");
        }
    }

    std::size_t shared_length = 0;
    const std::uint8_t* shared_bytes = nullptr;
    std::size_t leaf_depth = 0;
    std::vector<const Key*> keys_in_order;
};

} // namespace stx_btree_tests
