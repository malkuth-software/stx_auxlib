#pragma once

// Optimal binary search trees for given search weights (search_weights.hpp
// states the cost). The construction is chosen by the caller, who knows the
// weights (TreeConstruction):
//
//   knuth            Knuth's O(n^2) dynamic program with root monotonicity.
//                    Exact, for any weights. Diagonals by length over a
//                    packed triangle of costs, keeping only the previous
//                    diagonal's roots, and recovering the final tree's roots
//                    afterwards; 8 bytes a cell when the costs fit 64 bits
//                    (W (ceil(log2(n + 1)) + 2) < 2^63), else 16.
//   knuth_scattered  The same recurrence, rows from the last down with the
//                    costs stored by column, so each cell reads one run of
//                    its column beside the cell it writes. Exact. Faster than
//                    `knuth` when the weights are scattered over the keys
//                    (Zipf or log-normal popularity in random key order;
//                    1.4x at n = 16384 on the M4), slower when they vary
//                    smoothly along the keys (uniform, ramps, a Laplacian
//                    around a centre key; up to 2.4x).
//   garsia_wachs     Exact when every key weight is zero, the alphabetic-code
//                    case (only unsuccessful searches weighted): Garsia and
//                    Wachs's combination, O(n) memory, O(n log n) comparisons
//                    and at worst O(n^2) element moves. Rejects other weights.
//   balanced         The minimum-internal-path tree (every root the middle
//                    key). Optimal exactly when all key weights are equal and
//                    all gap weights are equal; O(n).
//   bottom_up        Near-optimal: optimal trees of the two halves joined
//                    around the middle key by re-solving only their facing
//                    spines (every subtree hanging off them kept whole),
//                    recursively. Within 0.4-1.7% of optimal on the
//                    benchmark's families at 16384 keys, in about 1 ms where
//                    Knuth takes about a second (docs/OPTIMAL_TREES.md).
//   weight_balanced  Each root splits its interval's mass as evenly as the
//                    keys allow (Mehlhorn's bisection rule). O(n log n), not
//                    exact.
//
// The exact constructions return the same optimal cost; their trees may
// differ where several trees are optimal (both Knuth layouts return the
// leftmost optimal root of every interval).

#include "stx_btree/search_weights.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace stx_btree {

enum class TreeConstruction : std::uint8_t {
    knuth,
    weight_balanced,
    knuth_scattered,
    garsia_wachs,
    balanced,
    bottom_up,
};

class OptimalTreeBuilder final {
public:
    [[nodiscard]] static SearchTree build(const SearchWeights& weights, TreeConstruction construction) {
        switch (construction) {
        case TreeConstruction::knuth:
            return knuth(weights);
        case TreeConstruction::weight_balanced:
            return weight_balanced(weights);
        case TreeConstruction::knuth_scattered:
            return knuth_scattered(weights);
        case TreeConstruction::garsia_wachs:
            return garsia_wachs(weights);
        case TreeConstruction::balanced:
            return balanced(weights);
        case TreeConstruction::bottom_up:
            return bottom_up(weights);
        }
        throw std::invalid_argument("unknown tree construction");
    }

    // ---- Knuth -------------------------------------------------------------

    // Interval (i, j) reads the roots of (i, j - 1) and (i + 1, j), which the
    // previous length left at i and i + 1, and overwrites i only after both
    // are read; so one array of roots serves, not a table. The final tree's
    // roots are recovered afterwards (recovered_tree).
    [[nodiscard]] static SearchTree knuth(const SearchWeights& weights) {
        SearchWeightRules::validate(weights);
        if (narrow_costs_fit(weights)) {
            return knuth_by_diagonals<std::int64_t>(weights);
        }
        return knuth_by_diagonals<ExactCost>(weights);
    }

    // Rows i from the last down, j rising, costs stored by column: the
    // candidates' right terms C(r + 1, j) are one run of column j beside the
    // cell (i, j) being written, and the left terms C(i, r) are the current
    // row, kept in one array. root(i, j - 1) is the previous cell's root and
    // root(i + 1, j) the previous row's, again one array.
    [[nodiscard]] static SearchTree knuth_scattered(const SearchWeights& weights) {
        SearchWeightRules::validate(weights);
        if (narrow_costs_fit(weights)) {
            return knuth_by_rows<std::int64_t>(weights);
        }
        return knuth_by_rows<ExactCost>(weights);
    }

    // ---- Weight-balanced bisection ---------------------------------------

    [[nodiscard]] static SearchTree weight_balanced(const SearchWeights& weights) {
        SearchWeightRules::validate(weights);
        const std::size_t key_count = weights.keys.size();
        const std::vector<ExactCost> prefix = mass_prefix(weights);
        std::vector<KeyIndex> parent(key_count, no_key);
        std::vector<PendingInterval> pending;
        pending.push_back(PendingInterval{.first = 0, .end = static_cast<KeyIndex>(key_count), .parent = no_key});
        while (!pending.empty()) {
            const PendingInterval interval = pending.back();
            pending.pop_back();
            if (interval.first == interval.end) {
                continue;
            }
            const KeyIndex chosen = balanced_root(prefix, weights, interval.first, interval.end);
            parent[static_cast<std::size_t>(chosen)] = interval.parent;
            pending.push_back(PendingInterval{.first = interval.first, .end = chosen, .parent = chosen});
            pending.push_back(PendingInterval{.first = chosen + 1, .end = interval.end, .parent = chosen});
        }
        SearchTree tree;
        tree.shape = BinaryTreeRules::from_parents(parent);
        tree.cost = BinaryTreeRules::cost(tree.shape, weights);
        return tree;
    }

    // ---- Garsia-Wachs ------------------------------------------------------

    // Every key weight must be zero (std::invalid_argument otherwise): the
    // cost is then the gaps' weighted depths, an optimal alphabetic tree over
    // the n + 1 gaps. Phase one repeatedly combines the leftmost pair whose
    // left neighbour is at most its right neighbour's weight and moves the
    // sum left past every lighter item, building a tree that is not in key
    // order; phase two reads each gap's depth in it; phase three builds the
    // search tree with exactly those depths (Garsia and Wachs 1977; Knuth,
    // TAOCP 6.2.2). The working list is an array, so a combination shifts its
    // tail: O(n log n) comparisons, O(n^2) element moves at worst.
    [[nodiscard]] static SearchTree garsia_wachs(const SearchWeights& weights) {
        SearchWeightRules::validate(weights);
        const std::size_t key_count = weights.keys.size();
        std::size_t key = 0;
        for (key = 0; key < key_count; key += 1) {
            if (weights.keys[key] != 0) {
                throw std::invalid_argument("garsia_wachs needs every key weight zero");
            }
        }
        SearchTree tree;
        if (key_count == 0) {
            return tree;
        }
        const std::size_t leaf_count = key_count + 1;
        // Nodes 0..n are the gaps; each combination adds one.
        std::vector<std::size_t> first_child(2 * leaf_count - 1, 0);
        std::vector<std::size_t> second_child(2 * leaf_count - 1, 0);
        std::vector<CombinedItem> work;
        work.reserve(leaf_count);
        std::size_t leaf = 0;
        for (leaf = 0; leaf < leaf_count; leaf += 1) {
            work.push_back(CombinedItem{.weight = static_cast<ExactCost>(weights.gaps[leaf]), .node = leaf});
        }
        std::size_t node_count = leaf_count;
        std::size_t start = 1;
        while (work.size() > 1) {
            // The leftmost k >= 1 with w[k - 1] <= w[k + 1], beyond the end
            // counting as infinite. No k below `start` qualifies: the items
            // before the last insertion point are unchanged.
            std::size_t k = start;
            while (k + 1 < work.size() && work[k - 1].weight > work[k + 1].weight) {
                k += 1;
            }
            const CombinedItem combined{.weight = work[k - 1].weight + work[k].weight, .node = node_count};
            first_child[node_count] = work[k - 1].node;
            second_child[node_count] = work[k].node;
            node_count += 1;
            work.erase(work.begin() + static_cast<std::ptrdiff_t>(k - 1), work.begin() + static_cast<std::ptrdiff_t>(k + 1));
            // Right after the nearest earlier item at least as heavy.
            std::size_t position = k - 1;
            while (position > 0 && work[position - 1].weight < combined.weight) {
                position -= 1;
            }
            work.insert(work.begin() + static_cast<std::ptrdiff_t>(position), combined);
            start = position > 1 ? position - 1 : 1;
        }
        std::vector<std::size_t> depth(leaf_count, 0);
        std::vector<DepthVisit> visits;
        visits.push_back(DepthVisit{.node = work[0].node, .depth = 0});
        while (!visits.empty()) {
            const DepthVisit visit = visits.back();
            visits.pop_back();
            if (visit.node < leaf_count) {
                depth[visit.node] = visit.depth;
            } else {
                visits.push_back(DepthVisit{.node = first_child[visit.node], .depth = visit.depth + 1});
                visits.push_back(DepthVisit{.node = second_child[visit.node], .depth = visit.depth + 1});
            }
        }
        // Rebuild in key order: adjacent runs at equal depth join under the
        // key between them (key k separates gaps k and k + 1).
        std::vector<KeyIndex> parent(key_count, no_key);
        std::vector<LeafRun> runs;
        for (leaf = 0; leaf < leaf_count; leaf += 1) {
            runs.push_back(LeafRun{.depth = depth[leaf], .root = no_key, .last_leaf = leaf});
            while (runs.size() >= 2 && runs[runs.size() - 1].depth == runs[runs.size() - 2].depth) {
                const LeafRun right_run = runs[runs.size() - 1];
                const LeafRun left_run = runs[runs.size() - 2];
                runs.pop_back();
                runs.pop_back();
                const KeyIndex joined = static_cast<KeyIndex>(left_run.last_leaf);
                if (left_run.root != no_key) {
                    parent[static_cast<std::size_t>(left_run.root)] = joined;
                }
                if (right_run.root != no_key) {
                    parent[static_cast<std::size_t>(right_run.root)] = joined;
                }
                runs.push_back(LeafRun{.depth = left_run.depth - 1, .root = joined, .last_leaf = right_run.last_leaf});
            }
        }
        assert(runs.size() == 1 && runs[0].depth == 0);
        tree.shape = BinaryTreeRules::from_parents(parent);
        tree.cost = BinaryTreeRules::cost(tree.shape, weights);
        return tree;
    }

    // ---- Balanced ---------------------------------------------------------

    // Every root the middle key, so subtree sizes differ by at most one and
    // every gap lies on the last two levels: the least internal path length.
    // A tree's cost with constant key weight p and gap weight q is
    // (p + q) I + n p + 2 n q for internal path length I, so this tree is
    // optimal for such weights; for others it is only balanced.
    [[nodiscard]] static SearchTree balanced(const SearchWeights& weights) {
        SearchWeightRules::validate(weights);
        const std::size_t key_count = weights.keys.size();
        std::vector<KeyIndex> parent(key_count, no_key);
        std::vector<PendingInterval> pending;
        pending.push_back(PendingInterval{.first = 0, .end = static_cast<KeyIndex>(key_count), .parent = no_key});
        while (!pending.empty()) {
            const PendingInterval interval = pending.back();
            pending.pop_back();
            if (interval.first == interval.end) {
                continue;
            }
            const KeyIndex chosen = static_cast<KeyIndex>(interval.first + (interval.end - interval.first) / 2);
            parent[static_cast<std::size_t>(chosen)] = interval.parent;
            pending.push_back(PendingInterval{.first = interval.first, .end = chosen, .parent = chosen});
            pending.push_back(PendingInterval{.first = chosen + 1, .end = interval.end, .parent = chosen});
        }
        SearchTree tree;
        tree.shape = BinaryTreeRules::from_parents(parent);
        tree.cost = BinaryTreeRules::cost(tree.shape, weights);
        return tree;
    }

    // ---- Bottom-up joins ----------------------------------------------------

    // Not exact. The halves around the middle key m are built recursively;
    // then the left half's right spine, m and the right half's left spine
    // are re-solved as one small optimal tree whose gaps are the subtrees
    // hanging off those spines, each kept whole (Knuth over the spine keys,
    // the hanging subtrees as atoms). A join costs the square of the two
    // spines' lengths; spines are short on most weights and as long as the
    // half on a geometric ramp.
    [[nodiscard]] static SearchTree bottom_up(const SearchWeights& weights) {
        SearchWeightRules::validate(weights);
        const std::size_t key_count = weights.keys.size();
        SearchTree tree;
        if (key_count == 0) {
            return tree;
        }
        BottomUpState state{.prefix = mass_prefix(weights),
                            .left = std::vector<KeyIndex>(key_count, no_key),
                            .right = std::vector<KeyIndex>(key_count, no_key),
                            .subtree_cost = std::vector<ExactCost>(key_count, 0),
                            .chain_keys = {},
                            .atoms = {},
                            .spine = {},
                            .chain_costs = {},
                            .chain_roots = {}};
        tree.shape.root = bottom_up_build(weights, state, 0, key_count);
        tree.shape.left = std::move(state.left);
        tree.shape.right = std::move(state.right);
        tree.cost = BinaryTreeRules::cost(tree.shape, weights);
        return tree;
    }

private:
    // An interval (first, end) of keys still to place, and the key it hangs
    // below.
    struct PendingInterval final {
        KeyIndex first;
        KeyIndex end;
        KeyIndex parent;
    };

    struct CombinedItem final {
        ExactCost weight;
        std::size_t node;
    };

    struct DepthVisit final {
        std::size_t node;
        std::size_t depth;
    };

    // A run of consecutive gaps already joined into one subtree.
    struct LeafRun final {
        std::size_t depth;
        KeyIndex root;  // no_key for a single gap
        std::size_t last_leaf;
    };

    // Cells (i, j), 0 <= i <= j <= n, row by row: row i starts at offset[i] + i.
    template <typename Cost>
    struct RowTriangle final {
        explicit RowTriangle(std::size_t key_count) : cells((key_count + 1) * (key_count + 2) / 2, 0), offset(key_count + 1, 0) {
            const std::size_t side = key_count + 1;
            std::size_t row = 0;
            for (row = 0; row <= key_count; row += 1) {
                offset[row] = row * side - row * (row + 1) / 2;
            }
        }

        [[nodiscard]] Cost at(std::size_t first, std::size_t end) const noexcept {
            return cells[offset[first] + end];
        }

        std::vector<Cost> cells;
        std::vector<std::size_t> offset;
    };

    // Cells (i, j), 0 <= i <= j <= n, column by column: column j starts at
    // j (j + 1) / 2.
    template <typename Cost>
    struct ColumnTriangle final {
        explicit ColumnTriangle(std::size_t key_count) : cells((key_count + 1) * (key_count + 2) / 2, 0) {}

        [[nodiscard]] static std::size_t column_start(std::size_t end) noexcept {
            return end * (end + 1) / 2;
        }

        [[nodiscard]] Cost at(std::size_t first, std::size_t end) const noexcept {
            return cells[column_start(end) + first];
        }

        std::vector<Cost> cells;
    };

    // Whether every cost and candidate sum fits 64 bits. An interval's
    // optimum is at most its mass times ceil(log2(n + 1)) (a balanced tree's
    // depth bound), a candidate sum of two disjoint intervals' optima at most
    // that, and the cell adds the mass once more.
    [[nodiscard]] static bool narrow_costs_fit(const SearchWeights& weights) noexcept {
        std::size_t key_bits = 0;
        std::size_t remaining = weights.keys.size();
        while (remaining != 0) {
            key_bits += 1;
            remaining >>= 1;
        }
        const ExactCost limit = static_cast<ExactCost>(std::numeric_limits<std::int64_t>::max());
        return SearchWeightRules::total_mass(weights) <= limit / static_cast<ExactCost>(key_bits + 2);
    }

    template <typename Cost>
    [[nodiscard]] static std::vector<Cost> cost_prefix(const SearchWeights& weights) {
        const std::size_t key_count = weights.keys.size();
        std::vector<Cost> prefix(key_count + 1, 0);
        std::size_t key = 0;
        for (key = 0; key < key_count; key += 1) {
            prefix[key + 1] = prefix[key] + static_cast<Cost>(weights.keys[key]) + static_cast<Cost>(weights.gaps[key + 1]);
        }
        return prefix;
    }

    template <typename Cost>
    [[nodiscard]] static Cost cost_mass(const std::vector<Cost>& prefix, const SearchWeights& weights, std::size_t first, std::size_t end) noexcept {
        return prefix[end] - prefix[first] + static_cast<Cost>(weights.gaps[first]);
    }

    template <typename Cost>
    [[nodiscard]] static SearchTree knuth_by_diagonals(const SearchWeights& weights) {
        const std::size_t key_count = weights.keys.size();
        const std::vector<Cost> prefix = cost_prefix<Cost>(weights);
        RowTriangle<Cost> table(key_count);
        // roots[i]: root of (i, i + length) once that length is done.
        std::vector<std::size_t> roots(key_count + 1, 0);
        std::size_t length = 0;
        for (length = 1; length <= key_count; length += 1) {
            std::size_t first = 0;
            for (first = 0; first + length <= key_count; first += 1) {
                const std::size_t end = first + length;
                // Roots are monotone (Knuth; Yao for non-negative weights):
                // root(i, j - 1) <= root(i, j) <= root(i + 1, j).
                const std::size_t low = length == 1 ? first : roots[first];
                const std::size_t high = length == 1 ? first : roots[first + 1];
                const Cost* const row = table.cells.data() + table.offset[first];
                Cost best = row[low] + table.at(low + 1, end);
                std::size_t best_root = low;
                std::size_t candidate = 0;
                for (candidate = low + 1; candidate <= high; candidate += 1) {
                    const Cost value = row[candidate] + table.at(candidate + 1, end);
                    if (value < best) {
                        best = value;
                        best_root = candidate;
                    }
                }
                table.cells[table.offset[first] + end] = best + cost_mass(prefix, weights, first, end);
                roots[first] = best_root;
            }
        }
        return recovered_tree(weights, prefix, table);
    }

    template <typename Cost>
    [[nodiscard]] static SearchTree knuth_by_rows(const SearchWeights& weights) {
        const std::size_t key_count = weights.keys.size();
        const std::vector<Cost> prefix = cost_prefix<Cost>(weights);
        ColumnTriangle<Cost> table(key_count);
        std::vector<Cost> row(key_count + 1, 0);           // C(i, r) for the current row i
        std::vector<std::size_t> roots(key_count + 1, 0);  // roots[j]: root(i + 1, j), then root(i, j)
        std::size_t first = key_count;
        while (first > 0) {
            first -= 1;
            row[first] = 0;
            std::size_t previous = first;  // root(i, j - 1)
            std::size_t end = 0;
            for (end = first + 1; end <= key_count; end += 1) {
                const std::size_t low = end == first + 1 ? first : previous;
                const std::size_t high = end == first + 1 ? first : roots[end];
                Cost* const column = table.cells.data() + ColumnTriangle<Cost>::column_start(end);
                Cost best = row[low] + column[low + 1];
                std::size_t best_root = low;
                std::size_t candidate = 0;
                for (candidate = low + 1; candidate <= high; candidate += 1) {
                    const Cost value = row[candidate] + column[candidate + 1];
                    if (value < best) {
                        best = value;
                        best_root = candidate;
                    }
                }
                const Cost value = best + cost_mass(prefix, weights, first, end);
                column[first] = value;
                row[end] = value;
                roots[end] = best_root;
                previous = best_root;
            }
        }
        return recovered_tree(weights, prefix, table);
    }

    // The final tree from the cost table alone: the root of each of its
    // intervals is the least r with C(i, r) + C(r + 1, j) = C(i, j) - W(i, j),
    // exact in integers, which is the leftmost optimal root, the one the
    // restricted search above settles on as well.
    template <typename Cost, typename Table>
    [[nodiscard]] static SearchTree recovered_tree(const SearchWeights& weights, const std::vector<Cost>& prefix, const Table& table) {
        const std::size_t key_count = weights.keys.size();
        std::vector<KeyIndex> parent(key_count, no_key);
        std::vector<PendingInterval> pending;
        pending.push_back(PendingInterval{.first = 0, .end = static_cast<KeyIndex>(key_count), .parent = no_key});
        while (!pending.empty()) {
            const PendingInterval interval = pending.back();
            pending.pop_back();
            if (interval.first == interval.end) {
                continue;
            }
            const std::size_t first = static_cast<std::size_t>(interval.first);
            const std::size_t end = static_cast<std::size_t>(interval.end);
            const Cost target = table.at(first, end) - cost_mass(prefix, weights, first, end);
            std::size_t chosen = first;
            while (table.at(first, chosen) + table.at(chosen + 1, end) != target) {
                chosen += 1;
                assert(chosen < end);
            }
            const KeyIndex chosen_key = static_cast<KeyIndex>(chosen);
            parent[chosen] = interval.parent;
            pending.push_back(PendingInterval{.first = interval.first, .end = chosen_key, .parent = chosen_key});
            pending.push_back(PendingInterval{.first = chosen_key + 1, .end = interval.end, .parent = chosen_key});
        }
        SearchTree tree;
        tree.cost = static_cast<ExactCost>(table.at(0, key_count));
        tree.shape = BinaryTreeRules::from_parents(parent);
        return tree;
    }

    // The bottom-up construction's tree and scratch. chain_keys and atoms
    // describe the current join: atom t is the subtree between chain keys
    // t - 1 and t.
    struct BottomUpState final {
        std::vector<ExactCost> prefix;
        std::vector<KeyIndex> left;
        std::vector<KeyIndex> right;
        std::vector<ExactCost> subtree_cost;
        std::vector<KeyIndex> chain_keys;
        std::vector<KeyIndex> atoms;
        std::vector<KeyIndex> spine;
        std::vector<ExactCost> chain_costs;
        std::vector<std::size_t> chain_roots;
    };

    [[nodiscard]] static ExactCost subtree_cost_of(const BottomUpState& state, KeyIndex node) noexcept {
        return node == no_key ? 0 : state.subtree_cost[static_cast<std::size_t>(node)];
    }

    // Builds keys [first, end); returns the root, no_key when empty.
    static KeyIndex bottom_up_build(const SearchWeights& weights, BottomUpState& state, std::size_t first, std::size_t end) {
        if (first == end) {
            return no_key;
        }
        if (end == first + 1) {
            state.left[first] = no_key;
            state.right[first] = no_key;
            state.subtree_cost[first] = interval_mass(state.prefix, weights, first, end);
            return static_cast<KeyIndex>(first);
        }
        const std::size_t middle = first + (end - first) / 2;
        const KeyIndex left_root = bottom_up_build(weights, state, first, middle);
        const KeyIndex right_root = bottom_up_build(weights, state, middle + 1, end);
        return bottom_up_join(weights, state, left_root, first, middle, right_root, end);
    }

    // Joins the trees of [first, middle) and [middle + 1, end) around middle.
    static KeyIndex bottom_up_join(const SearchWeights& weights, BottomUpState& state, KeyIndex left_root, std::size_t first,
                                   std::size_t middle, KeyIndex right_root, std::size_t end) {
        state.chain_keys.clear();
        state.atoms.clear();
        state.spine.clear();
        KeyIndex node = left_root;
        KeyIndex last = no_key;
        while (node != no_key) {
            state.atoms.push_back(state.left[static_cast<std::size_t>(node)]);
            state.chain_keys.push_back(node);
            last = node;
            node = state.right[static_cast<std::size_t>(node)];
        }
        state.atoms.push_back(last == no_key ? no_key : state.right[static_cast<std::size_t>(last)]);
        state.chain_keys.push_back(static_cast<KeyIndex>(middle));
        node = right_root;
        while (node != no_key) {
            state.spine.push_back(node);
            node = state.left[static_cast<std::size_t>(node)];
        }
        state.atoms.push_back(state.spine.empty() ? no_key : state.left[static_cast<std::size_t>(state.spine.back())]);
        std::size_t index = state.spine.size();
        while (index > 0) {
            index -= 1;
            state.chain_keys.push_back(state.spine[index]);
            state.atoms.push_back(state.right[static_cast<std::size_t>(state.spine[index])]);
        }
        // Knuth over the chain: interval (a, b) holds atoms a..b and chain
        // keys a..b-1. Every tree over it holds each atom once, so leaving
        // the atoms' own costs out puts it in Knuth's form and his root
        // bounds hold.
        const std::size_t count = state.chain_keys.size();
        const std::size_t side = count + 1;
        state.chain_costs.assign(side * side, 0);
        state.chain_roots.assign(side * side, 0);
        std::size_t length = 0;
        for (length = 1; length <= count; length += 1) {
            std::size_t a = 0;
            for (a = 0; a + length <= count; a += 1) {
                const std::size_t b = a + length;
                std::size_t low = a;
                std::size_t high = b - 1;
                if (length > 1) {
                    low = state.chain_roots[a * side + b - 1];
                    high = state.chain_roots[(a + 1) * side + b];
                }
                ExactCost best = state.chain_costs[a * side + low] + state.chain_costs[(low + 1) * side + b];
                std::size_t best_root = low;
                std::size_t candidate = 0;
                for (candidate = low + 1; candidate <= high; candidate += 1) {
                    const ExactCost value = state.chain_costs[a * side + candidate] + state.chain_costs[(candidate + 1) * side + b];
                    if (value < best) {
                        best = value;
                        best_root = candidate;
                    }
                }
                state.chain_costs[a * side + b] = best + chain_mass(weights, state, first, end, a, b);
                state.chain_roots[a * side + b] = best_root;
            }
        }
        return chain_link(weights, state, first, end, 0, count);
    }

    // The keys [low, high) that chain interval (a, b) covers.
    [[nodiscard]] static ExactCost chain_mass(const SearchWeights& weights, const BottomUpState& state, std::size_t first, std::size_t end,
                                              std::size_t a, std::size_t b) noexcept {
        const std::size_t low = a == 0 ? first : static_cast<std::size_t>(state.chain_keys[a - 1]) + 1;
        const std::size_t high = b == state.chain_keys.size() ? end : static_cast<std::size_t>(state.chain_keys[b]);
        return interval_mass(state.prefix, weights, low, high);
    }

    // Links the solved chain interval (a, b) into the tree; returns its root.
    static KeyIndex chain_link(const SearchWeights& weights, BottomUpState& state, std::size_t first, std::size_t end, std::size_t a,
                               std::size_t b) {
        if (a == b) {
            return state.atoms[a];
        }
        const std::size_t side = state.chain_keys.size() + 1;
        const std::size_t chosen = state.chain_roots[a * side + b];
        const KeyIndex key = state.chain_keys[chosen];
        const KeyIndex left_child = chain_link(weights, state, first, end, a, chosen);
        const KeyIndex right_child = chain_link(weights, state, first, end, chosen + 1, b);
        const std::size_t at = static_cast<std::size_t>(key);
        state.left[at] = left_child;
        state.right[at] = right_child;
        state.subtree_cost[at] = chain_mass(weights, state, first, end, a, b) + subtree_cost_of(state, left_child) + subtree_cost_of(state, right_child);
        return key;
    }

    // prefix[k] = sum over t < k of (p_t + q_{t+1}), so that
    // W(i, j) = prefix[j] - prefix[i] + q_i.
    [[nodiscard]] static std::vector<ExactCost> mass_prefix(const SearchWeights& weights) {
        const std::size_t key_count = weights.keys.size();
        std::vector<ExactCost> prefix(key_count + 1, 0);
        std::size_t key = 0;
        for (key = 0; key < key_count; key += 1) {
            prefix[key + 1] = prefix[key] + static_cast<ExactCost>(weights.keys[key]) +
                              static_cast<ExactCost>(weights.gaps[key + 1]);
        }
        return prefix;
    }

    [[nodiscard]] static ExactCost interval_mass(
        const std::vector<ExactCost>& prefix, const SearchWeights& weights, std::size_t first, std::size_t end) noexcept {
        return prefix[end] - prefix[first] + static_cast<ExactCost>(weights.gaps[first]);
    }

    // The root r of (first, end) that minimizes |W(first, r) - W(r + 1, end)|,
    // the least such r on a tie. W(first, r) - W(r + 1, end) grows with r, so
    // a binary search finds where it turns non-negative.
    [[nodiscard]] static KeyIndex balanced_root(
        const std::vector<ExactCost>& prefix, const SearchWeights& weights, KeyIndex first, KeyIndex end) noexcept {
        const std::size_t low_key = static_cast<std::size_t>(first);
        const std::size_t end_key = static_cast<std::size_t>(end);
        std::size_t low = low_key;
        std::size_t high = end_key; // the answer of the search lies in [low, high]
        while (low < high) {
            const std::size_t middle = low + (high - low) / 2;
            const ExactCost difference = split_difference(prefix, weights, low_key, middle, end_key);
            if (difference >= 0) {
                high = middle;
            } else {
                low = middle + 1;
            }
        }
        std::size_t chosen = low < end_key ? low : end_key - 1;
        if (chosen > low_key) {
            const ExactCost here = split_difference(prefix, weights, low_key, chosen, end_key);
            const ExactCost before = split_difference(prefix, weights, low_key, chosen - 1, end_key);
            const ExactCost here_size = here < 0 ? -here : here;
            const ExactCost before_size = before < 0 ? -before : before;
            if (before_size <= here_size) {
                chosen -= 1;
            }
        }
        return static_cast<KeyIndex>(chosen);
    }

    [[nodiscard]] static ExactCost split_difference(
        const std::vector<ExactCost>& prefix, const SearchWeights& weights,
        std::size_t first, std::size_t root, std::size_t end) noexcept {
        const ExactCost left_mass = interval_mass(prefix, weights, first, root);
        const ExactCost right_mass = interval_mass(prefix, weights, root + 1, end);
        return left_mass - right_mass;
    }

};

} // namespace stx_btree
