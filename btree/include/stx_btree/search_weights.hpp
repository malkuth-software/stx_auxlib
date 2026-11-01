#pragma once

// Search weights and binary search tree shapes, for the optimal tree
// constructions (optimal_tree.hpp) and the static map built on them
// (optimal_map.hpp).
//
// The conventions are the manuscript's (falseywinchnet/papers_please,
// obst/manuscript): n keys 0..n-1 found with weights p_i, and n + 1 gaps
// 0..n searched without success with weights q_j, where gap j lies between
// key j - 1 and key j. A key at depth d (the root has depth 0) costs p (d + 1)
// and a gap whose external position hangs below d keys costs q d: the cost of
// a tree is its number of three-way comparisons, weighted. The interval
// (i, j), i <= j, holds keys i..j-1 and gaps i..j, so its mass is
// W(i, j) = q_i + sum over i <= k < j of (p_k + q_{k+1}).
//
// Costs are exact integers. ExactCost is a signed 128-bit integer, which
// GCC and Clang provide as an extension; the constructions check that their
// largest intermediate value fits before they start.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace stx_btree {

__extension__ typedef __int128 ExactCost;

using SearchWeight = std::uint64_t;

// A key's position in sorted order. Signed, so that no_key can mark an
// absent child.
using KeyIndex = std::int32_t;

inline constexpr KeyIndex no_key = -1;

inline constexpr std::size_t maximum_search_keys =
    static_cast<std::size_t>(std::numeric_limits<KeyIndex>::max()) - 1;

struct SearchWeights final {
    std::vector<SearchWeight> keys; // p, one per key
    std::vector<SearchWeight> gaps; // q, one more than keys
};

// A binary search tree over keys 0..n-1 whose in-order is 0..n-1. The root
// is no_key exactly when n = 0.
struct BinaryTreeShape final {
    KeyIndex root = no_key;
    std::vector<KeyIndex> left;  // n entries, a key or no_key
    std::vector<KeyIndex> right; // n entries, a key or no_key
};

struct SearchTree final {
    ExactCost cost = 0;
    BinaryTreeShape shape;
};

class SearchWeightRules final {
public:
    // Throws std::invalid_argument unless there is exactly one more gap than
    // keys and the key count fits KeyIndex.
    static void validate(const SearchWeights& weights) {
        if (weights.keys.size() > maximum_search_keys) {
            throw std::invalid_argument("too many keys for a search tree");
        }
        if (weights.gaps.size() != weights.keys.size() + 1) {
            throw std::invalid_argument("search weights need one more gap weight than key weights");
        }
    }

    [[nodiscard]] static ExactCost total_mass(const SearchWeights& weights) noexcept {
        ExactCost total = 0;
        std::size_t index = 0;
        for (index = 0; index < weights.keys.size(); index += 1) {
            total += static_cast<ExactCost>(weights.keys[index]);
        }
        for (index = 0; index < weights.gaps.size(); index += 1) {
            total += static_cast<ExactCost>(weights.gaps[index]);
        }
        return total;
    }

    [[nodiscard]] static bool all_keys_positive(const SearchWeights& weights) noexcept {
        std::size_t index = 0;
        for (index = 0; index < weights.keys.size(); index += 1) {
            if (weights.keys[index] == 0) {
                return false;
            }
        }
        return true;
    }
};

// Evaluation and checking of shapes, independent of how they were built.
class BinaryTreeRules final {
public:
    // Whether `shape` is a binary search tree over keys 0..n-1: every key
    // reached exactly once from the root, and in-order 0..n-1.
    [[nodiscard]] static bool is_search_tree(const BinaryTreeShape& shape, std::size_t key_count) {
        if (shape.left.size() != key_count || shape.right.size() != key_count) {
            return false;
        }
        if (key_count == 0) {
            return shape.root == no_key;
        }
        std::vector<KeyIndex> pending;
        pending.reserve(key_count);
        KeyIndex expected = 0;
        KeyIndex current = shape.root;
        std::size_t steps = 0;
        while (current != no_key || !pending.empty()) {
            while (current != no_key) {
                if (current < 0 || static_cast<std::size_t>(current) >= key_count || steps > key_count) {
                    return false;
                }
                steps += 1;
                pending.push_back(current);
                current = shape.left[static_cast<std::size_t>(current)];
            }
            const KeyIndex visited = pending.back();
            pending.pop_back();
            if (visited != expected) {
                return false;
            }
            expected += 1;
            current = shape.right[static_cast<std::size_t>(visited)];
        }
        return static_cast<std::size_t>(expected) == key_count;
    }

    // The weighted comparison count of `shape` (see the head of this file).
    // The shape must satisfy is_search_tree.
    [[nodiscard]] static ExactCost cost(const BinaryTreeShape& shape, const SearchWeights& weights) {
        ExactCost total = 0;
        if (shape.root == no_key) {
            return total; // the one gap hangs below no key
        }
        struct Visit final {
            KeyIndex key;
            KeyIndex low;  // first key of its subtree
            KeyIndex high; // one past its last key
            ExactCost depth;
        };
        std::vector<Visit> pending;
        pending.push_back(Visit{.key = shape.root,
                                .low = 0,
                                .high = static_cast<KeyIndex>(weights.keys.size()),
                                .depth = 0});
        while (!pending.empty()) {
            const Visit visit = pending.back();
            pending.pop_back();
            const std::size_t key = static_cast<std::size_t>(visit.key);
            const ExactCost below = visit.depth + 1;
            total += static_cast<ExactCost>(weights.keys[key]) * below;
            const KeyIndex left_child = shape.left[key];
            const KeyIndex right_child = shape.right[key];
            if (left_child == no_key) {
                total += static_cast<ExactCost>(weights.gaps[key]) * below;
            } else {
                pending.push_back(Visit{.key = left_child, .low = visit.low, .high = visit.key, .depth = below});
            }
            if (right_child == no_key) {
                total += static_cast<ExactCost>(weights.gaps[key + 1]) * below;
            } else {
                pending.push_back(Visit{.key = right_child, .low = visit.key + 1, .high = visit.high, .depth = below});
            }
        }
        return total;
    }

    // Children from a parent array (no_key at the root), as the
    // reconstructions produce it.
    [[nodiscard]] static BinaryTreeShape from_parents(const std::vector<KeyIndex>& parent) {
        BinaryTreeShape shape;
        const std::size_t key_count = parent.size();
        shape.left.assign(key_count, no_key);
        shape.right.assign(key_count, no_key);
        std::size_t key = 0;
        for (key = 0; key < key_count; key += 1) {
            const KeyIndex up = parent[key];
            if (up == no_key) {
                shape.root = static_cast<KeyIndex>(key);
            } else if (static_cast<std::size_t>(up) > key) {
                shape.left[static_cast<std::size_t>(up)] = static_cast<KeyIndex>(key);
            } else {
                shape.right[static_cast<std::size_t>(up)] = static_cast<KeyIndex>(key);
            }
        }
        return shape;
    }
};

} // namespace stx_btree
