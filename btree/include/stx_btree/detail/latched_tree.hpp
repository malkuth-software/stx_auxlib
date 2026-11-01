#pragma once

// The tree threads share (bbtree tree/latched/, D54-D57): the unsync tree's
// nodes and operations, shared through optimistic lock coupling.
//
// - Internal nodes are read in place without a latch and validated by their
//   version; writers holding them change them in place.
// - Leaves are latched shared or exclusive; lookups read them optimistically
//   first and copy trivially copyable values out unlatched (D57).
// - A change that a leaf cannot take alone descends again keeping the path,
//   takes without waiting every node the unsync operation will change or
//   read a separator of, reserves its pages, then runs that operation along
//   a node stack from the topmost node it holds (D54).
// - Freed pages go on per-kind free lists and keep their latch words while
//   the tree lives, so a racing reader always reads a node of the kind its
//   pointer says, and fails its version check (type-stable nodes).
//
// Racing reads of node bytes are validated by versions, as in bbtree and in
// Leis et al.'s optimistic lock coupling; the language counts them as data
// races. Header fields, edges, latch words and the root pointer are atomic.

#include "stx_btree/detail/latch.hpp"
#include "stx_btree/detail/node.hpp"
#include "stx_btree/detail/node_stack.hpp"
#include "stx_btree/detail/root_page.hpp"
#include "stx_btree/detail/tree_algorithms.hpp"
#include "stx_btree/detail/tree_walk.hpp"
#include "stx_btree/page_allocator.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>

namespace stx_btree::detail {

enum class LatchMode : std::uint8_t { optimistic, shared, exclusive };

enum class DescentOutcome : std::uint8_t {
    // The leaf that covers the key is held in the requested mode.
    inside,
    // The key lacks what every key shares and sorts before (after) them.
    before_every_key,
    after_every_key
};

// How a descent went through one internal node (bbtree Routed). This and
// PathLevel are trivially constructible: a path's 23 levels are written
// only as a descent passes them, not zeroed when a path is made (bbtree
// 09-concurrency.md, Scans: a path written whole cost each scan 920 bytes).
struct Routed final {
    std::size_t edge;
    std::size_t length;
    bool at_separator;
};

struct PathLevel final {
    std::byte* node;
    Version version;
    Routed routed;
};

// The internal nodes a descent passed, with the versions it read (bbtree
// TreePath). Levels past `depth` are not written until used.
class TreePath final {
public:
    PathLevel levels[maximum_depth];
    std::size_t depth = 0;

    void clear() noexcept {
        depth = 0;
    }
    void record(std::byte* node, Version version, Routed routed) noexcept {
        levels[depth] = PathLevel{.node = node, .version = version, .routed = routed};
        depth += 1;
    }
    // The topmost level a change below `level` must hold so that the fences
    // of every node it changes stay put: the nearest levels above it with a
    // separator on each side of the path.
    [[nodiscard]] std::size_t fences_top(std::size_t level) const noexcept {
        std::size_t top = level;
        bool low_found = false;
        bool high_found = false;
        std::size_t above = level;
        while (above > 0 && !(low_found && high_found)) {
            above -= 1;
            const Routed routed = levels[above].routed;
            if (!low_found && routed.edge > 0) {
                low_found = true;
                top = above < top ? above : top;
            }
            if (!high_found && routed.edge < routed.length) {
                high_found = true;
                top = above < top ? above : top;
            }
        }
        return top;
    }
};

// A descent that keeps no path.
class NoPath final {
public:
    void clear() noexcept {}
    void record(std::byte*, Version, Routed) noexcept {}
};

// Nodes an operation took without waiting, let go of together (bbtree
// HeldNodes); taking a node twice is taking it once.
class HeldNodes final {
public:
    static constexpr std::size_t internal_capacity = 3 * maximum_depth + 2;
    static constexpr std::size_t leaf_capacity = 4;

    HeldNodes() noexcept = default;
    HeldNodes(const HeldNodes&) = delete;
    HeldNodes& operator=(const HeldNodes&) = delete;
    ~HeldNodes() {
        release_all();
    }

    [[nodiscard]] bool holds(std::byte* page) const noexcept {
        std::size_t index = 0;
        for (index = 0; index < internal_count; index += 1) {
            if (internals[index].page == page) {
                return true;
            }
        }
        for (index = 0; index < leaf_count; index += 1) {
            if (leaves[index].page == page) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool take_internal_at(std::byte* page, Version version) noexcept {
        if (holds(page)) {
            return true;
        }
        if (!NodeLatch::of(page).try_lock_at(version)) {
            return false;
        }
        internals[internal_count] = Taken{.page = page, .version = version};
        internal_count += 1;
        return true;
    }

    [[nodiscard]] bool take_internal(std::byte* page) noexcept {
        if (holds(page)) {
            return true;
        }
        Version version{0};
        if (!NodeLatch::of(page).try_read_version(&version)) {
            return false;
        }
        return take_internal_at(page, version);
    }

    [[nodiscard]] bool take_leaf(std::byte* page) noexcept {
        if (holds(page)) {
            return true;
        }
        Version version{0};
        if (!NodeLatch::of(page).try_lock_exclusive(&version)) {
            return false;
        }
        leaves[leaf_count] = Taken{.page = page, .version = version};
        leaf_count += 1;
        return true;
    }

    // A leaf already held exclusively at `version`, now let go of with the
    // rest.
    void adopt_leaf(std::byte* page, Version version) noexcept {
        leaves[leaf_count] = Taken{.page = page, .version = version};
        leaf_count += 1;
    }

    [[nodiscard]] bool take_path(const TreePath& path, std::size_t top) noexcept {
        std::size_t level = 0;
        for (level = top; level < path.depth; level += 1) {
            const PathLevel& taken = path.levels[level];
            if (!take_internal_at(taken.node, taken.version)) {
                return false;
            }
        }
        return true;
    }

    void release_all() noexcept {
        std::size_t index = 0;
        for (index = 0; index < internal_count; index += 1) {
            NodeLatch::of(internals[index].page).unlock_exclusive(internals[index].version);
        }
        for (index = 0; index < leaf_count; index += 1) {
            NodeLatch::of(leaves[index].page).unlock_exclusive(leaves[index].version);
        }
        internal_count = 0;
        leaf_count = 0;
    }

private:
    // Zeroed when made: a set of held nodes is made only for a change larger
    // than a leaf, and zeroing lets compilers see every slot read is written.
    struct Taken final {
        std::byte* page;
        Version version;
    };
    Taken internals[internal_capacity] = {};
    std::size_t internal_count = 0;
    Taken leaves[leaf_capacity] = {};
    std::size_t leaf_count = 0;
};

template <typename Shape, typename Allocator>
class LatchedTree final {
public:
    using Key = typename Shape::KeyType;
    using Value = typename Shape::ValueType;
    using Query = typename Shape::Query;
    using Traits = typename Shape::Traits;
    using Leaf = LeafNode<Shape>;
    using Internal = InternalNode<Shape>;
    using Stack = NodeStack<Shape>;
    using Root = RootPage<Shape>;
    using Walk = TreeWalk<Shape>;

    static_assert(Shape::latched, "a latched tree's nodes carry latch words");
    static_assert(!Traits::orders_by_own_bytes || std::is_trivially_copyable_v<Key>,
                  "keys compared after a racing copy must be trivially copyable");

    // A leaf latched in one mode, let go of when this goes away.
    class HeldLeaf final {
    public:
        HeldLeaf() noexcept = default;
        HeldLeaf(const HeldLeaf&) = delete;
        HeldLeaf& operator=(const HeldLeaf&) = delete;
        ~HeldLeaf() {
            release();
        }

        void lock(Leaf target, LatchMode latch_mode) noexcept {
            release();
            leaf = target;
            mode = latch_mode;
            const NodeLatch latch = NodeLatch::of(target.page);
            if (latch_mode == LatchMode::shared) {
                latch.lock_shared();
            } else if (latch_mode == LatchMode::exclusive) {
                version = latch.lock_exclusive();
            } else {
                version = latch.read_version();
            }
            holding = true;
        }
        // Takes the leaf exclusively at `at`, never waiting.
        [[nodiscard]] bool try_lock_exclusive_at(Leaf target, Version at) noexcept {
            release();
            if (!NodeLatch::of(target.page).try_lock_at(at)) {
                return false;
            }
            leaf = target;
            mode = LatchMode::exclusive;
            version = at;
            holding = true;
            return true;
        }
        [[nodiscard]] bool is_unchanged() const noexcept {
            if (mode != LatchMode::optimistic) {
                return true;
            }
            return NodeLatch::of(leaf.page).is_unchanged(version);
        }
        void release() noexcept {
            if (!holding) {
                return;
            }
            holding = false;
            const NodeLatch latch = NodeLatch::of(leaf.page);
            if (mode == LatchMode::shared) {
                latch.unlock_shared();
            } else if (mode == LatchMode::exclusive) {
                latch.unlock_exclusive(version);
            }
        }
        // Hands an exclusive hold to an operation, which lets go of it.
        void hand_over(HeldNodes& held) noexcept {
            held.adopt_leaf(leaf.page, version);
            holding = false;
        }

        Leaf leaf;
        Version version{0};
        LatchMode mode = LatchMode::optimistic;
        bool holding = false;
    };

    // One structural change: the host TreeAlgorithms runs on. It owns the
    // nodes taken, the pages reserved, taken and freed, and the root
    // pointer as the change leaves it; finishing publishes the root, lets go
    // of every node and only then puts freed pages on their free lists.
    class Operation final {
    public:
        explicit Operation(LatchedTree& owner) noexcept
            : tree(owner), root_tagged(owner.load_root()), original_root(owner.load_root()) {}
        Operation(const Operation&) = delete;
        Operation& operator=(const Operation&) = delete;
        ~Operation() {
            finish();
        }

        [[nodiscard]] NodePointer root() const noexcept {
            return root_tagged;
        }
        [[nodiscard]] std::size_t shared_length() const noexcept {
            return Root{root_tagged.page()}.shared_length();
        }

        // Takes the pages first, from the free lists, then the allocator;
        // on failure gives them back and rethrows, nothing changed.
        void reserve_pages(std::size_t leaves, std::size_t internals) {
            try {
                while (reserve.leaf_count < leaves) {
                    reserve.leaves[reserve.leaf_count] = tree.take_free_or_new(PageKind::leaf);
                    reserve.leaf_count += 1;
                }
                while (reserve.internal_count < internals) {
                    reserve.internals[reserve.internal_count] = tree.take_free_or_new(PageKind::internal);
                    reserve.internal_count += 1;
                }
            } catch (...) {
                give_back_reserve();
                throw;
            }
        }

        [[nodiscard]] std::byte* take_page(PageReserve& pages, PageKind kind) noexcept {
            std::byte* page = pages.take(kind);
            // A reader that reached a free page through a stale pointer may
            // hold it a moment; it waits for nothing.
            const Version version = NodeLatch::of(page).lock_exclusive();
            allocated[allocated_count] = TakenPage{.page = page, .version = version, .kind = kind};
            allocated_count += 1;
            return page;
        }

        void release_page(PageKind kind, std::byte* page) noexcept {
            freed[freed_count] = TakenPage{.page = page, .version = Version{0}, .kind = kind};
            freed_count += 1;
        }

        [[nodiscard]] bool has_freed(std::byte* page) const noexcept {
            std::size_t index = 0;
            for (index = 0; index < freed_count; index += 1) {
                if (freed[index].page == page) {
                    return true;
                }
            }
            return false;
        }

        // The root node moves to a fresh page of its kind; the root page,
        // held by this operation, becomes an internal node above it.
        NodePointer raise_root(PageReserve& pages) noexcept {
            const Root root = Root{root_tagged.page()};
            NodePointer child;
            if (root_tagged.is_leaf()) {
                const Leaf moved = NodeMove<Shape>::move_leaf(Leaf::at(root_tagged), take_page(pages, PageKind::leaf));
                child = moved.pointer();
            } else {
                const Internal moved =
                    NodeMove<Shape>::move_internal(Internal::at(root_tagged), take_page(pages, PageKind::internal));
                child = moved.pointer();
            }
            const Internal top = Internal::initialize(root.page, child, root.shared_length());
            root_tagged = top.pointer();
            return root_tagged;
        }

        void lower_root() noexcept {
            const Root root = Root{root_tagged.page()};
            const NodePointer child = Internal::at(root_tagged).edge(0);
            if (child.is_leaf()) {
                const Leaf moved = NodeMove<Shape>::move_leaf(Leaf::at(child), root.page);
                root_tagged = moved.pointer();
                release_page(PageKind::leaf, child.page());
            } else {
                const Internal moved = NodeMove<Shape>::move_internal(Internal::at(child), root.page);
                root_tagged = moved.pointer();
                release_page(PageKind::internal, child.page());
            }
        }

        LatchedTree& tree;
        NodePointer root_tagged;
        NodePointer original_root;
        HeldNodes held;
        PageReserve reserve;

    private:
        struct TakenPage final {
            std::byte* page;
            Version version;
            PageKind kind;
        };

        void give_back_reserve() noexcept {
            while (reserve.leaf_count > 0) {
                tree.free_list(PageKind::leaf).push(reserve.take(PageKind::leaf));
            }
            while (reserve.internal_count > 0) {
                tree.free_list(PageKind::internal).push(reserve.take(PageKind::internal));
            }
        }

        void finish() noexcept {
            if (!(root_tagged == original_root)) {
                tree.root_word.store(root_tagged.tagged, std::memory_order_release);
            }
            held.release_all();
            std::size_t index = 0;
            for (index = 0; index < allocated_count; index += 1) {
                NodeLatch::of(allocated[index].page).unlock_exclusive(allocated[index].version);
            }
            allocated_count = 0;
            for (index = 0; index < freed_count; index += 1) {
                tree.free_list(freed[index].kind).push(freed[index].page);
            }
            freed_count = 0;
            give_back_reserve();
        }

        TakenPage allocated[PageReserve::capacity + 2];
        std::size_t allocated_count = 0;
        TakenPage freed[2 * node_stack_levels + 2];
        std::size_t freed_count = 0;
    };

    using Algorithms = TreeAlgorithms<Shape, Operation>;

    LatchedTree() noexcept = default;
    explicit LatchedTree(const Allocator& allocator) noexcept : page_allocator(allocator) {}
    LatchedTree(const LatchedTree&) = delete;
    LatchedTree& operator=(const LatchedTree&) = delete;
    ~LatchedTree() {
        destroy();
    }

    // --- Roots and pages ---------------------------------------------------

    [[nodiscard]] NodePointer load_root() const noexcept {
        return NodePointer{root_word.load(std::memory_order_acquire)};
    }
    [[nodiscard]] bool is_root(Leaf leaf) const noexcept {
        return root_word.load(std::memory_order_acquire) == leaf.pointer().tagged;
    }
    [[nodiscard]] FreeList& free_list(PageKind kind) noexcept {
        return kind == PageKind::leaf ? free_leaves : free_internals;
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

    void deallocate_page(PageKind kind, std::byte* page) noexcept {
        if (kind == PageKind::leaf) {
            page_allocator.deallocate_page(kind, page, Shape::leaf.page_bytes, Shape::leaf.page_alignment);
        } else if (kind == PageKind::internal) {
            page_allocator.deallocate_page(kind, page, Shape::internal.page_bytes, Shape::internal.page_alignment);
        } else {
            page_allocator.deallocate_page(kind, page, Shape::root.page_bytes, Shape::root.page_alignment);
        }
    }

    [[nodiscard]] std::byte* take_free_or_new(PageKind kind) {
        std::byte* page = free_list(kind).pop();
        if (page != nullptr) {
            return page;
        }
        return allocate_page(kind);
    }

    // The first insert's root page; a thread that loses the race to publish
    // its own frees it (bbtree start_tree).
    void start_tree() {
        std::byte* page = allocate_page(PageKind::root);
        const NodePointer root = Root::initialize_empty(page);
        const NodeLatch latch = NodeLatch::of(page);
        const Version version = latch.lock_exclusive();
        std::uintptr_t expected = 0;
        if (root_word.compare_exchange_strong(expected, root.tagged, std::memory_order_acq_rel, std::memory_order_acquire)) {
            rightmost_leaf_word.store(root.tagged, std::memory_order_release);
            latch.unlock_exclusive(version);
            return;
        }
        deallocate_page(PageKind::root, page);
    }

    // --- Descent (bbtree latched/descent.rs) --------------------------------

    // Whether the query lacks the shared prefix in `shared`, and where it
    // then sorts.
    [[nodiscard]] static DescentOutcome place_against(const SharedPrefixBytes& shared, const Query& query) noexcept {
        if (shared.length == 0) {
            return DescentOutcome::inside;
        }
        return order_to_outcome(Traits::compare_shared(query, shared.bytes, shared.length));
    }

    [[nodiscard]] static DescentOutcome order_to_outcome(int order) noexcept {
        if (order < 0) {
            return DescentOutcome::before_every_key;
        }
        if (order > 0) {
            return DescentOutcome::after_every_key;
        }
        return DescentOutcome::inside;
    }

    // The same, read from the root page in place: racing a shrink, the
    // bytes read are vouched for by the root's version afterward, as any
    // other racing read; under the root leaf's latch they hold still.
    [[nodiscard]] STX_BTREE_ALWAYS_INLINE static DescentOutcome place_against_root(NodePointer root, const Query& query) noexcept {
        const Root root_page = Root{root.page()};
        const std::size_t racing_length = root_page.shared_length();
        if (racing_length == 0) {
            return DescentOutcome::inside;
        }
        const std::size_t length = racing_length < shared_prefix_capacity ? racing_length : shared_prefix_capacity;
        return order_to_outcome(Traits::compare_shared(query, root_page.shared_bytes(), length));
    }

    // Routes the query through an internal node read racing writers; false
    // if a check failed and the descent must start over.
    [[nodiscard]] STX_BTREE_ALWAYS_INLINE static bool route_racing(
        Internal node, Version version, const Query& query, Routed* routed) noexcept {
        const std::size_t racing_length = node.length();
        const std::size_t length = racing_length < Internal::capacity ? racing_length : Internal::capacity;
        const std::size_t prefix = node.prefix_length();
        const InlineWord word = Shape::window_word(query, prefix);
        const EqualRange range = Internal::Count::equal_range(node.inline_keys(), Internal::slot_of(word), length);
        if (range.begin == range.end) {
            *routed = Routed{.edge = range.begin, .length = length, .at_separator = false};
            return true;
        }
        return route_tied(node, version, query, range, length, prefix, routed);
    }

    // Tied separators compare their keys, the heads below them (D49): copied
    // and compared where keys order by their own bytes, else read with the
    // head's leaf latched shared; the node's version vouches for each.
    [[nodiscard]] STX_BTREE_NEVER_INLINE static bool route_tied(
        Internal node, Version version, const Query& query, EqualRange range,
        std::size_t length, std::size_t prefix, Routed* routed) noexcept {
        const std::size_t equal_length = prefix + Internal::width;
        const NodeLatch node_latch = NodeLatch::of(node.page);
        std::size_t low = range.begin;
        std::size_t high = range.end;
        while (low < high) {
            const std::size_t middle = low + (high - low) / 2;
            NodePointer walk = node.edge(middle + 1);
            if (walk.is_null() || !node_latch.is_unchanged(version)) {
                return false;
            }
            while (!walk.is_leaf()) {
                walk = Internal::at(walk).edge(0);
                if (walk.is_null()) {
                    return false;
                }
            }
            int order = 0;
            if (!compare_separator_head(node_latch, version, Leaf::at(walk), query, equal_length, &order)) {
                return false;
            }
            if (order == 0) {
                *routed = Routed{.edge = middle + 1, .length = length, .at_separator = true};
                return true;
            }
            if (order < 0) {
                high = middle;
            } else {
                low = middle + 1;
            }
        }
        *routed = Routed{.edge = low, .length = length, .at_separator = false};
        return true;
    }

    [[nodiscard]] static bool compare_separator_head(
        NodeLatch node_latch, Version version, Leaf leaf, const Query& query,
        std::size_t equal_length, int* order) noexcept {
        if constexpr (Traits::orders_by_own_bytes) {
            if (!leaf.has_head()) {
                return false;
            }
            alignas(Key) std::byte copy[sizeof(Key)];
            std::memcpy(copy, leaf.entry(0), sizeof(Key));
            const Key* head = std::launder(reinterpret_cast<const Key*>(copy));
            const int compared = Traits::compare_from(query, *head, equal_length);
            if (!node_latch.is_unchanged(version)) {
                return false;
            }
            *order = compared;
            return true;
        } else {
            const NodeLatch leaf_latch = NodeLatch::of(leaf.page);
            leaf_latch.lock_shared();
            const bool valid = node_latch.is_unchanged(version) && leaf.has_head();
            if (valid) {
                *order = Traits::compare_from(query, leaf.head_key(), equal_length);
            }
            leaf_latch.unlock_shared();
            return valid;
        }
    }

    // Down to the leaf that covers the query, holding it in `Mode`; the
    // internal nodes are read optimistically and recorded in `path`. Any
    // failed check starts over from the root.
    template <LatchMode Mode, typename Path>
    [[nodiscard]] STX_BTREE_ALWAYS_INLINE DescentOutcome descend(const Query& query, Path& path, HeldLeaf& held, bool* at_separator) const noexcept {
        while (true) {
            path.clear();
            held.release();
            const NodePointer root = load_root();
            if (root.is_leaf()) {
                const Leaf leaf = Leaf::at(root);
                held.lock(leaf, Mode);
                if (!is_root(leaf)) {
                    continue;
                }
                const DescentOutcome outcome = place_against_root(root, query);
                if (Mode == LatchMode::optimistic && !held.is_unchanged()) {
                    continue;
                }
                if (outcome != DescentOutcome::inside) {
                    held.release();
                    return outcome;
                }
                *at_separator = false;
                return DescentOutcome::inside;
            }
            Internal node = Internal::at(root);
            Version version = NodeLatch::of(node.page).read_version();
            if (!(load_root() == root)) {
                continue;
            }
            const DescentOutcome outcome = place_against_root(root, query);
            if (outcome != DescentOutcome::inside) {
                if (NodeLatch::of(node.page).is_unchanged(version)) {
                    return outcome;
                }
                continue;
            }
            bool separator = false;
            bool at_root = true;
            bool restart = false;
            while (true) {
                Routed routed{0, 0, false};
                if (!route_racing(node, version, query, &routed)) {
                    restart = true;
                    break;
                }
                const NodePointer child = node.edge(routed.edge);
                if (child.is_null()) {
                    restart = true;
                    break;
                }
                // The root page alone changes kind: its edge is followed
                // only once its version vouches for it.
                if (at_root && !NodeLatch::of(node.page).is_unchanged(version)) {
                    restart = true;
                    break;
                }
                at_root = false;
                separator = separator || routed.at_separator;
                path.record(node.page, version, routed);
                if (child.is_leaf()) {
                    const Leaf leaf = Leaf::at(child);
                    held.lock(leaf, Mode);
                    if (!NodeLatch::of(node.page).is_unchanged(version)) {
                        restart = true;
                        break;
                    }
                    if (Mode != LatchMode::optimistic && separator && !is_head(leaf, query)) {
                        restart = true;
                        break;
                    }
                    *at_separator = separator;
                    return DescentOutcome::inside;
                }
                const Internal child_node = Internal::at(child);
                const Version child_version = NodeLatch::of(child_node.page).read_version();
                if (!NodeLatch::of(node.page).is_unchanged(version)) {
                    restart = true;
                    break;
                }
                node = child_node;
                version = child_version;
            }
            if (restart) {
                continue;
            }
        }
    }

    // Whether the latched leaf's head is the query: a separator matched
    // above may have been replaced since.
    [[nodiscard]] static bool is_head(Leaf leaf, const Query& query) noexcept {
        if (!leaf.has_head()) {
            return false;
        }
        return Traits::compare_from(query, leaf.head_key(), 0) == 0;
    }

    // --- Lookups (bbtree latched/lookup.rs) ---------------------------------

    // A copy of the query's value into `out`, read without a latch where the
    // leaf settles the key (D57).
    [[nodiscard]] bool get_copied(const Query& query, Value* out) const noexcept {
        static_assert(std::is_trivially_copyable_v<Value>, "get_copied copies values racing writers");
        if (load_root().is_null()) {
            return false;
        }
        HeldLeaf at;
        bool at_separator = false;
        NoPath no_path;
        const DescentOutcome outcome = descend<LatchMode::optimistic>(query, no_path, at, &at_separator);
        if (outcome != DescentOutcome::inside) {
            return false;
        }
        const Leaf leaf = at.leaf;
        if (at_separator) {
            if constexpr (Traits::orders_by_own_bytes) {
                if (leaf.has_head()) {
                    alignas(Key) std::byte key_copy[sizeof(Key)];
                    alignas(Value) std::byte value_copy[sizeof(Value)];
                    std::memcpy(key_copy, leaf.entry(0), sizeof(Key));
                    std::memcpy(value_copy, leaf.entry(0) + Shape::leaf.value_offset, sizeof(Value));
                    if (at.is_unchanged()) {
                        const Key* head = std::launder(reinterpret_cast<const Key*>(key_copy));
                        if (Traits::compare_from(query, *head, 0) == 0) {
                            std::memcpy(static_cast<void*>(out), value_copy, sizeof(Value));
                            return true;
                        }
                    }
                }
            }
            return read_latched_copy(query, out);
        }
        const std::size_t racing_length = leaf.length();
        const std::size_t length = racing_length < Leaf::capacity ? racing_length : Leaf::capacity;
        const std::size_t prefix = leaf.prefix_length();
        const InlineWord word = Shape::window_word(query, prefix);
        const EqualRange range = Leaf::Count::equal_range(leaf.inline_keys(), Leaf::slot_of(word), length);
        const std::size_t count = range.end - range.begin;
        if (count == 0) {
            if (at.is_unchanged()) {
                return false;
            }
            return read_latched_copy(query, out);
        }
        if (count == 1) {
            const std::size_t index = range.begin;
            if constexpr (Traits::orders_by_own_bytes) {
                alignas(Key) std::byte key_copy[sizeof(Key)];
                alignas(Value) std::byte value_copy[sizeof(Value)];
                std::memcpy(key_copy, leaf.entry(index + 1), sizeof(Key));
                std::memcpy(value_copy, leaf.entry(index + 1) + Shape::leaf.value_offset, sizeof(Value));
                if (!at.is_unchanged()) {
                    return read_latched_copy(query, out);
                }
                const Key* stored = std::launder(reinterpret_cast<const Key*>(key_copy));
                if (Traits::compare_from(query, *stored, 0) != 0) {
                    return false;
                }
                std::memcpy(static_cast<void*>(out), value_copy, sizeof(Value));
                return true;
            } else if constexpr (Traits::tag_is_length && Shape::leaf.has_hash_tags) {
                // A key that ends within its window is the candidate of its
                // length; the tag tells the length below 255 (D36, D57).
                const std::size_t query_length = Traits::encoded_length(query);
                const std::uint8_t query_tag = Traits::hash_tag(query);
                if (query_length <= prefix + Leaf::width && query_tag < 255) {
                    const std::atomic_ref<std::uint8_t> tag_slot(leaf.hash_tags()[index]);
                    const std::uint8_t tag = tag_slot.load(std::memory_order_relaxed);
                    alignas(Value) std::byte value_copy[sizeof(Value)];
                    std::memcpy(value_copy, leaf.entry(index + 1) + Shape::leaf.value_offset, sizeof(Value));
                    if (!at.is_unchanged()) {
                        return read_latched_copy(query, out);
                    }
                    if (tag != query_tag) {
                        return false;
                    }
                    std::memcpy(static_cast<void*>(out), value_copy, sizeof(Value));
                    return true;
                }
            }
        }
        // Latch the same leaf at the same version and search it there.
        const NodeLatch latch = NodeLatch::of(leaf.page);
        latch.lock_shared();
        if (!latch.is_unchanged(at.version)) {
            latch.unlock_shared();
            return read_latched_copy(query, out);
        }
        const std::size_t found = leaf.find(query);
        const bool present = found != Leaf::absent;
        if (present) {
            std::memcpy(static_cast<void*>(out), static_cast<const void*>(&leaf.value(found)), sizeof(Value));
        }
        latch.unlock_shared();
        return present;
    }

    // A leaf that changed under an optimistic read: latch the way down,
    // which waits for a writer rather than racing it again.
    [[nodiscard]] STX_BTREE_NEVER_INLINE bool read_latched_copy(const Query& query, Value* out) const noexcept {
        HeldLeaf held;
        bool at_separator = false;
        NoPath no_path;
        const DescentOutcome outcome = descend<LatchMode::shared>(query, no_path, held, &at_separator);
        if (outcome != DescentOutcome::inside) {
            return false;
        }
        if (at_separator) {
            std::memcpy(static_cast<void*>(out), static_cast<const void*>(&held.leaf.head_value()), sizeof(Value));
            return true;
        }
        const std::size_t found = held.leaf.find(query);
        if (found == Leaf::absent) {
            return false;
        }
        std::memcpy(static_cast<void*>(out), static_cast<const void*>(&held.leaf.value(found)), sizeof(Value));
        return true;
    }

    // Runs visitor(key, value) on the query's entry with its leaf latched
    // shared; false if absent.
    template <typename Visitor>
    [[nodiscard]] bool read(const Query& query, Visitor& visitor) const {
        if (load_root().is_null()) {
            return false;
        }
        HeldLeaf at;
        bool at_separator = false;
        NoPath no_path;
        const DescentOutcome outcome = descend<LatchMode::optimistic>(query, no_path, at, &at_separator);
        if (outcome != DescentOutcome::inside) {
            return false;
        }
        const Leaf leaf = at.leaf;
        const Version version = at.version;
        at.release();
        HeldLeaf held;
        held.lock(leaf, LatchMode::shared);
        if (!NodeLatch::of(leaf.page).is_unchanged(version)) {
            held.release();
            return read_latched(query, visitor);
        }
        if (at_separator) {
            if (!is_head(leaf, query)) {
                held.release();
                return read_latched(query, visitor);
            }
            visitor(static_cast<const Key&>(leaf.head_key()), static_cast<const Value&>(leaf.head_value()));
            return true;
        }
        const std::size_t found = leaf.find(query);
        if (found == Leaf::absent) {
            return false;
        }
        visitor(static_cast<const Key&>(leaf.key(found)), static_cast<const Value&>(leaf.value(found)));
        return true;
    }

    template <typename Visitor>
    [[nodiscard]] STX_BTREE_NEVER_INLINE bool read_latched(const Query& query, Visitor& visitor) const {
        HeldLeaf held;
        bool at_separator = false;
        NoPath no_path;
        const DescentOutcome outcome = descend<LatchMode::shared>(query, no_path, held, &at_separator);
        if (outcome != DescentOutcome::inside) {
            return false;
        }
        if (at_separator) {
            visitor(static_cast<const Key&>(held.leaf.head_key()), static_cast<const Value&>(held.leaf.head_value()));
            return true;
        }
        const std::size_t found = held.leaf.find(query);
        if (found == Leaf::absent) {
            return false;
        }
        visitor(static_cast<const Key&>(held.leaf.key(found)), static_cast<const Value&>(held.leaf.value(found)));
        return true;
    }

    // Runs visitor(key, value&) with the leaf latched exclusively.
    template <typename Visitor>
    [[nodiscard]] bool update(const Query& query, Visitor& visitor) {
        if (load_root().is_null()) {
            return false;
        }
        HeldLeaf held;
        bool at_separator = false;
        NoPath no_path;
        const DescentOutcome outcome = descend<LatchMode::exclusive>(query, no_path, held, &at_separator);
        if (outcome != DescentOutcome::inside) {
            return false;
        }
        if (at_separator) {
            visitor(static_cast<const Key&>(held.leaf.head_key()), held.leaf.head_value());
            return true;
        }
        const std::size_t found = held.leaf.find(query);
        if (found == Leaf::absent) {
            return false;
        }
        visitor(static_cast<const Key&>(held.leaf.key(found)), held.leaf.value(found));
        return true;
    }

    // --- Insertion (bbtree latched/insert.rs, append.rs) --------------------
    //
    // A Policy decides what an insert does: occupied(stored key, stored
    // value, key, value) for a key present, vacant(key, value) just before a
    // new entry goes in; each returns Policy::Output.

    enum class InsertStep : std::uint8_t { done, full };

    template <typename Policy>
    typename Policy::Output insert(Key&& key, Value&& value, Policy& policy) {
        if (load_root().is_null()) {
            start_tree();
        }
        std::optional<typename Policy::Output> result;
        if (try_append(key, value, policy, result)) {
            return std::move(*result);
        }
        while (true) {
            const Query query = Traits::query_of(key);
            HeldLeaf held;
            bool at_separator = false;
            NoPath no_path;
            const DescentOutcome outcome = descend<LatchMode::exclusive>(query, no_path, held, &at_separator);
            if (outcome != DescentOutcome::inside) {
                held.release();
                shrink_shared_prefix_for(query);
                continue;
            }
            std::size_t edge = 0;
            const InsertStep step = insert_into(held, at_separator, key, value, policy, result, &edge);
            if (step == InsertStep::done) {
                return std::move(*result);
            }
            held.release();
            return insert_splitting(std::move(key), std::move(value), policy);
        }
    }

    // An append to the last leaf, without a descent, when the key goes after
    // every key (D45).
    template <typename Policy>
    [[nodiscard]] bool try_append(Key& key, Value& value, Policy& policy, std::optional<typename Policy::Output>& result) {
        const NodePointer last = NodePointer{rightmost_leaf_word.load(std::memory_order_acquire)};
        if (last.is_null()) {
            return false;
        }
        const Leaf leaf = Leaf::at(last);
        const Query query = Traits::query_of(key);
        // A racing glance first: no latch for a key that cannot go last.
        const std::size_t racing_length = leaf.length();
        if (racing_length == 0 || racing_length > Leaf::capacity) {
            return false;
        }
        const InlineWord racing_word = Shape::window_word(query, leaf.prefix_length());
        if (leaf.inline_keys()[racing_length - 1] > Leaf::slot_of(racing_word)) {
            return false;
        }
        HeldLeaf held;
        held.lock(leaf, LatchMode::exclusive);
        if (rightmost_leaf_word.load(std::memory_order_relaxed) != last.tagged) {
            return false;
        }
        const std::size_t length = leaf.length();
        if (length == 0 || length >= Leaf::capacity) {
            return false;
        }
        if (Traits::compare_from(query, leaf.key(length - 1), 0) <= 0) {
            return false;
        }
        if (!Algorithms::has_shared_prefix(load_root(), query)) {
            return false;
        }
        result.emplace(policy.vacant(static_cast<const Key&>(key), static_cast<const Value&>(value)));
        const InlineWord word = Shape::window_word(query, leaf.prefix_length());
        leaf.insert_fit(length, std::move(key), std::move(value), word);
        return true;
    }

    template <typename Policy>
    [[nodiscard]] InsertStep insert_into(
        HeldLeaf& held, bool at_separator, Key& key, Value& value, Policy& policy,
        std::optional<typename Policy::Output>& result, std::size_t* edge) {
        const Leaf leaf = held.leaf;
        if (at_separator) {
            result.emplace(policy.occupied(static_cast<const Key&>(leaf.head_key()), leaf.head_value(), std::move(key), std::move(value)));
            return InsertStep::done;
        }
        const Query query = Traits::query_of(key);
        const typename Leaf::SearchResult search = leaf.search(query);
        if (search.found) {
            result.emplace(policy.occupied(
                static_cast<const Key&>(leaf.key(search.index)), leaf.value(search.index), std::move(key), std::move(value)));
            return InsertStep::done;
        }
        if (leaf.length() >= Leaf::capacity) {
            *edge = search.index;
            return InsertStep::full;
        }
        result.emplace(policy.vacant(static_cast<const Key&>(key), static_cast<const Value&>(value)));
        if (is_root(leaf)) {
            // Holding the root leaf holds both borders.
            Algorithms::shrink_shared_prefix_for(load_root(), query);
        }
        const InlineWord word = Shape::window_word(query, leaf.prefix_length());
        leaf.insert_fit(search.index, std::move(key), std::move(value), word);
        return InsertStep::done;
    }

    // A full leaf: descend again keeping the path, then split.
    template <typename Policy>
    STX_BTREE_NEVER_INLINE typename Policy::Output insert_splitting(Key&& key, Value&& value, Policy& policy) {
        std::optional<typename Policy::Output> result;
        Backoff backoff;
        while (true) {
            const Query query = Traits::query_of(key);
            TreePath path;
            HeldLeaf held;
            bool at_separator = false;
            const DescentOutcome outcome = descend<LatchMode::exclusive>(query, path, held, &at_separator);
            if (outcome != DescentOutcome::inside) {
                held.release();
                shrink_shared_prefix_for(query);
                continue;
            }
            std::size_t edge = 0;
            const InsertStep step = insert_into(held, at_separator, key, value, policy, result, &edge);
            if (step == InsertStep::done) {
                return std::move(*result);
            }
            if (split(path, held, edge, key, value, policy, result)) {
                return std::move(*result);
            }
            held.release();
            backoff.spin();
        }
    }

    // Takes the path from the fences' top down to the lowest node with room,
    // reserves the split's pages, and runs the unsync split along it; false
    // to start over (bbtree split).
    template <typename Policy>
    [[nodiscard]] bool split(
        const TreePath& path, HeldLeaf& held, std::size_t edge, Key& key, Value& value, Policy& policy,
        std::optional<typename Policy::Output>& result) {
        const std::size_t depth = path.depth;
        bool has_room = false;
        std::size_t room = 0;
        std::size_t level = depth;
        while (level > 0) {
            level -= 1;
            if (path.levels[level].routed.length < Internal::capacity) {
                has_room = true;
                room = level;
                break;
            }
        }
        const std::size_t top = path.fences_top(has_room ? room : 0);
        Operation operation(*this);
        if (!operation.held.take_path(path, top)) {
            return false;
        }
        const std::size_t splits = depth - (has_room ? room + 1 : 0);
        std::size_t leaves = 1;
        std::size_t internals = splits;
        if (!has_room) {
            if (depth == 0) {
                leaves = 2;
                internals = 0;
            } else {
                internals = splits + 1;
            }
        }
        operation.reserve_pages(leaves, internals);
        const Leaf leaf = held.leaf;
        result.emplace(policy.vacant(static_cast<const Key&>(key), static_cast<const Value&>(value)));
        const bool was_last_leaf = rightmost_leaf_word.load(std::memory_order_relaxed) == leaf.pointer().tagged;
        held.hand_over(operation.held);
        const Query query = Traits::query_of(key);
        if (is_root(leaf)) {
            Algorithms::shrink_shared_prefix_for(load_root(), query);
        }
        Stack stack;
        build_stack_to_leaf(stack, path, top, leaf, edge);
        Algorithms::insert_splitting(operation, stack, std::move(key), std::move(value), operation.reserve);
        if (was_last_leaf) {
            const NodePointer took_separator =
                has_room ? Internal{path.levels[room].node}.pointer() : operation.root();
            point_appends_to(border_leaf(took_separator, false));
        }
        return true;
    }

    static void build_stack_to_leaf(Stack& stack, const TreePath& path, std::size_t top, Leaf leaf, std::size_t index) noexcept {
        if (top >= path.depth) {
            stack.start_at(leaf.pointer(), path.depth == 0);
            stack.set_index(index);
            return;
        }
        stack.start_at(Internal{path.levels[top].node}.pointer(), top == 0);
        stack.set_index(path.levels[top].routed.edge);
        std::size_t level = 0;
        for (level = top + 1; level < path.depth; level += 1) {
            stack.push(Internal{path.levels[level].node}.pointer(), path.levels[level].routed.edge);
        }
        stack.push(leaf.pointer(), index);
    }

    static void build_stack_to_separator(Stack& stack, const TreePath& path, std::size_t top, std::size_t level, std::size_t index) noexcept {
        stack.start_at(Internal{path.levels[top].node}.pointer(), top == 0);
        stack.set_index(top == level ? index : path.levels[top].routed.edge);
        std::size_t above = 0;
        for (above = top + 1; above <= level; above += 1) {
            const std::size_t taken = above == level ? index : path.levels[above].routed.edge;
            stack.push(Internal{path.levels[above].node}.pointer(), taken);
        }
    }

    [[nodiscard]] static Leaf border_leaf(NodePointer top, bool first_edges) noexcept {
        NodePointer node = top;
        while (!node.is_leaf()) {
            const Internal internal = Internal::at(node);
            node = internal.edge(first_edges ? 0 : internal.length());
        }
        return Leaf::at(node);
    }

    void point_appends_to(Leaf leaf) noexcept {
        rightmost_leaf_word.store(leaf.pointer().tagged, std::memory_order_release);
    }

    // A key that lacks the shared prefix: take the root and every node along
    // both borders, then shrink it (bbtree latched/shared_prefix.rs).
    STX_BTREE_NEVER_INLINE void shrink_shared_prefix_for(const Query& query) noexcept {
        Backoff backoff;
        while (true) {
            HeldNodes held;
            if (take_borders(held)) {
                Algorithms::shrink_shared_prefix_for(load_root(), query);
                return;
            }
            held.release_all();
            backoff.spin();
        }
    }

    [[nodiscard]] bool take_borders(HeldNodes& held) noexcept {
        const NodePointer root = load_root();
        if (!take_node(root, held)) {
            return false;
        }
        if (!(load_root() == root)) {
            return false;
        }
        return take_border(root, held, true) && take_border(root, held, false);
    }

    [[nodiscard]] static bool take_node(NodePointer node, HeldNodes& held) noexcept {
        if (node.is_leaf()) {
            return held.take_leaf(node.page());
        }
        return held.take_internal(node.page());
    }

    [[nodiscard]] static bool take_border(NodePointer top, HeldNodes& held, bool first_edges) noexcept {
        NodePointer node = top;
        while (!node.is_leaf()) {
            const Internal internal = Internal::at(node);
            node = internal.edge(first_edges ? 0 : internal.length());
            if (!take_node(node, held)) {
                return false;
            }
        }
        return true;
    }

    // --- Removal (bbtree latched/remove.rs) ---------------------------------

    // Removes the query's entry if condition(key, value) holds, into
    // `removed`.
    template <typename Condition>
    bool remove_if(const Query& query, Condition& condition, EntryBuffer<Key, Value>& removed) {
        if (load_root().is_null()) {
            return false;
        }
        {
            HeldLeaf held;
            bool at_separator = false;
            NoPath no_path;
            const DescentOutcome outcome = descend<LatchMode::exclusive>(query, no_path, held, &at_separator);
            if (outcome != DescentOutcome::inside) {
                return false;
            }
            const Leaf leaf = held.leaf;
            if (at_separator) {
                if (!condition(static_cast<const Key&>(leaf.head_key()), static_cast<const Value&>(leaf.head_value()))) {
                    return false;
                }
            } else {
                const std::size_t index = leaf.find(query);
                if (index == Leaf::absent) {
                    return false;
                }
                if (!condition(static_cast<const Key&>(leaf.key(index)), static_cast<const Value&>(leaf.value(index)))) {
                    return false;
                }
                if (leaf.keeps_its_minimum() || is_root(leaf)) {
                    leaf.remove(index, removed.key_storage(), removed.value_storage());
                    removed.mark_filled();
                    return true;
                }
            }
        }
        return remove_rebalancing(query, condition, removed);
    }

    template <typename Condition>
    STX_BTREE_NEVER_INLINE bool remove_rebalancing(const Query& query, Condition& condition, EntryBuffer<Key, Value>& removed) {
        Backoff backoff;
        while (true) {
            bool found = false;
            if (try_remove_rebalancing(query, condition, removed, &found)) {
                return found;
            }
            backoff.spin();
        }
    }

    // One attempt; false to start over.
    template <typename Condition>
    [[nodiscard]] bool try_remove_rebalancing(const Query& query, Condition& condition, EntryBuffer<Key, Value>& removed, bool* found) {
        TreePath path;
        HeldLeaf held;
        bool at_separator = false;
        const DescentOutcome outcome = descend<LatchMode::exclusive>(query, path, held, &at_separator);
        if (outcome != DescentOutcome::inside) {
            *found = false;
            return true;
        }
        const Leaf leaf = held.leaf;
        const std::size_t depth = path.depth;
        bool has_separator = false;
        std::size_t separator_level = 0;
        std::size_t leaf_index = 0;
        if (at_separator) {
            if (!condition(static_cast<const Key&>(leaf.head_key()), static_cast<const Value&>(leaf.head_value()))) {
                *found = false;
                return true;
            }
            std::size_t level = 0;
            for (level = 0; level < depth; level += 1) {
                if (path.levels[level].routed.at_separator) {
                    break;
                }
            }
            has_separator = true;
            separator_level = level;
        } else {
            const std::size_t index = leaf.find(query);
            if (index == Leaf::absent) {
                *found = false;
                return true;
            }
            if (!condition(static_cast<const Key&>(leaf.key(index)), static_cast<const Value&>(leaf.value(index)))) {
                *found = false;
                return true;
            }
            if (leaf.keeps_its_minimum() || is_root(leaf)) {
                leaf.remove(index, removed.key_storage(), removed.value_storage());
                removed.mark_filled();
                *found = true;
                return true;
            }
            leaf_index = index;
        }
        Operation operation(*this);
        std::size_t fixes = 0;
        if (!take_fixes(path, leaf.length() - 1, operation.held, &fixes)) {
            return false;
        }
        std::size_t top_changed = depth - fixes;
        if (has_separator) {
            const PathLevel& level = path.levels[separator_level];
            if (!operation.held.take_path(path, separator_level) ||
                !take_left_border(Internal{level.node}, level.routed.edge - 1, operation.held)) {
                return false;
            }
            top_changed = separator_level < top_changed ? separator_level : top_changed;
        }
        const std::size_t top = path.fences_top(top_changed);
        if (!operation.held.take_path(path, top)) {
            return false;
        }
        held.hand_over(operation.held);
        Stack stack;
        if (has_separator) {
            build_stack_to_separator(stack, path, top, separator_level, path.levels[separator_level].routed.edge - 1);
        } else {
            build_stack_to_leaf(stack, path, top, leaf, leaf_index);
        }
        Algorithms::remove(operation, stack, removed);
        const NodePointer last = NodePointer{rightmost_leaf_word.load(std::memory_order_relaxed)};
        if (!last.is_null() && operation.has_freed(last.page())) {
            const std::size_t fixed_top = depth - fixes;
            const NodePointer top_fixed = fixed_top == 0 ? operation.root() : Internal{path.levels[fixed_top].node}.pointer();
            point_appends_to(border_leaf(top_fixed, false));
        }
        *found = true;
        return true;
    }

    // For each level a removal leaves underfull, from the leaf up, the parent
    // and the sibling it merges with or takes from, as fix_underfull_child
    // decides (bbtree take_fixes).
    [[nodiscard]] static bool take_fixes(const TreePath& path, std::size_t leaf_length, HeldNodes& held, std::size_t* fixes_out) noexcept {
        std::size_t fixes = 0;
        std::size_t child_length = leaf_length;
        while (true) {
            const std::size_t level = path.depth - fixes;
            const std::size_t minimum = fixes == 0 ? Leaf::minimum : Internal::minimum;
            if (child_length >= minimum || level == 0) {
                *fixes_out = fixes;
                return true;
            }
            const PathLevel& parent_level = path.levels[level - 1];
            if (!held.take_internal_at(parent_level.node, parent_level.version)) {
                return false;
            }
            const Internal parent = Internal{parent_level.node};
            const std::size_t sibling_edge = parent_level.routed.edge > 0 ? parent_level.routed.edge - 1 : parent_level.routed.edge + 1;
            const NodePointer sibling = parent.edge(sibling_edge);
            bool merges = false;
            if (fixes == 0) {
                if (!held.take_leaf(sibling.page())) {
                    return false;
                }
                merges = child_length + 1 + Leaf::at(sibling).length() <= Leaf::capacity;
            } else {
                if (!held.take_internal(sibling.page())) {
                    return false;
                }
                merges = child_length + 1 + Internal::at(sibling).length() <= Internal::capacity;
            }
            fixes += 1;
            if (!merges) {
                *fixes_out = fixes;
                return true;
            }
            child_length = parent.length() - 1;
        }
    }

    // The right border of the subtree left of a separator, whose upper fence
    // its successor becomes.
    [[nodiscard]] static bool take_left_border(Internal node, std::size_t edge, HeldNodes& held) noexcept {
        NodePointer child = node.edge(edge);
        while (!child.is_leaf()) {
            if (!held.take_internal(child.page())) {
                return false;
            }
            const Internal internal = Internal::at(child);
            child = internal.edge(internal.length());
        }
        return held.take_leaf(child.page());
    }

    // --- Scans (bbtree latched/scan.rs) -------------------------------------
    //
    // visitor(key, value) runs on each entry from the start bound on, in key
    // order (or reverse), with its leaf latched shared, until it returns
    // false or the end bound is passed. A leaf's neighbor is latched before
    // the leaf is let go of; a scan is not a snapshot, but an entry present
    // throughout is visited exactly once.

    struct Bound final {
        const Query* key = nullptr;
        bool inclusive = true;
    };

    template <bool Ascending, typename Visitor>
    void scan(Bound start, Bound end, Visitor& visitor) const {
        if (load_root().is_null()) {
            return;
        }
        TreePath path;
        HeldLeaf held;
        std::size_t cursor = 0;
        if (!scan_start<Ascending>(start, path, held, &cursor)) {
            return;
        }
        bool whole = false;
        while (true) {
            const ScanVisit visit = visit_leaf<Ascending>(held.leaf, cursor, whole, end, visitor);
            if (visit != ScanVisit::exhausted) {
                return;
            }
            const NextLeaf next = next_leaf<Ascending>(path, held);
            if (next == NextLeaf::none) {
                return;
            }
            if (next == NextLeaf::latched) {
                cursor = Ascending ? 0 : entry_count(held.leaf);
                whole = true;
                continue;
            }
            // Start over from the last key visited, still holding its leaf.
            const Leaf leaf = held.leaf;
            const Key* last_key = last_visited<Ascending>(leaf);
            if (last_key == nullptr) {
                return;
            }
            const Query last_query = Traits::query_of(*last_key);
            HeldLeaf again;
            bool at_head = false;
            const DescentOutcome outcome = descend<LatchMode::shared>(last_query, path, again, &at_head);
            if (outcome != DescentOutcome::inside) {
                return;
            }
            cursor = cursor_at<Ascending>(again.leaf, last_query, at_head, false);
            held.release();
            move_hold(again, held);
            whole = false;
        }
    }

    enum class ScanVisit : std::uint8_t { stopped, past_end, exhausted };
    enum class NextLeaf : std::uint8_t { none, latched, start_over };

    static void move_hold(HeldLeaf& from, HeldLeaf& to) noexcept {
        to.leaf = from.leaf;
        to.version = from.version;
        to.mode = from.mode;
        to.holding = from.holding;
        from.holding = false;
    }

    [[nodiscard]] static std::size_t entry_count(Leaf leaf) noexcept {
        return (leaf.has_head() ? 1 : 0) + leaf.length();
    }

    // Where the scan starts within a leaf, in entry positions (head first):
    // ascending, the first position to visit; descending, one past it.
    template <bool Ascending>
    [[nodiscard]] static std::size_t cursor_at(Leaf leaf, const Query& query, bool at_head, bool inclusive) noexcept {
        if (at_head) {
            if (Ascending) {
                return inclusive ? 0 : 1;
            }
            return inclusive ? 1 : 0;
        }
        const std::size_t before = leaf.has_head() ? 1 : 0;
        const typename Leaf::SearchResult result = leaf.search(query);
        const std::size_t position = before + result.index;
        if (Ascending) {
            return position + ((result.found && !inclusive) ? 1 : 0);
        }
        return position + ((result.found && inclusive) ? 1 : 0);
    }

    template <bool Ascending>
    [[nodiscard]] bool scan_start(Bound start, TreePath& path, HeldLeaf& held, std::size_t* cursor) const noexcept {
        if (start.key == nullptr) {
            descend_to_end_leaf<Ascending>(path, held);
            *cursor = Ascending ? 0 : entry_count(held.leaf);
            return true;
        }
        while (true) {
            bool at_head = false;
            const DescentOutcome outcome = descend<LatchMode::shared>(*start.key, path, held, &at_head);
            if (outcome == DescentOutcome::inside) {
                *cursor = cursor_at<Ascending>(held.leaf, *start.key, at_head, start.inclusive);
                return true;
            }
            const DescentOutcome before_start = Ascending ? DescentOutcome::before_every_key : DescentOutcome::after_every_key;
            if (outcome != before_start) {
                return false;
            }
            // The bound sorts before every key: start at the end leaf, then
            // check the shared prefix again under its latch, which on the
            // border holds it (an insert may have shrunk it meanwhile).
            descend_to_end_leaf<Ascending>(path, held);
            if (place_against_root(load_root(), *start.key) == outcome) {
                *cursor = Ascending ? 0 : entry_count(held.leaf);
                return true;
            }
        }
    }

    template <bool Ascending>
    void descend_to_end_leaf(TreePath& path, HeldLeaf& held) const noexcept {
        while (true) {
            path.clear();
            held.release();
            const NodePointer root = load_root();
            if (root.is_leaf()) {
                held.lock(Leaf::at(root), LatchMode::shared);
                if (is_root(Leaf::at(root))) {
                    return;
                }
                continue;
            }
            const Internal node = Internal::at(root);
            const Version version = NodeLatch::of(node.page).read_version();
            if (!(load_root() == root)) {
                continue;
            }
            if (walk_down<Ascending>(node, version, false, 0, path, held)) {
                return;
            }
        }
    }

    // Down from `node` at `version` along end edges (or first along
    // `first_edge`), latching the leaf shared; false if a check failed.
    template <bool Ascending>
    [[nodiscard]] static bool walk_down(Internal start_node, Version start_version, bool has_first_edge, std::size_t first_edge,
                                        TreePath& path, HeldLeaf& held) noexcept {
        Internal node = start_node;
        Version version = start_version;
        bool use_first = has_first_edge;
        while (true) {
            const std::size_t racing_length = node.length();
            const std::size_t length = racing_length < Internal::capacity ? racing_length : Internal::capacity;
            const std::size_t edge = use_first ? first_edge : (Ascending ? 0 : length);
            use_first = false;
            const NodePointer child = node.edge(edge);
            if (child.is_null() || !NodeLatch::of(node.page).is_unchanged(version)) {
                return false;
            }
            path.record(node.page, version, Routed{.edge = edge, .length = length, .at_separator = false});
            if (child.is_leaf()) {
                held.lock(Leaf::at(child), LatchMode::shared);
                if (!NodeLatch::of(node.page).is_unchanged(version)) {
                    held.release();
                    return false;
                }
                return true;
            }
            const Internal child_node = Internal::at(child);
            const Version child_version = NodeLatch::of(child_node.page).read_version();
            if (!NodeLatch::of(node.page).is_unchanged(version)) {
                return false;
            }
            node = child_node;
            version = child_version;
        }
    }

    // The leaf beside the held one, latched before the held one is let go
    // of, if every node kept from the turning point down is unchanged.
    template <bool Ascending>
    [[nodiscard]] NextLeaf next_leaf(TreePath& path, HeldLeaf& held) const noexcept {
        const std::size_t depth = path.depth;
        std::size_t level = depth;
        bool turning = false;
        std::size_t edge = 0;
        while (level > 0) {
            level -= 1;
            const Routed routed = path.levels[level].routed;
            if (Ascending && routed.edge < routed.length) {
                turning = true;
                edge = routed.edge + 1;
                break;
            }
            if (!Ascending && routed.edge > 0) {
                turning = true;
                edge = routed.edge - 1;
                break;
            }
        }
        if (!turning) {
            return NextLeaf::none;
        }
        // The kept levels below the turning point, checked once the next leaf
        // is latched: then the held leaf is still the next one's neighbor.
        // Only the first below_count are written.
        PathLevel below[maximum_depth];
        std::size_t below_count = 0;
        std::size_t kept = 0;
        for (kept = level + 1; kept < depth; kept += 1) {
            below[below_count] = path.levels[kept];
            below_count += 1;
        }
        const PathLevel turn = path.levels[level];
        path.depth = level;
        HeldLeaf next;
        if (!walk_down<Ascending>(Internal{turn.node}, turn.version, true, edge, path, next)) {
            return NextLeaf::start_over;
        }
        for (kept = 0; kept < below_count; kept += 1) {
            if (!NodeLatch::of(below[kept].node).is_unchanged(below[kept].version)) {
                return NextLeaf::start_over;
            }
        }
        held.release();
        move_hold(next, held);
        return NextLeaf::latched;
    }

    template <bool Ascending>
    [[nodiscard]] static const Key* last_visited(Leaf leaf) noexcept {
        if (Ascending) {
            if (leaf.length() > 0) {
                return &leaf.key(leaf.length() - 1);
            }
            return leaf.has_head() ? &leaf.head_key() : nullptr;
        }
        if (leaf.has_head()) {
            return &leaf.head_key();
        }
        return leaf.length() > 0 ? &leaf.key(0) : nullptr;
    }

    // `order` compares the key with the bound.
    template <bool Ascending>
    [[nodiscard]] static bool past_end_order(int order, bool inclusive) noexcept {
        if (Ascending) {
            return inclusive ? order > 0 : order >= 0;
        }
        return inclusive ? order < 0 : order <= 0;
    }

    // Visits a leaf's entries from `cursor` (entry positions, head first).
    // The keys' addresses come from their indexes alone, so no entry load
    // waits on the header's head flag (bbtree ScanOrder::whole_entries); the
    // end bound is decided once per leaf.
    template <bool Ascending, typename Visitor>
    [[nodiscard]] static ScanVisit visit_leaf(Leaf leaf, std::size_t cursor, bool whole, Bound end, Visitor& visitor) {
        if (end.key == nullptr) {
            return visit_entries<Ascending, false>(leaf, cursor, whole, end, visitor);
        }
        return visit_entries<Ascending, true>(leaf, cursor, whole, end, visitor);
    }

    template <bool Ascending, bool Bounded, typename Visitor>
    [[nodiscard]] static ScanVisit visit_entries(Leaf leaf, std::size_t cursor, bool whole, Bound end, Visitor& visitor) {
        const std::size_t length = leaf.length();
        const bool has_head = leaf.has_head();
        if (Ascending) {
            // A whole leaf starts at its head, then key 0.
            const bool visits_head = has_head && (whole || cursor == 0);
            std::size_t first_key = 0;
            if (!whole && cursor > 0) {
                first_key = cursor - (has_head ? 1 : 0);
            }
            if (visits_head) {
                const ScanVisit visit = visit_entry<Ascending, Bounded>(leaf.head_key(), leaf.head_value(), end, visitor);
                if (visit != ScanVisit::exhausted) {
                    return visit;
                }
            }
            std::size_t index = 0;
            for (index = first_key; index < length; index += 1) {
                const ScanVisit visit = visit_entry<Ascending, Bounded>(leaf.key(index), leaf.value(index), end, visitor);
                if (visit != ScanVisit::exhausted) {
                    return visit;
                }
            }
            return ScanVisit::exhausted;
        }
        // Descending: keys from the end (or from the cursor) down, then the
        // head.
        std::size_t key_end = length;
        bool visits_head = has_head;
        if (!whole) {
            if (has_head) {
                visits_head = cursor > 0;
                key_end = cursor > 0 ? cursor - 1 : 0;
            } else {
                key_end = cursor;
            }
        }
        std::size_t index = key_end;
        while (index > 0) {
            index -= 1;
            const ScanVisit visit = visit_entry<Ascending, Bounded>(leaf.key(index), leaf.value(index), end, visitor);
            if (visit != ScanVisit::exhausted) {
                return visit;
            }
        }
        if (visits_head) {
            return visit_entry<Ascending, Bounded>(leaf.head_key(), leaf.head_value(), end, visitor);
        }
        return ScanVisit::exhausted;
    }

    template <bool Ascending, bool Bounded, typename Visitor>
    STX_BTREE_ALWAYS_INLINE static ScanVisit visit_entry(const Key& key, const Value& value, Bound end, Visitor& visitor) {
        if constexpr (Bounded) {
            const int order = compare_to_bound(key, end);
            if (past_end_order<Ascending>(order, end.inclusive)) {
                return ScanVisit::past_end;
            }
        }
        if (!visitor(key, value)) {
            return ScanVisit::stopped;
        }
        return ScanVisit::exhausted;
    }

    // The order of a stored key against a bound's query.
    [[nodiscard]] static int compare_to_bound(const Key& key, Bound bound) noexcept {
        const int query_order = Traits::compare_from(*bound.key, key, 0);
        return -query_order;
    }

    // --- Owner (exclusive access, no other thread) ---------------------------

    [[nodiscard]] bool is_empty() const noexcept {
        const NodePointer root = load_root();
        return Walk::is_empty(root);
    }

    // Destroys every entry and returns every page, the free ones included.
    void destroy() noexcept {
        const NodePointer root = load_root();
        if (!root.is_null()) {
            release_subtree(root);
            root_word.store(0, std::memory_order_relaxed);
            rightmost_leaf_word.store(0, std::memory_order_relaxed);
        }
        std::byte* page = free_leaves.pop();
        while (page != nullptr) {
            deallocate_page(PageKind::leaf, page);
            page = free_leaves.pop();
        }
        page = free_internals.pop();
        while (page != nullptr) {
            deallocate_page(PageKind::internal, page);
            page = free_internals.pop();
        }
    }

    void release_subtree(NodePointer top) noexcept {
        Stack stack;
        stack.start_at(top, true);
        while (true) {
            if (stack.ends_at_leaf()) {
                const Leaf leaf = stack.leaf();
                leaf.destroy_entries();
                if (stack.is_at_top()) {
                    deallocate_page(PageKind::root, leaf.page);
                    return;
                }
                deallocate_page(PageKind::leaf, leaf.page);
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
                deallocate_page(PageKind::root, node.page);
                return;
            }
            deallocate_page(PageKind::internal, node.page);
            stack.ascend();
            stack.set_index(stack.index() + 1);
        }
    }

    std::atomic<std::uintptr_t> root_word{0};

private:
    alignas(128) std::atomic<std::uintptr_t> rightmost_leaf_word{0};
    alignas(128) FreeList free_leaves;
    FreeList free_internals;
    [[no_unique_address]] Allocator page_allocator;
};

} // namespace stx_btree::detail
