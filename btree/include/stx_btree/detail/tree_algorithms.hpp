#pragma once

// The tree's structural operations along a node stack (bbtree insert.rs,
// remove.rs, prefix.rs, siblings.rs): splits carried up, prefixes fitted to
// changed fences, removals rebalanced bottom-up. Both maps run these: the
// unsync tree from its root, the latched tree from the topmost node it
// holds (D54). A Host supplies pages, the shared prefix and root changes:
//
//   std::byte* take_page(PageReserve&, PageKind)  a page reserved beforehand
//   void release_page(PageKind, std::byte*) a page no node uses any more
//   std::size_t shared_length() const       what every key shares
//   NodePointer raise_root(PageReserve&)    root node out, internal root in
//   void lower_root()                       an emptied internal root's child in
//   NodePointer root() const

#include "stx_btree/detail/node.hpp"
#include "stx_btree/detail/node_stack.hpp"
#include "stx_btree/detail/root_page.hpp"
#include "stx_btree/page_allocator.hpp"

#include <cstddef>
#include <cstdint>
#include <new>
#include <utility>

namespace stx_btree::detail {

// Owned storage for an entry moved out of a node, destroyed with the buffer
// unless taken.
template <typename Key, typename Value>
class EntryBuffer final {
public:
    EntryBuffer() noexcept = default;
    EntryBuffer(const EntryBuffer&) = delete;
    EntryBuffer& operator=(const EntryBuffer&) = delete;
    ~EntryBuffer() {
        if (filled) {
            key().~Key();
            value().~Value();
        }
    }

    [[nodiscard]] Key* key_storage() noexcept {
        return reinterpret_cast<Key*>(key_bytes);
    }
    [[nodiscard]] Value* value_storage() noexcept {
        return reinterpret_cast<Value*>(value_bytes);
    }
    // Called once the node has constructed both.
    void mark_filled() noexcept {
        filled = true;
    }
    [[nodiscard]] bool is_filled() const noexcept {
        return filled;
    }
    [[nodiscard]] Key& key() noexcept {
        return *std::launder(reinterpret_cast<Key*>(key_bytes));
    }
    [[nodiscard]] Value& value() noexcept {
        return *std::launder(reinterpret_cast<Value*>(value_bytes));
    }

private:
    alignas(Key) std::byte key_bytes[sizeof(Key)];
    alignas(Value) std::byte value_bytes[sizeof(Value)];
    bool filled = false;
};

// Pages taken before a change starts, so that a change never fails midway
// (house style 9; bbtree takes a latched split's pages first, D54).
class PageReserve final {
public:
    static constexpr std::size_t capacity = node_stack_levels + 2;

    std::byte* leaves[2] = {nullptr, nullptr};
    std::size_t leaf_count = 0;
    std::byte* internals[capacity] = {};
    std::size_t internal_count = 0;

    [[nodiscard]] std::byte* take(PageKind kind) noexcept {
        if (kind == PageKind::leaf) {
            leaf_count -= 1;
            std::byte* page = leaves[leaf_count];
            leaves[leaf_count] = nullptr;
            return page;
        }
        internal_count -= 1;
        std::byte* page = internals[internal_count];
        internals[internal_count] = nullptr;
        return page;
    }
};

// How many pages a split from the stack's leaf takes: the leaf's, one per
// full internal node above it, and one for a root that moves out of the
// root page if every node up to the top is full.
struct SplitPages final {
    std::size_t leaves = 0;
    std::size_t internals = 0;
};

enum class ChildFix : std::uint8_t {
    nothing, merged_into_left, merged_right_in, took_from_left, took_from_right
};

template <typename Shape, typename Host>
class TreeAlgorithms final {
public:
    using Key = typename Shape::KeyType;
    using Value = typename Shape::ValueType;
    using Query = typename Shape::Query;
    using Traits = typename Shape::Traits;
    using Leaf = LeafNode<Shape>;
    using Internal = InternalNode<Shape>;
    using Stack = NodeStack<Shape>;
    using Fences = typename Stack::Fences;

    // --- Prefixes (05-fence-key.md) ----------------------------------------

    // What a node with these fences shares; a missing fence is the border's.
    [[nodiscard]] static std::size_t fence_prefix_length(const Key* low, const Key* high, std::size_t shared_length) noexcept {
        if (low == nullptr || high == nullptr) {
            return shared_length;
        }
        return Traits::common_prefix_length(*low, *high);
    }

    // Fits the prefixes of the stack node's children at `edges` to their
    // fences, reading keys only where what the fences share changed.
    static void refit_children_to_fences(Host& host, Stack& stack, const std::size_t* edges, std::size_t edge_count) noexcept {
        const Internal node = stack.internal();
        const Fences fences = stack.fences();
        const std::size_t shared_length = host.shared_length();
        const std::size_t node_prefix = node.prefix_length();
        const bool leaf_children = node.has_leaf_children();
        std::size_t position = 0;
        for (position = 0; position < edge_count; position += 1) {
            const std::size_t edge = edges[position];
            const NodePointer child = node.edge(edge);
            const Key* child_low = edge > 0 ? &node.separator_key(edge - 1) : fences.low;
            const Key* child_high = edge < node.length() ? &node.separator_key(edge) : fences.high;
            if (leaf_children) {
                const Leaf leaf = Leaf::at(child);
                if (leaf.keys_differ_past_prefix()) {
                    continue;
                }
                const std::size_t fitted = fence_prefix_length(child_low, child_high, shared_length);
                const std::size_t floored = fitted > node_prefix ? fitted : node_prefix;
                leaf.refit_prefix(floored);
            } else {
                const Internal internal = Internal::at(child);
                if (internal.keys_differ_past_prefix()) {
                    continue;
                }
                const std::size_t fitted = fence_prefix_length(child_low, child_high, shared_length);
                const std::size_t floored = fitted > node_prefix ? fitted : node_prefix;
                internal.refit_prefix(floored);
            }
        }
    }

    static void refit_two_children(Host& host, Stack& stack, std::size_t first, std::size_t second) noexcept {
        const std::size_t edges[2] = {first, second};
        refit_children_to_fences(host, stack, edges, 2);
    }

    static void refit_one_child(Host& host, Stack& stack, std::size_t edge) noexcept {
        const std::size_t edges[1] = {edge};
        refit_children_to_fences(host, stack, edges, 1);
    }

    // After a split carried up `levels_climbed` levels, fits both halves at
    // each level, from the leaf up; the stack ends where it started.
    static void refit_split_halves(Host& host, Stack& stack, const std::size_t* separator_indexes, std::size_t levels_climbed) noexcept {
        std::size_t indexes[node_stack_levels + 1];
        indexes[0] = stack.index();
        std::size_t climbed = 0;
        for (climbed = 1; climbed <= levels_climbed; climbed += 1) {
            stack.ascend();
            indexes[climbed] = stack.index();
            const std::size_t separator_index = separator_indexes[climbed];
            refit_two_children(host, stack, separator_index, separator_index + 1);
        }
        climbed = levels_climbed;
        while (climbed > 0) {
            climbed -= 1;
            stack.descend();
            stack.set_index(indexes[climbed]);
        }
    }

    // Before separator `stack.index()` of the stack node is replaced by its
    // successor, shrinks the right border of the subtree left of it, whose
    // upper fence widens (05-fence-key.md, "Upkeep").
    static void refit_left_of_separator(Stack& stack) noexcept {
        const std::size_t index = stack.index();
        const Internal node = stack.internal();
        const Leaf head_leaf = node.head_leaf(index);
        const Key& successor = head_leaf.key(0);
        const Key& separator = head_leaf.head_key();
        const std::size_t shared = Traits::common_prefix_length(separator, successor);
        const std::size_t node_prefix = node.prefix_length();
        const std::size_t target = shared > node_prefix ? shared : node_prefix;
        NodePointer walk = node.edge(index);
        while (!walk.is_leaf()) {
            const Internal internal = Internal::at(walk);
            if (internal.prefix_length() > target) {
                internal.refit_prefix(target);
            }
            walk = internal.edge(internal.length());
        }
        const Leaf leaf = Leaf::at(walk);
        if (leaf.prefix_length() > target) {
            leaf.refit_prefix(target);
        }
    }

    // --- The shared prefix (D50) --------------------------------------------

    // An empty tree starts its shared prefix from the key; a key that shares
    // less shrinks it, and every border node moves back with it, filled from
    // the shared bytes, reading no key. Needs the root and both borders.
    static void shrink_shared_prefix_for(NodePointer root, const Query& query) noexcept {
        const RootPage<Shape> root_page = RootPage<Shape>{root.page()};
        if (root.is_leaf() && Leaf::at(root).length() == 0) {
            root_page.start_shared_prefix(query);
            HeaderAccess<Shape::latched>::set_prefix_length(root.page(), root_page.shared_length());
            return;
        }
        const std::size_t length = root_page.shared_length();
        if (length == 0) {
            return;
        }
        const std::size_t shared = Traits::shared_length(query, root_page.shared_bytes(), length);
        if (shared < length) {
            const Window fill = root_page.shared_window_at(shared);
            root_page.set_shared_length(shared);
            move_border_back(root, fill, true);
            move_border_back(root, fill, false);
        }
    }

    static void move_border_back(NodePointer root, Window fill, bool first_edges) noexcept {
        NodePointer node = root;
        while (!node.is_leaf()) {
            const Internal internal = Internal::at(node);
            internal.move_prefix_back_to(fill);
            node = internal.edge(first_edges ? 0 : internal.length());
        }
        Leaf::at(node).move_prefix_back_to(fill);
    }

    // Whether the query has every byte of the shared prefix.
    [[nodiscard]] static bool has_shared_prefix(NodePointer root, const Query& query) noexcept {
        const RootPage<Shape> root_page = RootPage<Shape>{root.page()};
        const std::size_t length = root_page.shared_length();
        return Traits::shared_length(query, root_page.shared_bytes(), length) == length;
    }

    // --- Insertion ----------------------------------------------------------

    [[nodiscard]] static SplitPages pages_for_split(const Stack& stack) noexcept {
        SplitPages pages{.leaves = 1, .internals = 0};
        std::size_t level = stack.depth();
        bool every_level_full = true;
        while (level > 0) {
            level -= 1;
            const Internal node = stack.internal_at(level);
            if (node.length() < Internal::capacity) {
                every_level_full = false;
                break;
            }
            pages.internals += 1;
        }
        if (every_level_full && stack.top_is_root) {
            if (stack.node_at(0).is_leaf()) {
                pages.leaves += 1;
            } else {
                pages.internals += 1;
            }
        }
        return pages;
    }

    // Goes up one level, raising the tree first when the stack is at the
    // root (bbtree NodeStackMut::ascend_raising).
    static Internal ascend_raising(Host& host, Stack& stack, PageReserve& reserve) noexcept {
        if (!stack.is_at_top()) {
            return stack.ascend();
        }
        const NodePointer root = host.raise_root(reserve);
        stack.start_at(root, true);
        return stack.internal_at(0);
    }

    // Inserts into the stack's full leaf, splitting it and every full node
    // above it; ends with the stack at the new entry (bbtree
    // insert_splitting). The reserve holds pages_for_split's pages.
    static void insert_splitting(Host& host, Stack& stack, Key&& key, Value&& value, PageReserve& reserve) noexcept {
        const std::size_t insertion_index = stack.index();
        const Leaf leaf = stack.leaf();
        const std::size_t leaf_prefix = leaf.prefix_length();
        const InlineWord word = Shape::window_word(Traits::query_of(key), leaf_prefix);
        const SplitPlace leaf_place = SplitPlace::of(Leaf::capacity, insertion_index, Leaf::minimum);
        const Leaf right_leaf = Leaf::initialize(host.take_page(reserve, PageKind::leaf), leaf_prefix);
        Window carried_separator = leaf.split_around(leaf_place.separator_index, right_leaf);
        const Leaf target_leaf = leaf_place.right ? right_leaf : leaf;
        target_leaf.insert_fit(leaf_place.index, std::move(key), std::move(value), word);

        std::size_t indexes_in_halves[node_stack_levels + 1];
        std::size_t separator_indexes[node_stack_levels + 1];
        indexes_in_halves[0] = leaf_place.index;
        separator_indexes[0] = 0;
        NodePointer carried_right = right_leaf.pointer();
        bool entry_is_right = leaf_place.right;
        std::size_t climbed = 0;
        while (true) {
            const Internal node = ascend_raising(host, stack, reserve);
            climbed += 1;
            const std::size_t index = stack.index();
            const std::size_t beside = entry_is_right ? 1 : 0;
            if (node.length() < Internal::capacity) {
                node.insert_fit(index, carried_separator, carried_right);
                separator_indexes[climbed] = index;
                stack.set_index(index + beside);
                break;
            }
            const SplitPlace place = SplitPlace::of(Internal::capacity, index, Internal::minimum);
            const Internal right_node =
                Internal::initialize_empty(host.take_page(reserve, PageKind::internal), node.prefix_length());
            const Window middle = node.split_around(place.separator_index, right_node);
            const Internal target_node = place.right ? right_node : node;
            target_node.insert_fit(place.index, carried_separator, carried_right);
            separator_indexes[climbed] = place.index;
            indexes_in_halves[climbed] = place.index + beside;
            carried_separator = middle;
            carried_right = right_node.pointer();
            entry_is_right = place.right;
        }
        const std::size_t levels_climbed = climbed;
        while (climbed > 0) {
            climbed -= 1;
            stack.descend();
            stack.set_index(indexes_in_halves[climbed]);
        }
        refit_split_halves(host, stack, separator_indexes, levels_climbed);
    }

    // --- Removal ------------------------------------------------------------

    // Which children a removal changed, level by level, to fit afterward
    // (bbtree ChangedFences).
    struct ChangedFences final {
        std::size_t indexes[node_stack_levels + 1];
        std::size_t donors[node_stack_levels + 1];
        bool has_donor[node_stack_levels + 1];
        std::size_t leaf_depth = 0;
        bool has_top = false;
        std::size_t top_depth = 0;

        void start(std::size_t leaf_level, std::size_t next_slot, bool must_reach, std::size_t must_reach_depth) noexcept {
            std::size_t level = 0;
            for (level = 0; level <= node_stack_levels; level += 1) {
                indexes[level] = 0;
                donors[level] = 0;
                has_donor[level] = false;
            }
            indexes[leaf_level] = next_slot;
            leaf_depth = leaf_level;
            has_top = must_reach;
            top_depth = must_reach_depth;
        }
        [[nodiscard]] bool reached_top() const noexcept {
            return has_top && top_depth == 0;
        }
        void lower() noexcept {
            std::size_t level = 0;
            for (level = 0; level < node_stack_levels; level += 1) {
                indexes[level] = indexes[level + 1];
                donors[level] = donors[level + 1];
                has_donor[level] = has_donor[level + 1];
            }
            leaf_depth -= 1;
            has_top = leaf_depth > 0;
            top_depth = 0;
        }
        void record(std::size_t depth, ChildFix fix, std::size_t left_length) noexcept {
            const std::size_t child = indexes[depth];
            switch (fix) {
            case ChildFix::nothing:
                return;
            case ChildFix::merged_into_left:
                indexes[depth] = child - 1;
                indexes[depth + 1] += left_length + 1;
                break;
            case ChildFix::merged_right_in:
                break;
            case ChildFix::took_from_left:
                indexes[depth + 1] += 1;
                donors[depth] = child - 1;
                has_donor[depth] = true;
                break;
            case ChildFix::took_from_right:
                donors[depth] = child + 1;
                has_donor[depth] = true;
                break;
            default:
                return;
            }
            if (!has_top || depth < top_depth) {
                top_depth = depth;
            }
            has_top = true;
        }
    };

    // Removes the entry the stack ends at, rebalancing what the removal
    // leaves underfull and fitting changed prefixes; its key and value move
    // to `removed` (bbtree NodeStackMut::remove).
    static void remove(Host& host, Stack& stack, EntryBuffer<Key, Value>& removed) noexcept {
        if (stack.ends_at_leaf()) {
            const Leaf leaf = stack.leaf();
            const bool at_root = stack.is_at_top() && stack.top_is_root;
            if (leaf.keeps_its_minimum() || at_root) {
                leaf.remove(stack.index(), removed.key_storage(), removed.value_storage());
                removed.mark_filled();
                stack.detach();
                return;
            }
        }
        ChangedFences changed;
        take_out(host, stack, removed, changed);
        lower_and_refit_changed_fences(host, stack, changed);
        stack.detach();
    }

    static void take_out(Host& host, Stack& stack, EntryBuffer<Key, Value>& removed, ChangedFences& changed) noexcept {
        const std::size_t entry_depth = stack.depth();
        const std::size_t entry_index = stack.index();
        if (stack.ends_at_leaf()) {
            stack.leaf().remove(entry_index, removed.key_storage(), removed.value_storage());
            removed.mark_filled();
            fix_up_the_stack(host, stack, entry_index + 1, false, 0, 0, Window{}, changed);
            return;
        }
        refit_left_of_separator(stack);
        stack.set_index(entry_index + 1);
        stack.descend();
        stack.descend_to_first_leaf();
        const Window successor =
            stack.leaf().take_head_for_successor(removed.key_storage(), removed.value_storage());
        removed.mark_filled();
        fix_up_the_stack(host, stack, 0, true, entry_depth, entry_index, successor, changed);
    }

    // Goes up from the leaf, fixing each underfull child, and replacing the
    // separator at must_reach_depth with its successor on the way.
    static void fix_up_the_stack(
        Host& host, Stack& stack, std::size_t next_slot,
        bool must_reach, std::size_t must_reach_depth, std::size_t separator_index, Window successor,
        ChangedFences& changed) noexcept {
        // A separator's removal fits the nodes below it on the path, whose
        // lower fence its successor becomes, even where nothing rebalances.
        changed.start(stack.depth(), next_slot, must_reach, must_reach_depth);
        bool child_is_underfull = stack.leaf().is_underfull();
        while (!stack.is_at_top() && (child_is_underfull || (must_reach && stack.depth() > must_reach_depth))) {
            const Internal node = stack.ascend();
            const std::size_t depth = stack.depth();
            const std::size_t index = stack.index();
            changed.indexes[depth] = index;
            if (must_reach && depth == must_reach_depth) {
                const Window fill = node.window(separator_index);
                node.replace_separator(separator_index, successor.moved_back_to(fill));
            }
            if (child_is_underfull) {
                std::size_t left_length = 0;
                ChildFix fix = ChildFix::nothing;
                if (node.has_leaf_children()) {
                    fix = fix_underfull_leaf(host, node, index, &left_length);
                } else {
                    fix = fix_underfull_internal(host, node, index, &left_length);
                }
                const bool merged = fix == ChildFix::merged_into_left || fix == ChildFix::merged_right_in;
                changed.record(depth, fix, left_length);
                child_is_underfull = merged && node.is_underfull();
            }
        }
    }

    static void lower_and_refit_changed_fences(Host& host, Stack& stack, ChangedFences& changed) noexcept {
        if (changed.reached_top() && stack.top_is_root) {
            const NodePointer root = host.root();
            if (!root.is_leaf() && Internal::at(root).length() == 0) {
                host.lower_root();
                stack.start_at(host.root(), true);
                changed.lower();
            }
        }
        if (!changed.has_top) {
            return;
        }
        while (stack.depth() + 1 < changed.leaf_depth) {
            stack.set_index(changed.indexes[stack.depth()]);
            stack.descend();
        }
        while (true) {
            const std::size_t depth = stack.depth();
            const std::size_t child = changed.indexes[depth];
            stack.set_index(child);
            if (changed.has_donor[depth]) {
                refit_two_children(host, stack, child, changed.donors[depth]);
            } else {
                refit_one_child(host, stack, child);
            }
            if (depth == changed.top_depth) {
                return;
            }
            stack.ascend();
        }
    }

    [[nodiscard]] static std::size_t fixing_sibling(std::size_t index) noexcept {
        return index > 0 ? index - 1 : index + 1;
    }

    static ChildFix fix_underfull_leaf(Host& host, Internal parent, std::size_t index, std::size_t* left_length) noexcept {
        const Leaf child = Leaf::at(parent.edge(index));
        if (child.length() >= Leaf::minimum) {
            return ChildFix::nothing;
        }
        const std::size_t sibling = fixing_sibling(index);
        const std::size_t separator_index = index < sibling ? index : sibling;
        const bool child_is_right = sibling < index;
        const Leaf left = Leaf::at(parent.edge(separator_index));
        const Leaf right = Leaf::at(parent.edge(separator_index + 1));
        *left_length = left.length();
        if (left.length() + 1 + right.length() <= Leaf::capacity) {
            merge_leaves(host, parent, separator_index, left, right);
            return child_is_right ? ChildFix::merged_into_left : ChildFix::merged_right_in;
        }
        if (child_is_right) {
            steal_into_right_leaf(parent, separator_index, left, right);
            return ChildFix::took_from_left;
        }
        steal_into_left_leaf(parent, separator_index, left, right);
        return ChildFix::took_from_right;
    }

    static ChildFix fix_underfull_internal(Host& host, Internal parent, std::size_t index, std::size_t* left_length) noexcept {
        const Internal child = Internal::at(parent.edge(index));
        if (child.length() >= Internal::minimum) {
            return ChildFix::nothing;
        }
        const std::size_t sibling = fixing_sibling(index);
        const std::size_t separator_index = index < sibling ? index : sibling;
        const bool child_is_right = sibling < index;
        const Internal left = Internal::at(parent.edge(separator_index));
        const Internal right = Internal::at(parent.edge(separator_index + 1));
        *left_length = left.length();
        if (left.length() + 1 + right.length() <= Internal::capacity) {
            merge_internals(host, parent, separator_index, left, right);
            return child_is_right ? ChildFix::merged_into_left : ChildFix::merged_right_in;
        }
        if (child_is_right) {
            steal_into_right_internal(parent, separator_index, left, right);
            return ChildFix::took_from_left;
        }
        steal_into_left_internal(parent, separator_index, left, right);
        return ChildFix::took_from_right;
    }

    // Both children and their separator in the left one (bbtree
    // Siblings::merge): both take the parent's prefix first, filled from the
    // separator's window, which comes down at it.
    static void merge_leaves(Host& host, Internal parent, std::size_t separator_index, Leaf left, Leaf right) noexcept {
        const Window fill = parent.window(separator_index);
        parent.remove_with_right_child(separator_index);
        left.move_prefix_back_to(fill);
        right.move_prefix_back_to(fill);
        const std::size_t left_length = left.length();
        const std::size_t right_length = right.length();
        right.move_entries(0, left, left_length + 1, right_length + 1);
        left.inline_keys()[left_length] = Leaf::slot_of(fill.word);
        Leaf::copy_columns(right, 0, left, left_length + 1, right_length);
        HeaderAccess<Shape::latched>::set_length(left.page, left_length + 1 + right_length);
        left.write_tag(left_length, left.key(left_length));
        HeaderAccess<Shape::latched>::set_has_head(right.page, false);
        HeaderAccess<Shape::latched>::set_length(right.page, 0);
        host.release_page(PageKind::leaf, right.page);
    }

    static void merge_internals(Host& host, Internal parent, std::size_t separator_index, Internal left, Internal right) noexcept {
        const Window fill = parent.window(separator_index);
        const Window separator = parent.stored_window(separator_index);
        parent.remove_with_right_child(separator_index);
        left.move_prefix_back_to(fill);
        right.move_prefix_back_to(fill);
        const std::size_t left_length = left.length();
        const std::size_t right_length = right.length();
        left.inline_keys()[left_length] = Internal::slot_of(separator.word);
        right.move_windows(0, left, left_length + 1, right_length);
        right.move_edges(0, left, left_length + 1, right_length + 1);
        HeaderAccess<Shape::latched>::set_length(left.page, left_length + 1 + right_length);
        HeaderAccess<Shape::latched>::set_length(right.page, 0);
        host.release_page(PageKind::internal, right.page);
    }

    // The right child takes the left's last entry; the separator rotates
    // through the parent.
    static void steal_into_right_leaf(Internal parent, std::size_t separator_index, Leaf left, Leaf right) noexcept {
        const Window fill = parent.window(separator_index);
        right.move_prefix_back_to(fill);
        const std::size_t left_length = left.length();
        const std::size_t right_length = right.length();
        const Window rising = left.window(left_length - 1);
        right.move_entries(0, right, 1, right_length + 1);
        right.move_columns(0, 1, right_length);
        left.move_entries(left_length, right, 0, 1);
        right.inline_keys()[0] = Leaf::slot_of(fill.word);
        HeaderAccess<Shape::latched>::set_length(right.page, right_length + 1);
        right.write_tag(0, right.key(0));
        parent.replace_separator(separator_index, rising.moved_back_to(fill));
        left.set_length_refilling(left_length - 1);
    }

    static void steal_into_left_leaf(Internal parent, std::size_t separator_index, Leaf left, Leaf right) noexcept {
        const Window fill = parent.window(separator_index);
        left.move_prefix_back_to(fill);
        const std::size_t left_length = left.length();
        const std::size_t right_length = right.length();
        const Window rising = right.window(0);
        right.move_entries(0, left, left_length + 1, 1);
        left.inline_keys()[left_length] = Leaf::slot_of(fill.word);
        HeaderAccess<Shape::latched>::set_length(left.page, left_length + 1);
        left.write_tag(left_length, left.key(left_length));
        right.move_entries(1, right, 0, right_length);
        right.move_columns(1, 0, right_length - 1);
        parent.replace_separator(separator_index, rising.moved_back_to(fill));
        right.set_length_refilling(right_length - 1);
    }

    static void steal_into_right_internal(Internal parent, std::size_t separator_index, Internal left, Internal right) noexcept {
        const Window fill = parent.window(separator_index);
        right.move_prefix_back_to(fill);
        const std::size_t left_length = left.length();
        const std::size_t right_length = right.length();
        const Window rising = left.stored_window(left_length - 1);
        const Window falling = parent.stored_window(separator_index);
        const NodePointer moved_edge = left.edge(left_length);
        parent.replace_separator(separator_index, rising.moved_back_to(fill));
        right.move_windows(0, right, 1, right_length);
        right.inline_keys()[0] = Internal::slot_of(falling.word);
        right.move_edges(0, right, 1, right_length + 1);
        right.set_edge(0, moved_edge);
        HeaderAccess<Shape::latched>::set_length(right.page, right_length + 1);
        left.set_length_refilling(left_length - 1);
    }

    static void steal_into_left_internal(Internal parent, std::size_t separator_index, Internal left, Internal right) noexcept {
        const Window fill = parent.window(separator_index);
        left.move_prefix_back_to(fill);
        const std::size_t left_length = left.length();
        const std::size_t right_length = right.length();
        const Window rising = right.stored_window(0);
        const Window falling = parent.stored_window(separator_index);
        const NodePointer moved_edge = right.edge(0);
        parent.replace_separator(separator_index, rising.moved_back_to(fill));
        left.inline_keys()[left_length] = Internal::slot_of(falling.word);
        left.set_edge(left_length + 1, moved_edge);
        HeaderAccess<Shape::latched>::set_length(left.page, left_length + 1);
        right.move_windows(1, right, 0, right_length - 1);
        right.move_edges(1, right, 0, right_length);
        right.set_length_refilling(right_length - 1);
    }
};

} // namespace stx_btree::detail
