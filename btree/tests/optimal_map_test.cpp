// The tree packing and the static optimal map.
//
// Packing: its reported cost equals the cost of walking every search through
// the packed nodes; with capacity 1 it is the binary tree's own cost; on
// small trees it equals the best of every way of cutting the tree's edges
// into nodes of at most `capacity` keys.
//
// Map: every key is found with its value, keys absent are not, find_many
// agrees with find, for integer, signed and string keys and each
// construction.

#include "stx_btree/optimal_map.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using stx_btree::BinaryTreeRules;
using stx_btree::BinaryTreeShape;
using stx_btree::ExactCost;
using stx_btree::KeyIndex;
using stx_btree::OptimalMap;
using stx_btree::OptimalMapPlan;
using stx_btree::OptimalTreeBuilder;
using stx_btree::SearchTree;
using stx_btree::SearchWeight;
using stx_btree::SearchWeights;
using stx_btree::TreeConstruction;
using stx_btree::no_key;
using stx_btree::detail::ColdSubtreeLayout;
using stx_btree::detail::PackedTree;
using stx_btree::detail::TreePacker;
using stx_btree_tests::SplitMix64;
using stx_btree_tests::TestFailure;

SearchWeights random_weights(std::size_t key_count, SplitMix64& random, bool skewed) {
    SearchWeights weights;
    weights.keys.assign(key_count, 0);
    weights.gaps.assign(key_count + 1, 0);
    std::size_t index = 0;
    for (index = 0; index < key_count; index += 1) {
        weights.keys[index] = skewed ? 100000 / (1 + random.below(key_count)) : 1 + random.below(4);
    }
    for (index = 0; index <= key_count; index += 1) {
        weights.gaps[index] = random.below(3);
    }
    return weights;
}

// Nodes visited by a search for target (2 k + 1 for key k, 2 g for gap g),
// walking the packed nodes by key index.
std::size_t visits_for(const PackedTree& packed, std::size_t target, bool& found) {
    std::size_t visits = 0;
    std::size_t node = 0;
    found = false;
    while (true) {
        visits += 1;
        const std::size_t first = packed.key_begin[node];
        const std::size_t end = packed.key_begin[node + 1];
        std::size_t exit = 0;
        std::size_t position = 0;
        for (position = first; position < end; position += 1) {
            const std::size_t key_target = 2 * static_cast<std::size_t>(packed.keys[position]) + 1;
            if (key_target == target) {
                found = true;
                return visits;
            }
            if (key_target < target) {
                exit += 1;
            }
        }
        const std::uint64_t exits = packed.child_exits[node];
        if (((exits >> exit) & 1) == 0) {
            return visits;
        }
        const std::uint64_t before = exits & ((static_cast<std::uint64_t>(1) << exit) - 1);
        node = packed.first_child[node] + static_cast<std::size_t>(std::popcount(before));
    }
}

ExactCost walked_cost(const PackedTree& packed, const SearchWeights& weights) {
    ExactCost total = 0;
    const std::size_t key_count = weights.keys.size();
    if (key_count == 0) {
        return total;
    }
    std::size_t index = 0;
    for (index = 0; index < key_count; index += 1) {
        bool found = false;
        const std::size_t visits = visits_for(packed, 2 * index + 1, found);
        TestFailure::expect(found, "packing", "key not reached");
        total += static_cast<ExactCost>(weights.keys[index]) * static_cast<ExactCost>(visits);
    }
    for (index = 0; index <= key_count; index += 1) {
        bool found = false;
        const std::size_t visits = visits_for(packed, 2 * index, found);
        TestFailure::expect(!found, "packing", "gap matched a key");
        total += static_cast<ExactCost>(weights.gaps[index]) * static_cast<ExactCost>(visits);
    }
    return total;
}

void expect_packing_shape(const PackedTree& packed, std::size_t key_count, std::size_t capacity) {
    TestFailure::expect(packed.keys.size() == key_count, "packing", "keys lost or repeated");
    std::vector<bool> seen(key_count, false);
    std::size_t node = 0;
    for (node = 0; node < packed.node_count(); node += 1) {
        const std::size_t first = packed.key_begin[node];
        const std::size_t end = packed.key_begin[node + 1];
        TestFailure::expect(end > first && end - first <= capacity, "packing", "node size");
        std::size_t position = 0;
        for (position = first; position < end; position += 1) {
            const std::size_t key = static_cast<std::size_t>(packed.keys[position]);
            TestFailure::expect(!seen[key], "packing", "key in two nodes");
            seen[key] = true;
            if (position > first) {
                TestFailure::expect(packed.keys[position - 1] < packed.keys[position], "packing", "node not ascending");
            }
        }
    }
}

// The best packing by brute force: every subset of the tree's edges kept
// inside nodes, nodes of at most `capacity` keys, cost = sum of the masses
// below each node's top key.
ExactCost brute_force_packing(const BinaryTreeShape& shape, const SearchWeights& weights, std::size_t capacity) {
    const std::size_t key_count = weights.keys.size();
    std::vector<KeyIndex> parent(key_count, no_key);
    std::size_t key = 0;
    for (key = 0; key < key_count; key += 1) {
        if (shape.left[key] != no_key) {
            parent[static_cast<std::size_t>(shape.left[key])] = static_cast<KeyIndex>(key);
        }
        if (shape.right[key] != no_key) {
            parent[static_cast<std::size_t>(shape.right[key])] = static_cast<KeyIndex>(key);
        }
    }
    // Mass below each key: its own weight, its subtree's, its missing children's gaps.
    std::vector<ExactCost> mass(key_count, 0);
    for (key = 0; key < key_count; key += 1) {
        // Every key k adds p_k and its absent-child gaps to itself and each ancestor.
        ExactCost own = static_cast<ExactCost>(weights.keys[key]);
        if (shape.left[key] == no_key) {
            own += static_cast<ExactCost>(weights.gaps[key]);
        }
        if (shape.right[key] == no_key) {
            own += static_cast<ExactCost>(weights.gaps[key + 1]);
        }
        KeyIndex up = static_cast<KeyIndex>(key);
        while (up != no_key) {
            mass[static_cast<std::size_t>(up)] += own;
            up = parent[static_cast<std::size_t>(up)];
        }
    }
    // Non-root keys, each with an edge to its parent: bit set = edge kept.
    std::vector<std::size_t> children;
    for (key = 0; key < key_count; key += 1) {
        if (parent[key] != no_key) {
            children.push_back(key);
        }
    }
    bool have_best = false;
    ExactCost best = 0;
    const std::uint64_t subsets = static_cast<std::uint64_t>(1) << children.size();
    std::uint64_t subset = 0;
    for (subset = 0; subset < subsets; subset += 1) {
        // Top of each key's node: follow kept edges upwards.
        std::vector<std::size_t> top(key_count, 0);
        std::vector<std::size_t> node_size(key_count, 0);
        std::vector<bool> kept(key_count, false);
        std::size_t bit = 0;
        for (bit = 0; bit < children.size(); bit += 1) {
            kept[children[bit]] = ((subset >> bit) & 1) != 0;
        }
        for (key = 0; key < key_count; key += 1) {
            std::size_t up = key;
            while (kept[up]) {
                up = static_cast<std::size_t>(parent[up]);
            }
            top[key] = up;
            node_size[up] += 1;
        }
        bool fits = true;
        ExactCost cost = 0;
        for (key = 0; key < key_count; key += 1) {
            if (node_size[key] > capacity) {
                fits = false;
            }
            if (top[key] == key) {
                cost += mass[key];
            }
        }
        if (fits && (!have_best || cost < best)) {
            best = cost;
            have_best = true;
        }
    }
    return best;
}

void check_packing(SplitMix64& random) {
    std::size_t checked = 0;
    std::size_t trial = 0;
    for (trial = 0; trial < 300; trial += 1) {
        const std::size_t key_count = static_cast<std::size_t>(random.below(13));
        const bool skewed = (trial & 1) != 0;
        const SearchWeights weights = random_weights(key_count, random, skewed);
        const TreeConstruction construction = (trial % 3 == 0) ? TreeConstruction::weight_balanced : TreeConstruction::knuth;
        const SearchTree tree = OptimalTreeBuilder::build(weights, construction);
        std::size_t capacity = 0;
        for (capacity = 1; capacity <= 4; capacity += 1) {
            const PackedTree packed = TreePacker::pack(tree.shape, weights, capacity);
            expect_packing_shape(packed, key_count, capacity);
            TestFailure::expect(walked_cost(packed, weights) == packed.cost, "packing", "walked cost differs");
            if (capacity == 1) {
                TestFailure::expect(packed.cost == tree.cost, "packing", "capacity 1 is not the tree's cost");
            }
            const std::uint64_t slack_percent = random.below(3) * 10;
            const PackedTree laid = ColdSubtreeLayout::relayout(packed, weights, capacity, slack_percent);
            if (slack_percent == 0) {
                TestFailure::expect(laid.cost <= packed.cost, "relayout", "slack 0 costs more than the packing");
            }
            expect_packing_shape(laid, key_count, capacity);
            TestFailure::expect(walked_cost(laid, weights) == laid.cost, "relayout", "walked cost differs");
            if (key_count > 0) {
                const ExactCost best = brute_force_packing(tree.shape, weights, capacity);
                TestFailure::expect(packed.cost == best, "packing", "not the best packing");
            }
            checked += 1;
        }
    }
    for (trial = 0; trial < 6; trial += 1) {
        const std::size_t key_count = 2000 + static_cast<std::size_t>(random.below(3000));
        const SearchWeights weights = random_weights(key_count, random, true);
        const SearchTree tree = OptimalTreeBuilder::knuth(weights);
        const std::size_t capacity = 1 + static_cast<std::size_t>(random.below(TreePacker::maximum_capacity));
        const PackedTree packed = TreePacker::pack(tree.shape, weights, capacity);
        expect_packing_shape(packed, key_count, capacity);
        TestFailure::expect(walked_cost(packed, weights) == packed.cost, "packing large", "walked cost differs");
        const PackedTree laid = ColdSubtreeLayout::relayout(packed, weights, capacity, 0);
        expect_packing_shape(laid, key_count, capacity);
        TestFailure::expect(walked_cost(laid, weights) == laid.cost, "relayout large", "walked cost differs");
        TestFailure::expect(laid.cost <= packed.cost, "relayout large", "slack 0 costs more than the packing");
        checked += 1;
    }
    std::printf("packing: %zu packings checked\n", checked);
}

template <typename Key>
void check_map(const std::vector<Key>& keys, const std::vector<Key>& absent, const SearchWeights& weights,
               TreeConstruction construction, const char* label) {
    using Map = OptimalMap<Key, std::uint64_t>;
    using Query = typename Map::Query;
    using Traits = typename Map::Traits;
    std::vector<std::uint64_t> values(keys.size(), 0);
    std::size_t value_index = 0;
    for (value_index = 0; value_index < keys.size(); value_index += 1) {
        values[value_index] = 3 * value_index + 7;
    }
    const std::uint32_t slack_percents[] = {0, 2, 50};
    for (const std::uint32_t slack_percent : slack_percents) {
        const OptimalMapPlan plan{.tree = construction, .count_layout_slack_percent = slack_percent};
        const Map map = Map::build(keys, values, weights, plan);
        TestFailure::expect(map.size() == keys.size(), label, "size");
        std::vector<Query> queries;
        std::size_t index = 0;
        for (index = 0; index < keys.size(); index += 1) {
            const std::uint64_t* found = map.find(Traits::query_of(keys[index]));
            TestFailure::expect(found != nullptr && *found == values[index], label, "key not found");
            queries.push_back(Traits::query_of(keys[index]));
        }
        for (index = 0; index < absent.size(); index += 1) {
            const std::uint64_t* found = map.find(Traits::query_of(absent[index]));
            TestFailure::expect(found == nullptr, label, "absent key found");
            queries.push_back(Traits::query_of(absent[index]));
        }
        std::vector<const std::uint64_t*> results(queries.size(), nullptr);
        map.find_many(std::span<const Query>(queries.data(), queries.size()),
                      std::span<const std::uint64_t*>(results.data(), results.size()));
        for (index = 0; index < queries.size(); index += 1) {
            TestFailure::expect(results[index] == map.find(queries[index]), label, "find_many differs from find");
        }
        if (!keys.empty()) {
            TestFailure::expect(map.stats().packed_visits <= map.stats().tree_cost, label, "packing cost above tree cost");
        }
    }
}

std::vector<std::uint64_t> sorted_unique_u64(std::size_t count, SplitMix64& random, std::uint64_t spread_mask) {
    std::vector<std::uint64_t> keys;
    keys.reserve(count);
    while (keys.size() < count) {
        keys.push_back(random.next() & spread_mask);
        if (keys.size() == count) {
            std::sort(keys.begin(), keys.end());
            keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
        }
    }
    return keys;
}

std::vector<std::uint64_t> u64_absent(const std::vector<std::uint64_t>& keys, SplitMix64& random) {
    std::vector<std::uint64_t> absent;
    std::size_t index = 0;
    for (index = 0; index < keys.size(); index += 7) {
        const std::uint64_t above = keys[index] + 1;
        if (!std::binary_search(keys.begin(), keys.end(), above)) {
            absent.push_back(above);
        }
        const std::uint64_t below = keys[index] - 1;
        if (!std::binary_search(keys.begin(), keys.end(), below)) {
            absent.push_back(below);
        }
    }
    for (index = 0; index < 200; index += 1) {
        const std::uint64_t value = random.next();
        if (!std::binary_search(keys.begin(), keys.end(), value)) {
            absent.push_back(value);
        }
    }
    absent.push_back(0);
    absent.push_back(~static_cast<std::uint64_t>(0));
    std::vector<std::uint64_t> filtered;
    for (index = 0; index < absent.size(); index += 1) {
        if (!std::binary_search(keys.begin(), keys.end(), absent[index])) {
            filtered.push_back(absent[index]);
        }
    }
    return filtered;
}

void check_u64_maps(SplitMix64& random) {
    const std::size_t sizes[] = {0, 1, 2, 47, 48, 49, 300, 5000, 60000};
    // Full range, and keys crowded into a narrow range (long fence prefixes,
    // many equal windows at the root).
    const std::uint64_t masks[] = {~static_cast<std::uint64_t>(0), 0x0000'0000'000f'ffffULL};
    std::size_t checked = 0;
    for (const std::size_t count : sizes) {
        for (const std::uint64_t mask : masks) {
            const std::vector<std::uint64_t> keys = sorted_unique_u64(count, random, mask);
            const std::vector<std::uint64_t> absent = u64_absent(keys, random);
            const bool skewed = (checked & 1) != 0;
            const SearchWeights weights = random_weights(keys.size(), random, skewed);
            check_map(keys, absent, weights, TreeConstruction::weight_balanced, "u64 weight_balanced");
            check_map(keys, absent, weights, TreeConstruction::balanced, "u64 balanced");
            check_map(keys, absent, weights, TreeConstruction::bottom_up, "u64 bottom_up");
            if (keys.size() <= 5000) {
                check_map(keys, absent, weights, TreeConstruction::knuth, "u64 knuth");
                check_map(keys, absent, weights, TreeConstruction::knuth_scattered, "u64 knuth_scattered");
                SearchWeights gaps_only = weights;
                gaps_only.keys.assign(weights.keys.size(), 0);
                check_map(keys, absent, gaps_only, TreeConstruction::garsia_wachs, "u64 garsia_wachs");
            }
            checked += 1;
        }
    }
    std::printf("u64 maps: %zu key sets\n", checked);
}

void check_i64_map(SplitMix64& random) {
    std::vector<std::int64_t> keys;
    std::int64_t value = -5000000;
    while (keys.size() < 4000) {
        keys.push_back(value);
        value += 1 + static_cast<std::int64_t>(random.below(5000));
    }
    std::vector<std::int64_t> absent;
    std::size_t index = 0;
    for (index = 0; index + 1 < keys.size(); index += 5) {
        if (keys[index] + 1 != keys[index + 1]) {
            absent.push_back(keys[index] + 1);
        }
    }
    absent.push_back(keys.front() - 1);
    absent.push_back(keys.back() + 1);
    const SearchWeights weights = random_weights(keys.size(), random, true);
    check_map(keys, absent, weights, TreeConstruction::knuth, "i64 knuth");
    std::printf("i64 map: %zu keys\n", keys.size());
}

void check_string_map(SplitMix64& random) {
    std::vector<std::string> keys;
    std::size_t index = 0;
    for (index = 0; index < 3000; index += 1) {
        // Long shared prefixes, then a few distinguishing bytes, and some
        // keys that are prefixes of others.
        std::string key = "https://example.org/catalog/item/";
        key += std::to_string(random.below(100000));
        if ((index % 5) == 0) {
            key += "/details";
        }
        keys.push_back(key);
    }
    keys.push_back("");
    keys.push_back("a");
    keys.push_back(std::string("a\0b", 3));
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    std::vector<std::string> absent;
    for (index = 0; index < keys.size(); index += 9) {
        std::string longer = keys[index];
        longer.push_back('\0');
        absent.push_back(longer);
        std::string changed = keys[index];
        changed += "x";
        absent.push_back(changed);
    }
    absent.push_back("b");
    absent.push_back(std::string(1, '\0'));
    std::vector<std::string> filtered;
    for (index = 0; index < absent.size(); index += 1) {
        if (!std::binary_search(keys.begin(), keys.end(), absent[index])) {
            filtered.push_back(absent[index]);
        }
    }
    const SearchWeights weights = random_weights(keys.size(), random, true);
    check_map(keys, filtered, weights, TreeConstruction::knuth, "string knuth");
    check_map(keys, filtered, weights, TreeConstruction::weight_balanced, "string balanced");
    std::printf("string map: %zu keys, %zu absent\n", keys.size(), filtered.size());
}

void check_rejections() {
    std::vector<std::uint64_t> keys{1, 3, 2};
    std::vector<std::uint64_t> values{1, 2, 3};
    SearchWeights weights;
    weights.keys.assign(3, 1);
    weights.gaps.assign(4, 1);
    bool threw = false;
    try {
        const OptimalMap<std::uint64_t, std::uint64_t> map =
            OptimalMap<std::uint64_t, std::uint64_t>::build(keys, values, weights, OptimalMapPlan{});
        (void)map;
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    TestFailure::expect(threw, "rejections", "unsorted keys accepted");
}

} // namespace

int main() {
    SplitMix64 random(0x0b57'0a11'2026ULL);
    check_rejections();
    check_packing(random);
    check_u64_maps(random);
    check_i64_map(random);
    check_string_map(random);
    std::printf("optimal map tests passed\n");
    return 0;
}
