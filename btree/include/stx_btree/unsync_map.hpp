#pragma once

// stx_btree::unsync::BBTreeMap: the single-threaded ordered map, a C++ form
// of bbtree's `unsync::BBTreeMap` by Xiangpeng Hao
// (https://github.com/XiangpengHao/bbtree).
//
// Keys need an InlineKeyTraits specialization (std::uint64_t, std::int64_t,
// std::array<std::uint8_t, N> and std::string are provided). Every pointer,
// reference and iterator into the map is invalidated by any change to it.

#include "stx_btree/detail/batch.hpp"
#include "stx_btree/detail/unsync_tree.hpp"
#include "stx_btree/inline_key.hpp"
#include "stx_btree/page_allocator.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <utility>

namespace stx_btree::unsync {

template <typename Key, typename Value, typename Allocator = GlobalPageAllocator>
class BBTreeMap final {
public:
    using Traits = InlineKeyTraits<Key>;
    using Query = typename Traits::Query;
    using Shape = detail::TreeShape<Key, Value, false>;
    using Tree = detail::UnsyncTree<Shape, Allocator>;

    // An entry taken out of the map.
    struct Entry final {
        Key key;
        Value value;
    };

    // An entry in place.
    struct EntryView final {
        const Key& key;
        Value& value;
    };

    // Ordered traversal, both directions. Invalidated by any change.
    class Iterator final {
    public:
        [[nodiscard]] EntryView operator*() const noexcept {
            const detail::EntryPointers<Key, Value> entry = (*tree).entry_at_cursor(cursor);
            return EntryView{*entry.key, *entry.value};
        }
        Iterator& operator++() noexcept {
            (*tree).advance(cursor);
            return *this;
        }
        Iterator& operator--() noexcept {
            if (!cursor.stack.is_attached()) {
                (*tree).last(cursor);
                return *this;
            }
            (*tree).retreat(cursor);
            return *this;
        }
        [[nodiscard]] bool operator==(const Iterator& other) const noexcept {
            const bool attached = cursor.stack.is_attached();
            if (attached != other.cursor.stack.is_attached()) {
                return false;
            }
            if (!attached) {
                return true;
            }
            return cursor.stack.node() == other.cursor.stack.node() && cursor.slot == other.cursor.slot;
        }
        [[nodiscard]] bool operator!=(const Iterator& other) const noexcept {
            const bool equal = *this == other;
            return !equal;
        }

    private:
        friend class BBTreeMap;
        explicit Iterator(const Tree* source) noexcept : tree(source) {
            cursor.stack.detach();
        }
        const Tree* tree = nullptr;
        typename Tree::Cursor cursor;
    };

    BBTreeMap() noexcept = default;
    explicit BBTreeMap(const Allocator& allocator) noexcept : tree(allocator) {}
    BBTreeMap(const BBTreeMap&) = delete;
    BBTreeMap& operator=(const BBTreeMap&) = delete;
    BBTreeMap(BBTreeMap&&) noexcept = default;
    BBTreeMap& operator=(BBTreeMap&&) noexcept = default;
    ~BBTreeMap() = default;

    [[nodiscard]] std::size_t size() const noexcept {
        return tree.size();
    }
    [[nodiscard]] bool empty() const noexcept {
        return tree.size() == 0;
    }

    [[nodiscard]] Value* find(const Query& query) noexcept {
        const detail::EntryPointers<Key, Value> entry = tree.find(query);
        return entry.value;
    }
    [[nodiscard]] const Value* find(const Query& query) const noexcept {
        const detail::EntryPointers<Key, Value> entry = tree.find(query);
        return entry.value;
    }
    // Value pointers for `Count` keys looked up together: each key descends
    // in its own lane, so their waits for memory overlap (D53). Small trees
    // look keys up one at a time.
    template <std::size_t Count>
    [[nodiscard]] std::array<Value*, Count> get_many(const std::array<Query, Count>& queries) noexcept {
        std::array<Value*, Count> results{};
        const detail::NodePointer root = tree.root();
        if (Count < 2 || root.is_null() || root.is_leaf() || !detail::BatchCutoff<Shape>::batches(tree.size())) {
            std::size_t index = 0;
            for (index = 0; index < Count; index += 1) {
                results[index] = find(queries[index]);
            }
            return results;
        }
        detail::UnsyncBatch<Shape>::find_many(root, queries.data(), results.data(), Count);
        return results;
    }

    [[nodiscard]] bool contains(const Query& query) const noexcept {
        const detail::EntryPointers<Key, Value> entry = tree.find(query);
        return entry.value != nullptr;
    }

    // Inserts, or replaces the value of an equal key and returns the old
    // value; the key already present stays (as bbtree's and Rust std's
    // `insert`).
    std::optional<Value> insert(Key key, Value value) {
        return tree.insert_replacing(std::move(key), std::move(value));
    }

    // Inserts `Count` entries as `insert` would one by one, with the same
    // results and the same tree, their descents overlapped in lanes (D53).
    template <std::size_t Count>
    std::array<std::optional<Value>, Count> insert_many(std::array<Entry, Count> entries) {
        std::array<std::optional<Value>, Count> results{};
        tree.template insert_many<Entry, Count>(entries.data(), results.data());
        return results;
    }

    // Inserts unless an equal key is present; returns whether it inserted.
    // On false, key and value are destroyed unused.
    bool try_insert(Key key, Value value) {
        typename Tree::InsertOutcome outcome = tree.insert(std::move(key), std::move(value));
        return outcome.inserted;
    }

    std::optional<Entry> remove(const Query& query) noexcept {
        detail::EntryBuffer<Key, Value> removed;
        const bool found = tree.erase(query, removed);
        if (!found) {
            return std::nullopt;
        }
        return Entry{std::move(removed.key()), std::move(removed.value())};
    }

    std::optional<Entry> pop_first() noexcept {
        detail::EntryBuffer<Key, Value> removed;
        const bool found = tree.pop_first(removed);
        if (!found) {
            return std::nullopt;
        }
        return Entry{std::move(removed.key()), std::move(removed.value())};
    }

    std::optional<Entry> pop_last() noexcept {
        detail::EntryBuffer<Key, Value> removed;
        const bool found = tree.pop_last(removed);
        if (!found) {
            return std::nullopt;
        }
        return Entry{std::move(removed.key()), std::move(removed.value())};
    }

    void clear() noexcept {
        tree.clear();
    }

    [[nodiscard]] Iterator begin() const noexcept {
        Iterator iterator(&tree);
        tree.first(iterator.cursor);
        return iterator;
    }
    [[nodiscard]] Iterator end() const noexcept {
        Iterator iterator(&tree);
        return iterator;
    }
    // The first entry whose key is not below the query.
    [[nodiscard]] Iterator lower_bound(const Query& query) const noexcept {
        Iterator iterator(&tree);
        tree.lower_bound(iterator.cursor, query);
        return iterator;
    }

    // For the tests' invariant checker.
    [[nodiscard]] const Tree& tree_for_inspection() const noexcept {
        return tree;
    }

private:
    Tree tree;
};

} // namespace stx_btree::unsync
