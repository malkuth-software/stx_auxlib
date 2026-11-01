#pragma once

// A static ordered map laid out for a known search distribution: an optimal
// (or weight-balanced) binary search tree over the keys (optimal_tree.hpp),
// packed into multiway nodes so that the weighted number of nodes visited is
// least for that tree (detail/tree_packing.hpp), each node searched the way
// bbtree searches its nodes.
//
// What it takes from bbtree:
//   - Inline keys (inline_key.hpp): each node keeps fixed-width windows of
//     its keys' order-preserving encodings, taken after the bytes that every
//     key and every query reaching the node share. Those shared bytes are
//     the common prefix of the node's fences (the parent keys around the
//     exit that leads to it), so the windows are short (2 bytes for
//     integers) and many fit a line.
//   - The count search (detail/vector_count.hpp): count windows below and
//     equal to the query's over every slot in 16-byte lane groups, slots past
//     the length all ones; one candidate is compared inline, several go to an
//     outlined tie search.
//   - Batch lanes (find_many): one state machine per query that prefetches
//     the line it reads next and yields.
// What it takes from the optimal tree work: where each key sits. Heavy keys
// sit in the root node, which stays in cache; the packing decides which keys
// share a node so that the expected number of nodes per search is least for
// the chosen tree.
//
// A node is 128 bytes, one cache line on Apple M-series cores (two on
// 64-byte-line machines): 96 bytes of windows (48 two-byte, 24 four-byte or
// 12 eight-byte), an exit mask, and offsets to its entries and first child.
// Nodes are in breadth-first order; a node's children are consecutive.
//
// The map is immutable after build. find returns a pointer into the map,
// valid until the map is destroyed or assigned; moving the map keeps it.

#include "stx_btree/detail/inlining.hpp"
#include "stx_btree/detail/prefetch.hpp"
#include "stx_btree/detail/tree_packing.hpp"
#include "stx_btree/detail/vector_count.hpp"
#include "stx_btree/inline_key.hpp"
#include "stx_btree/optimal_tree.hpp"
#include "stx_btree/search_weights.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace stx_btree {

namespace detail {

template <InlineWidth Width>
struct InlineSlot;

template <>
struct InlineSlot<InlineWidth::two> final {
    using Type = std::uint16_t;
};

template <>
struct InlineSlot<InlineWidth::four> final {
    using Type = std::uint32_t;
};

template <>
struct InlineSlot<InlineWidth::eight> final {
    using Type = std::uint64_t;
};

template <typename Slot>
struct alignas(128) OptimalMapNode final {
    static constexpr std::size_t window_bytes = 96;
    static constexpr std::size_t capacity = window_bytes / sizeof(Slot);

    Slot slots[capacity];       // windows of the live keys ascending, then all ones
    std::uint64_t child_exits;  // bit e set when exit e leads to a child node
    std::uint32_t first_entry;  // the node's entries are entries[first_entry, first_entry + length)
    std::uint32_t first_child;  // exit e leads to first_child + popcount(child_exits below bit e)
    std::uint16_t length;
    std::uint16_t prefix;       // bytes shared by every key and query below this node's fences
};

} // namespace detail

// How the map is laid out. `tree` builds the binary tree that is packed into
// nodes. A subtree is laid out instead as full count-balanced nodes (a
// B-tree's bulk load) when that costs at most count_layout_slack_percent
// more weighted node visits than the packing there
// (detail::ColdSubtreeLayout): flat regions get full nodes, skewed ones keep
// the packing.
struct OptimalMapPlan final {
    TreeConstruction tree = TreeConstruction::knuth;
    std::uint32_t count_layout_slack_percent = 2;
};

template <typename Key, typename Value>
class OptimalMap final {
public:
    using Traits = InlineKeyTraits<Key>;
    using Query = typename Traits::Query;
    using Slot = typename detail::InlineSlot<Traits::internal_width>::Type;
    using Node = detail::OptimalMapNode<Slot>;

    static constexpr std::size_t node_capacity =
        Node::capacity < detail::TreePacker::maximum_capacity ? Node::capacity : detail::TreePacker::maximum_capacity;

    struct Entry final {
        Key key;
        Value value;
    };

    // Shape and expected work, for measurement.
    struct Statistics final {
        std::size_t entries = 0;
        std::size_t nodes = 0;
        std::size_t height = 0;          // nodes on the longest search path
        ExactCost tree_cost = 0;         // weighted comparisons of the binary tree
        ExactCost packed_visits = 0;     // weighted node visits of the packing alone
        ExactCost expected_visits = 0;   // weighted node visits of the layout built
    };

    OptimalMap() = default;
    OptimalMap(const OptimalMap&) = delete;
    OptimalMap& operator=(const OptimalMap&) = delete;
    OptimalMap(OptimalMap&&) noexcept = default;
    OptimalMap& operator=(OptimalMap&&) noexcept = default;
    ~OptimalMap() = default;

    // Builds the map over `keys` (strictly ascending in the traits' order)
    // with their `values`, for searches distributed as `weights` (one weight
    // per key found, one per gap searched in vain). Throws
    // std::invalid_argument on mismatched lengths or unsorted keys, and
    // whatever the chosen tree construction throws.
    [[nodiscard]] static OptimalMap build(
        std::vector<Key> keys, std::vector<Value> values, const SearchWeights& weights,
        const OptimalMapPlan& plan) {
        SearchWeightRules::validate(weights);
        if (keys.size() != values.size() || keys.size() != weights.keys.size()) {
            throw std::invalid_argument("keys, values and key weights differ in length");
        }
        if (keys.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
            throw std::invalid_argument("too many keys for the static map");
        }
        std::size_t index = 0;
        for (index = 1; index < keys.size(); index += 1) {
            const int order = Traits::compare_from(Traits::query_of(keys[index - 1]), keys[index], 0);
            if (order >= 0) {
                throw std::invalid_argument("keys are not strictly ascending");
            }
        }
        const SearchTree tree = OptimalTreeBuilder::build(weights, plan.tree);
        const detail::PackedTree packed = detail::TreePacker::pack(tree.shape, weights, node_capacity);
        const detail::PackedTree laid =
            detail::ColdSubtreeLayout::relayout(packed, weights, node_capacity, plan.count_layout_slack_percent);
        OptimalMap map;
        map.statistics.entries = keys.size();
        map.statistics.tree_cost = tree.cost;
        map.statistics.packed_visits = packed.cost;
        map.statistics.expected_visits = laid.cost;
        map.lay_out(laid, keys, values);
        return map;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return entries.size();
    }

    [[nodiscard]] const Statistics& stats() const noexcept {
        return statistics;
    }

    // The value of `query`, or null.
    [[nodiscard]] STX_BTREE_ALWAYS_INLINE const Value* find(const Query& query) const noexcept {
        if (nodes.empty()) {
            return nullptr;
        }
        std::uint32_t node_index = 0;
        while (true) {
            const Node& node = nodes[node_index];
            const NodeStep step = search_node(node, query);
            if (step.found) {
                return &entries[node.first_entry + step.position].value;
            }
            if (((node.child_exits >> step.position) & 1) == 0) {
                return nullptr;
            }
            node_index = child_of(node, step.position);
        }
    }

    // results[i] = find(queries[i]); the spans have equal lengths. Up to
    // lane_count queries descend together, each prefetching its next node or
    // entry before it yields.
    void find_many(std::span<const Query> queries, std::span<const Value*> results) const noexcept {
        if (nodes.empty()) {
            std::size_t index = 0;
            for (index = 0; index < results.size(); index += 1) {
                results[index] = nullptr;
            }
            return;
        }
        Lane lanes[lane_count];
        std::size_t next_query = 0;
        std::size_t live = 0;
        std::size_t lane_index = 0;
        for (lane_index = 0; lane_index < lane_count; lane_index += 1) {
            lanes[lane_index] = Lane{};
            if (next_query < queries.size()) {
                lanes[lane_index].query = next_query;
                lanes[lane_index].state = LaneState::node;
                next_query += 1;
                live += 1;
            }
        }
        while (live > 0) {
            for (lane_index = 0; lane_index < lane_count; lane_index += 1) {
                Lane& lane = lanes[lane_index];
                if (lane.state == LaneState::idle) {
                    continue;
                }
                const bool finished = advance(lane, queries[lane.query], results);
                if (!finished) {
                    continue;
                }
                if (next_query < queries.size()) {
                    lane = Lane{};
                    lane.query = next_query;
                    lane.state = LaneState::node;
                    next_query += 1;
                } else {
                    lane.state = LaneState::idle;
                    live -= 1;
                }
            }
        }
    }

    // The entries in node order (not key order), for inspection.
    [[nodiscard]] std::span<const Entry> node_order_entries() const noexcept {
        return std::span<const Entry>(entries.data(), entries.size());
    }

private:
    static constexpr std::size_t lane_count = 16;
    static constexpr unsigned slot_shift = static_cast<unsigned>(64 - 8 * sizeof(Slot));
    static constexpr Slot padding_slot = std::numeric_limits<Slot>::max();
    using Count = detail::InlineKeyCount<Slot, Node::capacity>;

    // The outcome of one node: the key's position among the node's entries,
    // or the exit to take.
    struct NodeStep final {
        bool found;
        std::size_t position;
    };

    enum class LaneState : std::uint8_t { idle, node, candidate };

    struct Lane final {
        std::size_t query = 0;
        std::uint32_t node = 0;
        std::uint32_t candidate = 0; // the one entry whose window equals the query's
        LaneState state = LaneState::idle;
    };

    [[nodiscard]] STX_BTREE_ALWAYS_INLINE static Slot window_slot(const Query& query, std::size_t prefix) noexcept {
        const InlineWord word = Traits::window(query, prefix);
        return static_cast<Slot>(word >> slot_shift);
    }

    [[nodiscard]] STX_BTREE_ALWAYS_INLINE std::uint32_t child_of(const Node& node, std::size_t exit) const noexcept {
        const std::uint64_t before = node.child_exits & ((static_cast<std::uint64_t>(1) << exit) - 1);
        const int rank = std::popcount(before);
        return node.first_child + static_cast<std::uint32_t>(rank);
    }

    [[nodiscard]] STX_BTREE_ALWAYS_INLINE NodeStep search_node(const Node& node, const Query& query) const noexcept {
        const Slot target = window_slot(query, node.prefix);
        const detail::EqualRange equal = Count::equal_range(node.slots, target, node.length);
        if (equal.end == equal.begin) {
            return NodeStep{.found = false, .position = equal.begin};
        }
        if (equal.end - equal.begin == 1) {
            return compare_candidate(node, query, equal.begin);
        }
        return search_ties(node, query, equal);
    }

    [[nodiscard]] STX_BTREE_ALWAYS_INLINE NodeStep compare_candidate(
        const Node& node, const Query& query, std::size_t position) const noexcept {
        const Key& stored = entries[node.first_entry + position].key;
        const int order = Traits::compare_from(query, stored, node.prefix);
        if (order == 0) {
            return NodeStep{.found = true, .position = position};
        }
        const std::size_t exit = order < 0 ? position : position + 1;
        return NodeStep{.found = false, .position = exit};
    }

    // Several keys share the query's window: the first not below the query
    // decides.
    [[nodiscard]] STX_BTREE_NEVER_INLINE NodeStep search_ties(
        const Node& node, const Query& query, detail::EqualRange equal) const noexcept {
        std::size_t position = 0;
        for (position = equal.begin; position < equal.end; position += 1) {
            const Key& stored = entries[node.first_entry + position].key;
            const int order = Traits::compare_from(query, stored, node.prefix);
            if (order == 0) {
                return NodeStep{.found = true, .position = position};
            }
            if (order < 0) {
                return NodeStep{.found = false, .position = position};
            }
        }
        return NodeStep{.found = false, .position = equal.end};
    }

    // One stage of a lane. Returns whether its query is answered.
    bool advance(Lane& lane, const Query& query, std::span<const Value*> results) const noexcept {
        const Node& node = nodes[lane.node];
        std::size_t exit = 0;
        if (lane.state == LaneState::candidate) {
            const NodeStep step = compare_candidate(node, query, lane.candidate);
            if (step.found) {
                results[lane.query] = &entries[node.first_entry + step.position].value;
                return true;
            }
            exit = step.position;
        } else {
            const Slot target = window_slot(query, node.prefix);
            const detail::EqualRange equal = Count::equal_range(node.slots, target, node.length);
            if (equal.end - equal.begin == 1) {
                lane.candidate = static_cast<std::uint32_t>(equal.begin);
                lane.state = LaneState::candidate;
                detail::Prefetcher::hint(&entries[node.first_entry + equal.begin]);
                return false;
            }
            exit = equal.begin;
            if (equal.end != equal.begin) {
                const NodeStep step = search_ties(node, query, equal);
                if (step.found) {
                    results[lane.query] = &entries[node.first_entry + step.position].value;
                    return true;
                }
                exit = step.position;
            }
        }
        if (((node.child_exits >> exit) & 1) == 0) {
            results[lane.query] = nullptr;
            return true;
        }
        lane.node = child_of(node, exit);
        lane.state = LaneState::node;
        detail::Prefetcher::hint(&nodes[lane.node]);
        return false;
    }

    // Fences of a node still to fill: the keys just outside it, by index
    // into the sorted keys, or no_key where it runs to the end.
    struct NodeFences final {
        KeyIndex lower;
        KeyIndex upper;
    };

    void lay_out(const detail::PackedTree& packed, std::vector<Key>& keys, std::vector<Value>& values) {
        const std::size_t node_count = packed.node_count();
        nodes.resize(node_count);
        entries.reserve(keys.size());
        std::vector<NodeFences> fences(node_count, NodeFences{.lower = no_key, .upper = no_key});
        std::vector<std::size_t> depth(node_count, 1);
        std::size_t node_index = 0;
        for (node_index = 0; node_index < node_count; node_index += 1) {
            Node& node = nodes[node_index];
            const std::size_t first = packed.key_begin[node_index];
            const std::size_t end = packed.key_begin[node_index + 1];
            const std::size_t length = end - first;
            const NodeFences own = fences[node_index];
            std::size_t prefix = 0;
            if (own.lower != no_key && own.upper != no_key) {
                prefix = Traits::common_prefix_length(keys[static_cast<std::size_t>(own.lower)],
                                                      keys[static_cast<std::size_t>(own.upper)]);
            }
            if (prefix > std::numeric_limits<std::uint16_t>::max()) {
                prefix = std::numeric_limits<std::uint16_t>::max();
            }
            node.child_exits = packed.child_exits[node_index];
            node.first_entry = static_cast<std::uint32_t>(first);
            node.first_child = packed.first_child[node_index];
            node.length = static_cast<std::uint16_t>(length);
            node.prefix = static_cast<std::uint16_t>(prefix);
            std::size_t slot = 0;
            for (slot = 0; slot < Node::capacity; slot += 1) {
                node.slots[slot] = padding_slot;
            }
            for (slot = 0; slot < length; slot += 1) {
                const std::size_t key = static_cast<std::size_t>(packed.keys[first + slot]);
                const Query query = Traits::query_of(keys[key]);
                node.slots[slot] = window_slot(query, prefix);
            }
            // Children inherit fences from the keys around their exits.
            std::uint32_t child = node.first_child;
            std::size_t exit = 0;
            for (exit = 0; exit <= length; exit += 1) {
                if (((node.child_exits >> exit) & 1) == 0) {
                    continue;
                }
                const KeyIndex lower = exit > 0 ? packed.keys[first + exit - 1] : own.lower;
                const KeyIndex upper = exit < length ? packed.keys[first + exit] : own.upper;
                fences[child] = NodeFences{.lower = lower, .upper = upper};
                depth[child] = depth[node_index] + 1;
                child += 1;
            }
            if (depth[node_index] > statistics.height) {
                statistics.height = depth[node_index];
            }
        }
        // Entries move in node order once every window has been taken.
        for (node_index = 0; node_index < packed.keys.size(); node_index += 1) {
            const std::size_t key = static_cast<std::size_t>(packed.keys[node_index]);
            entries.push_back(Entry{.key = std::move(keys[key]), .value = std::move(values[key])});
        }
        statistics.nodes = node_count;
    }

    std::vector<Node> nodes;
    std::vector<Entry> entries;
    Statistics statistics;
};

} // namespace stx_btree
