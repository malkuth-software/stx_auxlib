#pragma once

// stx_btree::BBTreeMap: the ordered map threads share, a C++ form of
// bbtree's `BBTreeMap` by Xiangpeng Hao (https://github.com/XiangpengHao/bbtree).
//
// No reference, guard or handle outlives a call: operations that expose an
// entry run the caller's visitor while the entry's leaf is latched, or hand
// out a copy (bbtree 09-concurrency.md). A visitor must not call back into
// the same map. Entries are single-entry atomic; nothing spans entries.
//
// Owner operations (clear, for_each_mut) need the map to be used by no
// other thread meanwhile.

#include "stx_btree/detail/batch.hpp"
#include "stx_btree/detail/latched_tree.hpp"
#include "stx_btree/inline_key.hpp"
#include "stx_btree/page_allocator.hpp"

#include <array>
#include <cstddef>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>

namespace stx_btree {

template <typename Key, typename Value, typename Allocator = GlobalPageAllocator>
class BBTreeMap final {
public:
    using Traits = InlineKeyTraits<Key>;
    using Query = typename Traits::Query;
    using Shape = detail::TreeShape<Key, Value, true>;
    using Tree = detail::LatchedTree<Shape, Allocator>;
    using Bound = typename Tree::Bound;

    struct Entry final {
        Key key;
        Value value;
    };

    BBTreeMap() noexcept = default;
    explicit BBTreeMap(const Allocator& allocator) noexcept : tree(allocator) {}
    BBTreeMap(const BBTreeMap&) = delete;
    BBTreeMap& operator=(const BBTreeMap&) = delete;
    ~BBTreeMap() = default;

    // --- Reads ---------------------------------------------------------------

    // A copy of the value, read without a latch where the leaf settles the
    // key; for trivially copyable values.
    [[nodiscard]] std::optional<Value> get_copied(const Query& query) const noexcept {
        alignas(Value) std::byte storage[sizeof(Value)];
        Value* const value = reinterpret_cast<Value*>(storage);
        const bool found = tree.get_copied(query, value);
        if (!found) {
            return std::nullopt;
        }
        return *std::launder(value);
    }

    // Runs visitor(const Key&, const Value&) on the entry, its leaf latched
    // shared; returns whether the key is present.
    template <typename Visitor>
    bool read(const Query& query, Visitor&& visitor) const {
        return tree.read(query, visitor);
    }

    [[nodiscard]] std::optional<Value> get_cloned(const Query& query) const {
        CloneValue clone;
        const bool found = tree.read(query, clone);
        if (!found) {
            return std::nullopt;
        }
        return std::move(clone.copy);
    }

    [[nodiscard]] bool contains(const Query& query) const {
        IgnoreEntry ignore;
        return tree.read(query, ignore);
    }

    [[nodiscard]] bool empty() const noexcept {
        return tree.is_empty();
    }

    // Copies of `count` keys' values, looked up together: each key descends
    // in its own lane, so their waits for memory overlap (D53, D55).
    // Results come in input order.
    template <std::size_t Count>
    [[nodiscard]] std::array<std::optional<Value>, Count> get_many_copied(const std::array<Query, Count>& queries) const noexcept {
        std::array<std::optional<Value>, Count> results{};
        detail::LatchedBatch<Shape, Allocator>::get_many_copied(tree, queries.data(), results.data(), Count);
        return results;
    }

    // --- Writes --------------------------------------------------------------

    // Inserts, or replaces the value of an equal key and returns the old
    // one. Throws std::bad_alloc when a page is needed and none is given;
    // the map is then unchanged.
    std::optional<Value> insert(Key key, Value value) {
        ReplaceValue policy;
        return tree.insert(std::move(key), std::move(value), policy);
    }

    // Inserts `Count` entries in input order, as `insert` would one by one:
    // each key's lane finds its leaf first, overlapping their waits for
    // memory, then each insert takes its leaf by compare-and-swap at the
    // version its lane read; a leaf another thread changed, or a full one,
    // sends that entry through a single insert (D55).
    template <std::size_t Count>
    std::array<std::optional<Value>, Count> insert_many(std::array<Entry, Count> entries) {
        std::array<std::optional<std::optional<Value>>, Count> outputs{};
        ReplaceValue policy;
        detail::LatchedBatch<Shape, Allocator>::template insert_many<Entry, Count>(tree, entries.data(), outputs.data(), policy);
        std::array<std::optional<Value>, Count> results{};
        std::size_t index = 0;
        for (index = 0; index < Count; index += 1) {
            results[index] = std::move(*outputs[index]);
        }
        return results;
    }

    // Inserts unless an equal key is present; returns whether it did.
    bool try_insert(Key key, Value value) {
        KeepPresent policy;
        return tree.insert(std::move(key), std::move(value), policy);
    }

    // Runs merge(const Key&, Value& stored, Value&& incoming) on a present
    // entry, or inserts; returns whether it inserted.
    template <typename Merge>
    bool update_or_insert(Key key, Value value, Merge&& merge) {
        MergeOrInsert<Merge> policy{merge};
        return tree.insert(std::move(key), std::move(value), policy);
    }

    // Inserts unless present, then runs visitor(const Key&, const Value&)
    // on the entry the map keeps, latched exclusively.
    template <typename Visitor>
    void get_or_insert(Key key, Value value, Visitor&& visitor) {
        ReadOrInsert<Visitor> policy{visitor};
        tree.insert(std::move(key), std::move(value), policy);
    }

    // Runs visitor(const Key&, Value&) on the entry, latched exclusively.
    template <typename Visitor>
    bool update(const Query& query, Visitor&& visitor) {
        return tree.update(query, visitor);
    }

    std::optional<Entry> remove(const Query& query) {
        AlwaysRemove condition;
        return remove_if(query, condition);
    }

    // Removes the entry only if condition(const Key&, const Value&) holds.
    template <typename Condition>
    std::optional<Entry> remove_if(const Query& query, Condition&& condition) {
        detail::EntryBuffer<Key, Value> removed;
        const bool found = tree.remove_if(query, condition, removed);
        if (!found) {
            return std::nullopt;
        }
        return Entry{std::move(removed.key()), std::move(removed.value())};
    }

    // --- Scans ---------------------------------------------------------------

    // visitor(const Key&, const Value&) -> bool on entries from `start` to
    // `end` in key order, until it returns false. Bounds with a null key are
    // unbounded. Not a snapshot: an entry present throughout is visited once.
    template <typename Visitor>
    void scan(Bound start, Bound end, Visitor&& visitor) const {
        tree.template scan<true>(start, end, visitor);
    }

    template <typename Visitor>
    void scan_rev(Bound start, Bound end, Visitor&& visitor) const {
        tree.template scan<false>(end, start, visitor);
    }

    // --- Owner ---------------------------------------------------------------

    void clear() noexcept {
        tree.destroy();
    }

    // visitor(const Key&, Value&) on every entry in key order.
    template <typename Visitor>
    void for_each_mut(Visitor&& visitor) {
        typename detail::TreeWalk<Shape>::Cursor cursor;
        detail::TreeWalk<Shape>::first(tree.load_root(), cursor);
        while (cursor.stack.is_attached()) {
            const detail::EntryPointers<Key, Value> entry = detail::TreeWalk<Shape>::entry_at(cursor);
            visitor(static_cast<const Key&>(*entry.key), *entry.value);
            detail::TreeWalk<Shape>::advance(cursor);
        }
    }

    [[nodiscard]] const Tree& tree_for_inspection() const noexcept {
        return tree;
    }

private:
    struct CloneValue final {
        std::optional<Value> copy;
        void operator()(const Key&, const Value& value) {
            copy.emplace(value);
        }
    };
    struct IgnoreEntry final {
        void operator()(const Key&, const Value&) const noexcept {}
    };
    struct AlwaysRemove final {
        bool operator()(const Key&, const Value&) const noexcept {
            return true;
        }
    };
    struct ReplaceValue final {
        using Output = std::optional<Value>;
        Output occupied(const Key&, Value& stored, Key&&, Value&& value) {
            Output old(std::move(stored));
            stored = std::move(value);
            return old;
        }
        Output vacant(const Key&, const Value&) noexcept {
            return std::nullopt;
        }
    };
    struct KeepPresent final {
        using Output = bool;
        Output occupied(const Key&, Value&, Key&&, Value&&) noexcept {
            return false;
        }
        Output vacant(const Key&, const Value&) noexcept {
            return true;
        }
    };
    template <typename Merge>
    struct MergeOrInsert final {
        using Output = bool;
        Merge& merge;
        Output occupied(const Key& stored_key, Value& stored, Key&&, Value&& value) {
            merge(stored_key, stored, std::move(value));
            return false;
        }
        Output vacant(const Key&, const Value&) noexcept {
            return true;
        }
    };
    template <typename Visitor>
    struct ReadOrInsert final {
        using Output = bool;
        Visitor& visitor;
        Output occupied(const Key& stored_key, Value& stored, Key&&, Value&&) {
            visitor(stored_key, static_cast<const Value&>(stored));
            return false;
        }
        Output vacant(const Key& key, const Value& value) {
            visitor(key, value);
            return true;
        }
    };

    Tree tree;
};

} // namespace stx_btree
