// A timed stress run of the shared map (bbtree's 12-thread release stress
// run found races the model tests had missed). Threads insert, remove and
// update keys at random while others copy single and batched values, read
// with visitors and scan both ways. Every value is tied to its key, so a
// read of a torn, stale or foreign entry is caught; every scan must ascend
// (or descend) strictly. Shapes: about one leaf (the root raised and
// lowered over and over), two levels, and deep trees of 12-entry leaves.
//
//   stx_btree_stress_test [seconds per shape]   (default 2)

#include "stx_btree/bbtree_map.hpp"
#include "stx_btree/pooled_page_allocator.hpp"
#include "test_support.hpp"
#include "tree_invariants.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using stx_btree_tests::KeyMaker;
using stx_btree_tests::SplitMix64;
using stx_btree_tests::TestFailure;

// A 64-byte value whose every word is derived from its key.
struct Payload final {
    std::uint64_t words[8];

    [[nodiscard]] static Payload of(std::uint64_t key) noexcept {
        Payload payload{};
        std::size_t index = 0;
        for (index = 0; index < 8; index += 1) {
            payload.words[index] = SplitMix64::mix(key + index);
        }
        return payload;
    }
    [[nodiscard]] bool belongs_to(std::uint64_t key) const noexcept {
        std::size_t index = 0;
        for (index = 0; index < 8; index += 1) {
            if (words[index] != SplitMix64::mix(key + index)) {
                return false;
            }
        }
        return true;
    }
};

using PayloadMap = stx_btree::BBTreeMap<std::uint64_t, Payload>;
using PooledPayloadMap = stx_btree::BBTreeMap<std::uint64_t, Payload, stx_btree::PooledPageAllocator>;

struct RewritePayload final {
    void operator()(const std::uint64_t& key, Payload& value) const noexcept {
        value = Payload::of(key);
    }
};

struct CheckPayload final {
    void operator()(const std::uint64_t& key, const Payload& value) const {
        TestFailure::expect(value.belongs_to(key), "stress", "a visitor sees its own entry");
    }
};

struct OrderedScan final {
    bool ascending = true;
    bool started = false;
    std::uint64_t previous = 0;
    std::size_t visited = 0;
    bool operator()(const std::uint64_t& key, const Payload& value) {
        TestFailure::expect(value.belongs_to(key), "stress", "a scanned value is its key's");
        if (started) {
            const bool ordered = ascending ? key > previous : key < previous;
            TestFailure::expect(ordered, "stress", "a scan is strictly ordered");
        }
        started = true;
        previous = key;
        visited += 1;
        return visited < 200;
    }
};

enum class Role : std::uint8_t { churn, read, scan };

template <typename Map>
struct StressThread final {
    Map* map = nullptr;
    Role role = Role::churn;
    std::uint64_t key_space = 0;
    std::uint64_t seed = 0;
    std::atomic<bool>* stop = nullptr;
    std::atomic<std::uint64_t>* operations = nullptr;

    void operator()() const {
        SplitMix64 random(seed);
        std::uint64_t done = 0;
        while (!(*stop).load(std::memory_order_relaxed)) {
            const std::uint64_t key = random.below(key_space);
            if (role == Role::churn) {
                churn(random, key);
            } else if (role == Role::read) {
                read(random, key);
            } else {
                scan(random, key);
            }
            done += 1;
        }
        (*operations).fetch_add(done, std::memory_order_relaxed);
    }

    void churn(SplitMix64& random, std::uint64_t key) const {
        const std::uint64_t choice = random.below(10);
        if (choice < 5) {
            (*map).insert(key, Payload::of(key));
        } else if (choice < 9) {
            const std::optional<typename Map::Entry> removed = (*map).remove(key);
            if (removed.has_value()) {
                TestFailure::expect((*removed).key == key && (*removed).value.belongs_to(key), "stress", "a removed entry is whole");
            }
        } else {
            (*map).update(key, RewritePayload{});
        }
    }

    void read(SplitMix64& random, std::uint64_t key) const {
        const std::uint64_t choice = random.below(4);
        if (choice == 0) {
            const std::optional<Payload> copy = (*map).get_copied(key);
            if (copy.has_value()) {
                TestFailure::expect((*copy).belongs_to(key), "stress", "a copy is of its own key");
            }
        } else if (choice == 1) {
            (*map).read(key, CheckPayload{});
        } else {
            std::array<std::uint64_t, 16> keys{};
            std::size_t index = 0;
            for (index = 0; index < keys.size(); index += 1) {
                keys[index] = random.below(key_space);
            }
            const std::array<std::optional<Payload>, 16> copies = (*map).get_many_copied(keys);
            for (index = 0; index < keys.size(); index += 1) {
                if (copies[index].has_value()) {
                    TestFailure::expect((*copies[index]).belongs_to(keys[index]), "stress", "a batched copy is of its own key");
                }
            }
        }
    }

    void scan(SplitMix64& random, std::uint64_t key) const {
        const bool ascending = random.below(2) == 0;
        OrderedScan visitor{ascending, false, 0, 0};
        const typename Map::Bound bound{&key, random.below(2) == 0};
        const typename Map::Bound unbounded{nullptr, true};
        if (ascending) {
            (*map).scan(bound, unbounded, visitor);
        } else {
            (*map).scan_rev(unbounded, bound, visitor);
        }
    }
};

struct CountAll final {
    std::size_t* total = nullptr;
    void operator()(const std::uint64_t& key, Payload& value) const {
        TestFailure::expect(value.belongs_to(key), "stress", "the final contents are whole");
        *total += 1;
    }
};

template <typename Map>
void run_shape(const char* label, std::uint64_t key_space, double seconds) {
    Map map;
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> operations{0};
    const Role roles[10] = {Role::churn, Role::churn, Role::churn, Role::churn, Role::read,
                            Role::read, Role::read, Role::scan, Role::scan, Role::churn};
    std::vector<std::thread> threads;
    std::size_t index = 0;
    for (index = 0; index < 10; index += 1) {
        threads.emplace_back(StressThread<Map>{&map, roles[index], key_space, 1000 + index, &stop, &operations});
    }
    const std::chrono::duration<double> duration(seconds);
    std::this_thread::sleep_for(duration);
    stop.store(true, std::memory_order_relaxed);
    for (index = 0; index < threads.size(); index += 1) {
        threads[index].join();
    }
    std::size_t total = 0;
    map.for_each_mut(CountAll{&total});
    stx_btree_tests::TreeInvariants<typename Map::Shape> invariants;
    const typename stx_btree_tests::TreeInvariants<typename Map::Shape>::Report report =
        invariants.check(map.tree_for_inspection().load_root(), total);
    TestFailure::expect(report.inexact_prefixes == 0, label, "prefixes are exact after the run");
    std::printf("%-28s ok: %llu operations, %zu entries, %zu internal nodes at the end\n", label,
                static_cast<unsigned long long>(operations.load()), total, report.internals);
}

} // namespace

int main(int argument_count, char** arguments) {
    const double seconds = argument_count > 1 ? std::atof(arguments[1]) : 2.0;
    run_shape<PayloadMap>("stress, about one leaf", 24, seconds);
    run_shape<PayloadMap>("stress, two levels", 3000, seconds);
    run_shape<PayloadMap>("stress, deep 12-entry leaves", 200000, seconds);
    run_shape<PooledPayloadMap>("stress pooled, about one leaf", 24, seconds);
    run_shape<PooledPayloadMap>("stress pooled, deep leaves", 200000, seconds);
    std::printf("stress tests passed\n");
    return 0;
}
