// Tests of stx_btree::BBTreeMap, the map threads share: a single-thread
// differential against std::map with the structural invariant check, then
// threaded tests in the shape of bbtree's map/tests.rs: writers whose keys
// readers watch appear, threads inserting and removing their own keys around
// one leaf (the root raised and lowered over and over), counters updated by
// many threads, scans of steady keys while others change, batched copies
// while the tree changes, and string keys whose shared prefix shrinks while
// other threads append.

#include "stx_btree/bbtree_map.hpp"
#include "stx_btree/pooled_page_allocator.hpp"
#include "test_support.hpp"
#include "tree_invariants.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using stx_btree_tests::KeyMaker;
using stx_btree_tests::SplitMix64;
using stx_btree_tests::TestFailure;

template <typename Map>
void check_structure(const Map& map, std::size_t entries, const char* label) {
    using Shape = typename Map::Shape;
    stx_btree_tests::TreeInvariants<Shape> invariants;
    const typename stx_btree_tests::TreeInvariants<Shape>::Report report =
        invariants.check(map.tree_for_inspection().load_root(), entries);
    TestFailure::expect(report.inexact_prefixes == 0, label, "prefixes are exact");
}

// Collects a scan's keys, up to a limit.
template <typename Key, typename Value>
struct CollectKeys final {
    std::vector<Key>* keys = nullptr;
    std::size_t limit = 0;
    bool operator()(const Key& key, const Value&) {
        (*keys).push_back(key);
        return (*keys).size() < limit;
    }
};

struct AddInto final {
    template <typename Key>
    void operator()(const Key&, std::uint64_t& stored, std::uint64_t&& incoming) const noexcept {
        stored += incoming;
    }
};

template <typename Key>
struct VisitAll final {
    typename std::map<Key, std::uint64_t>::iterator* expected = nullptr;
    std::size_t* visited = nullptr;
    const char* label = "";
    void operator()(const Key& key, std::uint64_t& value) {
        TestFailure::expect((**expected).first == key && (**expected).second == value, label, "owner walk order");
        ++(*expected);
        *visited += 1;
    }
};

template <typename Key, typename Allocator = stx_btree::GlobalPageAllocator>
class SingleThread final {
public:
    using Map = stx_btree::BBTreeMap<Key, std::uint64_t, Allocator>;
    using Traits = stx_btree::InlineKeyTraits<Key>;
    using Query = typename Traits::Query;
    using Reference = std::map<Key, std::uint64_t>;

    static void run(const char* label, std::uint64_t seed, std::size_t operations, std::size_t key_space) {
        Map map;
        Reference reference;
        SplitMix64 random(seed);
        KeyMaker<Key> keys(seed);
        std::size_t step = 0;
        for (step = 0; step < operations; step += 1) {
            const std::uint64_t choice = random.below(100);
            const Key key = keys.make(random.below(key_space));
            const std::uint64_t value = random.next();
            if (choice < 35) {
                insert(map, reference, key, value, label);
            } else if (choice < 42) {
                update_or_insert(map, reference, key, value, label);
            } else if (choice < 65) {
                remove(map, reference, key, label);
            } else if (choice < 85) {
                look_up(map, reference, key, label);
            } else if (choice < 95) {
                scan_both_ways(map, reference, key, choice % 2 == 0, label);
            } else {
                batch(map, reference, keys, random, key_space, label);
            }
            if (step % 4096 == 0) {
                check_structure(map, reference.size(), label);
            }
        }
        check_structure(map, reference.size(), label);
        std::size_t visited = 0;
        typename Reference::iterator expected = reference.begin();
        VisitAll<Key> visit_all{&expected, &visited, label};
        map.for_each_mut(visit_all);
        TestFailure::expect(visited == reference.size(), label, "owner walk visits every entry");
        std::printf("%-26s ok (%zu entries at the end)\n", label, reference.size());
    }

private:
    static void insert(Map& map, Reference& reference, const Key& key, std::uint64_t value, const char* label) {
        const std::optional<std::uint64_t> old = map.insert(key, value);
        const typename Reference::iterator found = reference.find(key);
        if (found == reference.end()) {
            TestFailure::expect(!old.has_value(), label, "insert of a new key");
            reference.emplace(key, value);
        } else {
            TestFailure::expect(old.has_value() && *old == (*found).second, label, "insert replaces");
            (*found).second = value;
        }
    }

    static void update_or_insert(Map& map, Reference& reference, const Key& key, std::uint64_t value, const char* label) {
        const bool inserted = map.update_or_insert(key, value, AddInto{});
        const typename Reference::iterator found = reference.find(key);
        TestFailure::expect(inserted == (found == reference.end()), label, "update_or_insert");
        if (found == reference.end()) {
            reference.emplace(key, value);
        } else {
            (*found).second += value;
        }
    }

    static void remove(Map& map, Reference& reference, const Key& key, const char* label) {
        const Query query = Traits::query_of(key);
        const std::optional<typename Map::Entry> removed = map.remove(query);
        const typename Reference::iterator found = reference.find(key);
        if (found == reference.end()) {
            TestFailure::expect(!removed.has_value(), label, "remove of an absent key");
        } else {
            TestFailure::expect(removed.has_value() && (*removed).value == (*found).second, label, "remove");
            reference.erase(found);
        }
    }

    static void look_up(Map& map, Reference& reference, const Key& key, const char* label) {
        const Query query = Traits::query_of(key);
        const std::optional<std::uint64_t> copied = map.get_copied(query);
        const std::optional<std::uint64_t> cloned = map.get_cloned(query);
        const bool contained = map.contains(query);
        const typename Reference::iterator found = reference.find(key);
        if (found == reference.end()) {
            TestFailure::expect(!copied.has_value() && !cloned.has_value() && !contained, label, "absent lookups");
        } else {
            TestFailure::expect(copied.has_value() && *copied == (*found).second, label, "get_copied");
            TestFailure::expect(cloned.has_value() && *cloned == (*found).second, label, "get_cloned");
            TestFailure::expect(contained, label, "contains");
        }
    }

    static void scan_both_ways(Map& map, Reference& reference, const Key& key, bool inclusive, const char* label) {
        const Query query = Traits::query_of(key);
        std::vector<Key> scanned;
        CollectKeys<Key, std::uint64_t> collect{&scanned, 40};
        const typename Map::Bound start{&query, inclusive};
        const typename Map::Bound unbounded{nullptr, true};
        map.scan(start, unbounded, collect);
        typename Reference::iterator expected = inclusive ? reference.lower_bound(key) : reference.upper_bound(key);
        std::size_t index = 0;
        for (index = 0; index < scanned.size(); index += 1) {
            TestFailure::expect(expected != reference.end() && (*expected).first == scanned[index], label, "scan order");
            ++expected;
        }
        TestFailure::expect(scanned.size() == 40 || expected == reference.end(), label, "scan reaches the end");

        std::vector<Key> backward;
        CollectKeys<Key, std::uint64_t> collect_back{&backward, 25};
        const typename Map::Bound high{&query, true};
        map.scan_rev(unbounded, high, collect_back);
        typename Reference::iterator upper = reference.upper_bound(key);
        for (index = 0; index < backward.size(); index += 1) {
            TestFailure::expect(upper != reference.begin(), label, "reverse scan stays in range");
            --upper;
            TestFailure::expect((*upper).first == backward[index], label, "reverse scan order");
        }
        TestFailure::expect(backward.size() == 25 || upper == reference.begin(), label, "reverse scan reaches the start");
    }

    static void batch(Map& map, Reference& reference, KeyMaker<Key>& keys, SplitMix64& random, std::size_t key_space, const char* label) {
        std::array<Key, 8> batch_keys;
        std::array<Query, 8> batch_queries;
        std::size_t index = 0;
        for (index = 0; index < 8; index += 1) {
            batch_keys[index] = keys.make(random.below(key_space));
        }
        for (index = 0; index < 8; index += 1) {
            batch_queries[index] = Traits::query_of(batch_keys[index]);
        }
        const std::array<std::optional<std::uint64_t>, 8> copies = map.get_many_copied(batch_queries);
        for (index = 0; index < 8; index += 1) {
            const typename Reference::iterator found = reference.find(batch_keys[index]);
            const bool present = found != reference.end();
            TestFailure::expect(copies[index].has_value() == present, label, "batched presence");
            if (present) {
                TestFailure::expect(*copies[index] == (*found).second, label, "batched value");
            }
        }
    }
};

// --- Threaded tests ---------------------------------------------------------

using U64Map = stx_btree::BBTreeMap<std::uint64_t, std::uint64_t>;

// A value tied to its key, so a reader can tell a copy of the wrong entry.
class KeyedValue final {
public:
    [[nodiscard]] static std::uint64_t of(std::uint64_t key) noexcept {
        return SplitMix64::mix(key ^ 0xfeedULL);
    }
};

// Inserts its own keys in order, publishing how far it got.
struct OrderedWriter final {
    U64Map* map = nullptr;
    std::uint64_t thread_index = 0;
    std::uint64_t count = 0;
    std::uint64_t threads = 0;
    std::atomic<std::uint64_t>* progress = nullptr;
    void operator()() const {
        std::uint64_t step = 0;
        for (step = 0; step < count; step += 1) {
            const std::uint64_t key = SplitMix64::mix(step * threads + thread_index);
            (*map).insert(key, KeyedValue::of(key));
            progress[thread_index].store(step + 1, std::memory_order_release);
        }
    }
};

// Checks that every key a writer published is found with its own value,
// by single and batched copies.
struct ProgressReader final {
    const U64Map* map = nullptr;
    std::uint64_t threads = 0;
    std::atomic<std::uint64_t>* progress = nullptr;
    std::atomic<bool>* stop = nullptr;
    std::uint64_t seed = 0;
    void operator()() const {
        SplitMix64 random(seed);
        while (!(*stop).load(std::memory_order_acquire)) {
            const std::uint64_t writer = random.below(threads);
            const std::uint64_t done = progress[writer].load(std::memory_order_acquire);
            if (done == 0) {
                continue;
            }
            std::array<std::uint64_t, 16> keys{};
            std::size_t index = 0;
            for (index = 0; index < keys.size(); index += 1) {
                const std::uint64_t step = random.below(done);
                keys[index] = SplitMix64::mix(step * threads + writer);
                const std::optional<std::uint64_t> value = (*map).get_copied(keys[index]);
                TestFailure::expect(value.has_value() && *value == KeyedValue::of(keys[index]), "readers", "a published key is found");
            }
            const std::array<std::optional<std::uint64_t>, 16> copies = (*map).get_many_copied(keys);
            for (index = 0; index < keys.size(); index += 1) {
                TestFailure::expect(copies[index].has_value() && *copies[index] == KeyedValue::of(keys[index]), "readers", "batched copies");
            }
        }
    }
};

void writers_and_readers() {
    const std::uint64_t writers = 4;
    const std::uint64_t readers = 4;
    const std::uint64_t count = 150000;
    U64Map map;
    std::atomic<std::uint64_t> progress[4];
    std::size_t index = 0;
    for (index = 0; index < writers; index += 1) {
        progress[index].store(0, std::memory_order_relaxed);
    }
    std::atomic<bool> stop{false};
    std::vector<std::thread> threads;
    for (index = 0; index < readers; index += 1) {
        threads.emplace_back(ProgressReader{&map, writers, progress, &stop, 100 + index});
    }
    std::vector<std::thread> writing;
    for (index = 0; index < writers; index += 1) {
        writing.emplace_back(OrderedWriter{&map, index, count, writers, progress});
    }
    for (index = 0; index < writing.size(); index += 1) {
        writing[index].join();
    }
    stop.store(true, std::memory_order_release);
    for (index = 0; index < threads.size(); index += 1) {
        threads[index].join();
    }
    check_structure(map, writers * count, "writers and readers");
    std::printf("%-26s ok\n", "writers and readers");
}

// Inserts and removes its own keys at random against a model of its own.
struct ModelThread final {
    U64Map* map = nullptr;
    std::uint64_t thread_index = 0;
    std::uint64_t threads = 0;
    std::uint64_t key_space = 0;
    std::uint64_t operations = 0;
    std::map<std::uint64_t, std::uint64_t>* model = nullptr;
    void operator()() const {
        SplitMix64 random(thread_index * 7919 + 1);
        std::uint64_t step = 0;
        for (step = 0; step < operations; step += 1) {
            const std::uint64_t key = random.below(key_space) * threads + thread_index;
            const std::uint64_t choice = random.below(10);
            if (choice < 5) {
                const std::uint64_t value = random.next();
                const std::optional<std::uint64_t> old = (*map).insert(key, value);
                const bool had = (*model).count(key) != 0;
                TestFailure::expect(old.has_value() == had, "models", "insert sees the model");
                (*model)[key] = value;
            } else if (choice < 8) {
                const std::optional<U64Map::Entry> removed = (*map).remove(key);
                const bool had = (*model).count(key) != 0;
                TestFailure::expect(removed.has_value() == had, "models", "remove sees the model");
                if (had) {
                    TestFailure::expect((*removed).value == (*model)[key], "models", "removed value");
                    (*model).erase(key);
                }
            } else {
                const std::optional<std::uint64_t> value = (*map).get_copied(key);
                const std::map<std::uint64_t, std::uint64_t>::iterator found = (*model).find(key);
                if (found == (*model).end()) {
                    TestFailure::expect(!value.has_value(), "models", "absent key");
                } else {
                    TestFailure::expect(value.has_value() && *value == (*found).second, "models", "present key");
                }
            }
        }
    }
};

void models(const char* label, std::uint64_t key_space, std::uint64_t operations) {
    const std::uint64_t thread_count = 8;
    U64Map map;
    std::vector<std::map<std::uint64_t, std::uint64_t>> per_thread(thread_count);
    std::vector<std::thread> threads;
    std::size_t index = 0;
    for (index = 0; index < thread_count; index += 1) {
        threads.emplace_back(ModelThread{&map, index, thread_count, key_space, operations, &per_thread[index]});
    }
    for (index = 0; index < threads.size(); index += 1) {
        threads[index].join();
    }
    std::size_t total = 0;
    for (index = 0; index < thread_count; index += 1) {
        total += per_thread[index].size();
        std::map<std::uint64_t, std::uint64_t>::iterator entry = per_thread[index].begin();
        while (entry != per_thread[index].end()) {
            const std::optional<std::uint64_t> value = map.get_copied((*entry).first);
            TestFailure::expect(value.has_value() && *value == (*entry).second, label, "final contents");
            ++entry;
        }
    }
    check_structure(map, total, label);
    std::printf("%-26s ok (%zu entries)\n", label, total);
}

// Adds to shared counters, half through update_or_insert.
struct Counter final {
    U64Map* map = nullptr;
    std::uint64_t counters = 0;
    std::uint64_t increments = 0;
    std::uint64_t seed = 0;
    void operator()() const {
        SplitMix64 random(seed);
        std::uint64_t step = 0;
        for (step = 0; step < increments; step += 1) {
            const std::uint64_t key = SplitMix64::mix(random.below(counters));
            (*map).update_or_insert(key, 1, AddInto{});
        }
    }
};

struct SumValues final {
    std::uint64_t* total = nullptr;
    void operator()(const std::uint64_t&, std::uint64_t& value) const noexcept {
        *total += value;
    }
};

void counters() {
    const std::uint64_t thread_count = 8;
    const std::uint64_t increments = 60000;
    U64Map map;
    std::vector<std::thread> threads;
    std::size_t index = 0;
    for (index = 0; index < thread_count; index += 1) {
        threads.emplace_back(Counter{&map, 3000, increments, 31 + index});
    }
    for (index = 0; index < threads.size(); index += 1) {
        threads[index].join();
    }
    std::uint64_t total = 0;
    map.for_each_mut(SumValues{&total});
    TestFailure::expect(total == thread_count * increments, "counters", "no increment is lost");
    std::printf("%-26s ok\n", "counters");
}

// Scans the steady keys (even) while others insert and remove odd keys:
// every steady key is visited exactly once, in order.
struct SteadyScanner final {
    const U64Map* map = nullptr;
    std::uint64_t steady = 0;
    std::atomic<bool>* stop = nullptr;
    bool reverse = false;
    void operator()() const {
        while (!(*stop).load(std::memory_order_acquire)) {
            std::vector<std::uint64_t> keys;
            CollectKeys<std::uint64_t, std::uint64_t> collect{&keys, static_cast<std::size_t>(-1)};
            const U64Map::Bound unbounded{nullptr, true};
            if (reverse) {
                (*map).scan_rev(unbounded, unbounded, collect);
            } else {
                (*map).scan(unbounded, unbounded, collect);
            }
            std::uint64_t seen = 0;
            std::size_t index = 0;
            std::uint64_t previous = 0;
            for (index = 0; index < keys.size(); index += 1) {
                if (index > 0) {
                    const bool ordered = reverse ? keys[index] < previous : keys[index] > previous;
                    TestFailure::expect(ordered, "scans", "scan order under change");
                }
                previous = keys[index];
                if (keys[index] % 2 == 0) {
                    seen += 1;
                }
            }
            TestFailure::expect(seen == steady, "scans", "every steady key once");
        }
    }
};

struct OddChurner final {
    U64Map* map = nullptr;
    std::uint64_t range = 0;
    std::uint64_t operations = 0;
    std::uint64_t seed = 0;
    void operator()() const {
        SplitMix64 random(seed);
        std::uint64_t step = 0;
        for (step = 0; step < operations; step += 1) {
            const std::uint64_t key = 2 * random.below(range) + 1;
            if (random.below(2) == 0) {
                (*map).insert(key, key);
            } else {
                (*map).remove(key);
            }
        }
    }
};

void scans_under_change() {
    const std::uint64_t steady = 20000;
    U64Map map;
    std::uint64_t key = 0;
    for (key = 0; key < steady; key += 1) {
        map.insert(2 * key, 2 * key);
    }
    std::atomic<bool> stop{false};
    std::vector<std::thread> scanners;
    scanners.emplace_back(SteadyScanner{&map, steady, &stop, false});
    scanners.emplace_back(SteadyScanner{&map, steady, &stop, true});
    std::vector<std::thread> churners;
    std::size_t index = 0;
    for (index = 0; index < 4; index += 1) {
        churners.emplace_back(OddChurner{&map, steady, 120000, 77 + index});
    }
    for (index = 0; index < churners.size(); index += 1) {
        churners[index].join();
    }
    stop.store(true, std::memory_order_release);
    for (index = 0; index < scanners.size(); index += 1) {
        scanners[index].join();
    }
    std::printf("%-26s ok\n", "scans under change");
}

// String keys: appends of a common head while another thread inserts keys
// that shrink the shared prefix.
using StringMap = stx_btree::BBTreeMap<std::string, std::uint64_t>;

struct StringAppender final {
    StringMap* map = nullptr;
    std::uint64_t thread_index = 0;
    std::uint64_t count = 0;
    void operator()() const {
        KeyMaker<std::string> keys(thread_index);
        std::uint64_t step = 0;
        for (step = 0; step < count; step += 1) {
            std::string key = std::string("tenant/") + std::to_string(thread_index) + "/" + keys.make_ordered(step);
            (*map).insert(std::move(key), step);
        }
    }
};

struct PrefixShrinker final {
    StringMap* map = nullptr;
    std::uint64_t count = 0;
    void operator()() const {
        KeyMaker<std::string> keys(99);
        std::uint64_t step = 0;
        for (step = 0; step < count; step += 1) {
            (*map).insert(keys.make(step), step);
        }
    }
};

struct CountU64 final {
    std::size_t* total = nullptr;
    void operator()(const std::uint64_t&, std::uint64_t&) const noexcept {
        *total += 1;
    }
};

struct CountEntries final {
    std::size_t* total = nullptr;
    void operator()(const std::string&, std::uint64_t&) const noexcept {
        *total += 1;
    }
};

void string_prefixes_under_appends() {
    StringMap map;
    std::vector<std::thread> threads;
    std::size_t index = 0;
    for (index = 0; index < 4; index += 1) {
        threads.emplace_back(StringAppender{&map, index, 40000});
    }
    threads.emplace_back(PrefixShrinker{&map, 40000});
    for (index = 0; index < threads.size(); index += 1) {
        threads[index].join();
    }
    for (index = 0; index < 4; index += 1) {
        KeyMaker<std::string> keys(index);
        std::uint64_t step = 0;
        for (step = 0; step < 40000; step += 997) {
            const std::string key = std::string("tenant/") + std::to_string(index) + "/" + keys.make_ordered(step);
            const std::optional<std::uint64_t> value = map.get_copied(key);
            TestFailure::expect(value.has_value() && *value == step, "string prefixes", "appended key found");
        }
    }
    // The shrinking thread's random keys may repeat; the appenders' don't.
    std::set<std::string> shrinker_keys;
    KeyMaker<std::string> shrinker(99);
    std::uint64_t made = 0;
    for (made = 0; made < 40000; made += 1) {
        shrinker_keys.insert(shrinker.make(made));
    }
    std::size_t total = 0;
    CountEntries counter{&total};
    map.for_each_mut(counter);
    TestFailure::expect(total == 4 * 40000 + shrinker_keys.size(), "string prefixes", "every distinct key is present");
    check_structure(map, total, "string prefixes");
    std::printf("%-26s ok (%zu entries)\n", "string prefixes", total);
}

// Batched inserts against inserts one at a time, single-threaded, past the
// three-level batch threshold; then threads batching their own keys while
// others look them up.
template <typename Key, typename Allocator = stx_btree::GlobalPageAllocator>
void shared_insert_many(const char* label, std::uint64_t seed, bool base_shares_prefix) {
    using Map = stx_btree::BBTreeMap<Key, std::uint64_t, Allocator>;
    Map map;
    std::map<Key, std::uint64_t> reference;
    SplitMix64 random(seed);
    KeyMaker<Key> keys(seed);
    std::uint64_t index = 0;
    for (index = 0; index < 150000; index += 1) {
        const Key key = base_shares_prefix ? keys.make_ordered(index) : keys.make(index);
        map.insert(key, index);
        reference[key] = index;
    }
    std::size_t round = 0;
    for (round = 0; round < 3000; round += 1) {
        std::array<typename Map::Entry, 16> entries{};
        std::size_t lane = 0;
        const bool ascending = round % 50 == 0;
        for (lane = 0; lane < 16; lane += 1) {
            const std::uint64_t pick = ascending ? 2000000 + round * 16 + lane : random.below(220000);
            const std::uint64_t key_index = (lane == 9 && !ascending) ? entries[3].value : pick;
            entries[lane].key = ascending ? keys.make_ordered(pick) : keys.make(key_index);
            entries[lane].value = key_index;
        }
        std::array<typename Map::Entry, 16> copies = entries;
        const std::array<std::optional<std::uint64_t>, 16> results = map.insert_many(std::move(entries));
        for (lane = 0; lane < 16; lane += 1) {
            const typename std::map<Key, std::uint64_t>::iterator found = reference.find(copies[lane].key);
            if (found == reference.end()) {
                TestFailure::expect(!results[lane].has_value(), label, "a batched new key returns nothing");
                reference.emplace(copies[lane].key, copies[lane].value);
            } else {
                TestFailure::expect(results[lane].has_value() && *results[lane] == (*found).second, label, "a batched replacement");
                (*found).second = copies[lane].value;
            }
        }
        if (round % 1000 == 0) {
            check_structure(map, reference.size(), label);
        }
    }
    check_structure(map, reference.size(), label);
    std::size_t visited = 0;
    typename std::map<Key, std::uint64_t>::iterator expected = reference.begin();
    VisitAll<Key> visit_all{&expected, &visited, label};
    map.for_each_mut(visit_all);
    TestFailure::expect(visited == reference.size(), label, "every batched entry is there");
    std::printf("%-26s ok\n", label);
}

struct BatchWriter final {
    U64Map* map = nullptr;
    std::uint64_t thread_index = 0;
    std::uint64_t batches = 0;
    std::uint64_t threads = 0;
    std::atomic<std::uint64_t>* progress = nullptr;
    void operator()() const {
        std::uint64_t round = 0;
        for (round = 0; round < batches; round += 1) {
            std::array<U64Map::Entry, 16> entries{};
            std::size_t lane = 0;
            for (lane = 0; lane < 16; lane += 1) {
                const std::uint64_t step = round * 16 + lane;
                const std::uint64_t key = SplitMix64::mix(step * threads + thread_index);
                entries[lane].key = key;
                entries[lane].value = KeyedValue::of(key);
            }
            (*map).insert_many(std::move(entries));
            progress[thread_index].store((round + 1) * 16, std::memory_order_release);
        }
    }
};

void batch_writers_and_readers() {
    const std::uint64_t writers = 4;
    U64Map map;
    std::uint64_t seed_key = 0;
    for (seed_key = 0; seed_key < 120000; seed_key += 1) {
        const std::uint64_t key = SplitMix64::mix(seed_key) | 1;
        map.insert(key, KeyedValue::of(key));
    }
    std::atomic<std::uint64_t> progress[4];
    std::size_t index = 0;
    for (index = 0; index < writers; index += 1) {
        progress[index].store(0, std::memory_order_relaxed);
    }
    std::atomic<bool> stop{false};
    std::vector<std::thread> readers;
    for (index = 0; index < 4; index += 1) {
        readers.emplace_back(ProgressReader{&map, writers, progress, &stop, 300 + index});
    }
    std::vector<std::thread> writing;
    for (index = 0; index < writers; index += 1) {
        writing.emplace_back(BatchWriter{&map, index, 6000, writers, progress});
    }
    for (index = 0; index < writing.size(); index += 1) {
        writing[index].join();
    }
    stop.store(true, std::memory_order_release);
    for (index = 0; index < readers.size(); index += 1) {
        readers[index].join();
    }
    std::size_t total = 0;
    std::uint64_t sum = 0;
    map.for_each_mut(SumValues{&sum});
    CountU64 counter{&total};
    map.for_each_mut(counter);
    check_structure(map, total, "batch writers and readers");
    std::printf("%-26s ok (%zu entries)\n", "batch writers and readers", total);
}

} // namespace

int main() {
    SingleThread<std::uint64_t>::run("shared u64", 11, 200000, 40000);
    SingleThread<std::uint64_t>::run("shared u64 one leaf", 12, 60000, 80);
    SingleThread<std::array<std::uint8_t, 16>>::run("shared uuid", 13, 150000, 40000);
    SingleThread<std::string>::run("shared string", 14, 150000, 30000);
    writers_and_readers();
    models("models, many leaves", 20000, 60000);
    models("models, about a leaf", 12, 60000);
    counters();
    scans_under_change();
    string_prefixes_under_appends();
    shared_insert_many<std::uint64_t>("shared batches u64", 41, false);
    shared_insert_many<std::string>("shared batches string", 42, false);
    shared_insert_many<std::uint64_t>("shared batches, prefix", 43, true);
    shared_insert_many<std::string>("shared batches, str prefix", 44, true);
    batch_writers_and_readers();
    SingleThread<std::uint64_t, stx_btree::PooledPageAllocator>::run("pooled shared u64", 51, 200000, 40000);
    SingleThread<std::string, stx_btree::PooledPageAllocator>::run("pooled shared string", 52, 150000, 30000);
    shared_insert_many<std::uint64_t, stx_btree::PooledPageAllocator>("pooled shared batches", 53, false);
    std::printf("shared map tests passed\n");
    return 0;
}
