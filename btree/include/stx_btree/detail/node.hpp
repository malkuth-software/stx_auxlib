#pragma once

// Node pages and the operations of one node (bbtree tree/node/). A node
// handle is a page address with no access of its own: the code holding it
// has the access of the operation it is part of (D14).
//
// Invariants every operation keeps (bbtree tree/node/mod.rs):
//  1. A node's length is at most its capacity. A leaf's keys and values
//     0..length are constructed; an internal node's windows 0..length and
//     edges 0..=length are written.
//  2. Every edge is the only pointer to a distinct node one level down, and
//     its low bit says whether that node is a leaf (D52). Every leaf lies at
//     the same depth.
//  3. A leaf's head key and head value are constructed exactly when its
//     header says it has a head; every leaf but the leftmost has one (D17).
//  4. Inline key i is key i's window at the node's prefix length; in an
//     internal node, separator i's, whose key is the head of the leftmost
//     leaf right of it (D49). Slots length.. hold all ones, so a search may
//     count every slot (05-fence-key.md, "Narrow slots").
//  5. A leaf with hash tags keeps each live key's tag in the matching byte.
//
// Inline keys and prefix lengths steer searches only: every index a search
// returns is in bounds whatever they hold.

#include "stx_btree/detail/inlining.hpp"
#include "stx_btree/detail/node_layout.hpp"
#include "stx_btree/detail/vector_count.hpp"
#include "stx_btree/inline_key.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include "stx_auxlib/process.hpp"

#include <cstdlib>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>

namespace stx_btree::detail {

// The first eight bytes of every page.
struct NodeHeader final {
    std::uint16_t length = 0;
    std::uint8_t has_head = 0;
    std::uint8_t reserved = 0;
    // What every key of the node shares: its fences' common prefix, or on
    // the tree's borders what every key shares (05-fence-key.md, D50).
    std::uint16_t prefix_length = 0;
    std::uint16_t reserved_tail = 0;
};
static_assert(sizeof(NodeHeader) == 8, "the header precedes 8-aligned inline keys");

inline constexpr std::size_t latch_word_offset = 8;
inline constexpr std::size_t maximum_prefix_length = 65535;

template <std::size_t Bytes>
struct SlotOfWidth;
template <>
struct SlotOfWidth<2> final {
    using Type = std::uint16_t;
};
template <>
struct SlotOfWidth<4> final {
    using Type = std::uint32_t;
};
template <>
struct SlotOfWidth<8> final {
    using Type = std::uint64_t;
};

// A key's window at a prefix length: the eight encoding bytes after the
// prefix, of which a node keeps its width (05-fence-key.md).
struct Window final {
    std::size_t prefix_length = 0;
    InlineWord word = 0;

    // The same key's window at fill's shorter prefix: this window's bytes
    // move down by the difference, under fill's leading bytes, which every
    // key of the node shares (bbtree Window::moved_back_to).
    [[nodiscard]] Window moved_back_to(Window fill) const noexcept {
        const std::size_t shift = prefix_length > fill.prefix_length ? prefix_length - fill.prefix_length : 0;
        InlineWord moved = word;
        if (shift >= inline_window_bytes) {
            moved = fill.word;
        } else if (shift != 0) {
            const unsigned shift_bits = static_cast<unsigned>(8 * shift);
            const InlineWord filled_bits = ~(~static_cast<InlineWord>(0) >> shift_bits);
            moved = (fill.word & filled_bits) | (word >> shift_bits);
        }
        return Window{.prefix_length = fill.prefix_length, .word = moved};
    }
};

// A pointer to a node page whose lowest bit says whether the node is a
// leaf: pages are aligned to at least 512 bytes, so the bit is free (D52).
class NodePointer final {
public:
    static constexpr std::uintptr_t leaf_tag = 1;

    std::uintptr_t tagged = 0;

    [[nodiscard]] static NodePointer leaf_at(std::byte* page) noexcept {
        const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(page);
        return NodePointer{address | leaf_tag};
    }
    [[nodiscard]] static NodePointer internal_at(std::byte* page) noexcept {
        const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(page);
        return NodePointer{address};
    }
    [[nodiscard]] bool is_leaf() const noexcept {
        return (tagged & leaf_tag) != 0;
    }
    [[nodiscard]] bool is_null() const noexcept {
        return tagged == 0;
    }
    [[nodiscard]] std::byte* page() const noexcept {
        const std::uintptr_t address = tagged & ~leaf_tag;
        return reinterpret_cast<std::byte*>(address);
    }
    [[nodiscard]] bool operator==(const NodePointer& other) const noexcept {
        return tagged == other.tagged;
    }
};

// Header access. A latched tree's readers load length and prefix racing a
// writer, so its header fields are accessed as relaxed atomics on both
// sides; an unlatched tree's are plain.
template <bool Latched>
class HeaderAccess final {
public:
    [[nodiscard]] static NodeHeader* header(std::byte* page) noexcept {
        return std::launder(reinterpret_cast<NodeHeader*>(page));
    }
    [[nodiscard]] static std::size_t length(std::byte* page) noexcept {
        NodeHeader* fields = header(page);
        if constexpr (Latched) {
            const std::atomic_ref<std::uint16_t> field((*fields).length);
            return field.load(std::memory_order_relaxed);
        } else {
            return (*fields).length;
        }
    }
    static void set_length(std::byte* page, std::size_t length) noexcept {
        NodeHeader* fields = header(page);
        const std::uint16_t narrow = static_cast<std::uint16_t>(length);
        if constexpr (Latched) {
            const std::atomic_ref<std::uint16_t> field((*fields).length);
            field.store(narrow, std::memory_order_relaxed);
        } else {
            (*fields).length = narrow;
        }
    }
    [[nodiscard]] static std::size_t prefix_length(std::byte* page) noexcept {
        NodeHeader* fields = header(page);
        if constexpr (Latched) {
            const std::atomic_ref<std::uint16_t> field((*fields).prefix_length);
            return field.load(std::memory_order_relaxed);
        } else {
            return (*fields).prefix_length;
        }
    }
    static void set_prefix_length(std::byte* page, std::size_t prefix_length) noexcept {
        NodeHeader* fields = header(page);
        const std::uint16_t narrow = static_cast<std::uint16_t>(prefix_length);
        if constexpr (Latched) {
            const std::atomic_ref<std::uint16_t> field((*fields).prefix_length);
            field.store(narrow, std::memory_order_relaxed);
        } else {
            (*fields).prefix_length = narrow;
        }
    }
    [[nodiscard]] static bool has_head(std::byte* page) noexcept {
        NodeHeader* fields = header(page);
        if constexpr (Latched) {
            const std::atomic_ref<std::uint8_t> field((*fields).has_head);
            return field.load(std::memory_order_relaxed) != 0;
        } else {
            return (*fields).has_head != 0;
        }
    }
    static void set_has_head(std::byte* page, bool has_head) noexcept {
        NodeHeader* fields = header(page);
        const std::uint8_t narrow = has_head ? 1 : 0;
        if constexpr (Latched) {
            const std::atomic_ref<std::uint8_t> field((*fields).has_head);
            field.store(narrow, std::memory_order_relaxed);
        } else {
            (*fields).has_head = narrow;
        }
    }
};

// Everything decided per key type, value type and sharing mode: the layouts,
// slot widths and the encoding (bbtree's NodePlan, D47), fixed at compile
// time.
template <typename Key, typename Value, bool Latched>
struct TreeShape final {
    using KeyType = Key;
    using ValueType = Value;
    using Traits = InlineKeyTraits<Key>;
    using Query = typename Traits::Query;

    static constexpr bool latched = Latched;
    static constexpr std::size_t header_bytes = Latched ? 16 : 8;
    static constexpr std::size_t leaf_width = static_cast<std::size_t>(Traits::leaf_width);
    static constexpr std::size_t internal_width = static_cast<std::size_t>(Traits::internal_width);
    static constexpr LeafLayout leaf = LayoutPlanner::leaf(
        header_bytes, leaf_width, Traits::has_hash_tags,
        sizeof(Key), alignof(Key), sizeof(Value), alignof(Value));
    static constexpr InternalLayout internal = LayoutPlanner::internal(header_bytes, internal_width);
    static constexpr RootPageLayout root = LayoutPlanner::root_page(leaf, internal);

    using LeafSlot = typename SlotOfWidth<leaf_width>::Type;
    using InternalSlot = typename SlotOfWidth<internal_width>::Type;

    // Windows an internal node hands to a leaf are read again from their
    // keys where internal nodes keep fewer bytes (D43).
    static constexpr bool rehydrates_windows = internal_width < leaf_width;
    static constexpr bool trivially_relocatable =
        std::is_trivially_copyable_v<Key> && std::is_trivially_copyable_v<Value>;

    static_assert(std::is_nothrow_move_constructible_v<Key>, "keys move inside nodes without failing");
    static_assert(std::is_nothrow_move_constructible_v<Value>, "values move inside nodes without failing");
    static_assert(std::is_nothrow_destructible_v<Key> && std::is_nothrow_destructible_v<Value>,
                  "entries are destroyed inside node operations");
    static_assert(leaf.capacity >= minimum_node_capacity && leaf.capacity <= maximum_node_capacity);
    static_assert(internal.capacity >= minimum_node_capacity && internal.capacity <= maximum_node_capacity);
    static_assert(leaf.page_alignment >= 512 && internal.page_alignment >= 512,
                  "the leaf tag needs the low address bit");
    static_assert(leaf.entries_offset + (leaf.capacity + 1) * leaf.entry_stride <= leaf.page_bytes);
    static_assert(internal.edges_offset + (internal.capacity + 1) * edge_bytes <= internal.page_bytes);

    // Below a node's root: fewer than this many keys is underfull (D22).
    static constexpr std::size_t leaf_minimum = leaf.capacity / 8 > 4 ? leaf.capacity / 8 : 4;
    static constexpr std::size_t internal_minimum = internal.capacity / 8 > 4 ? internal.capacity / 8 : 4;

    [[nodiscard]] static InlineWord window_word(const Query& query, std::size_t prefix_length) noexcept {
        return Traits::window(query, prefix_length);
    }
    [[nodiscard]] static Window window_of_key(const Key& key, std::size_t prefix_length) noexcept {
        const Query query = Traits::query_of(key);
        const InlineWord word = Traits::window(query, prefix_length);
        return Window{.prefix_length = prefix_length, .word = word};
    }
};

// Where the inserted entry went in a split (bbtree InsertionSide).
struct SplitPlace final {
    std::size_t separator_index = 0;
    bool right = false;
    std::size_t index = 0;

    // A split at a node's end leaves the arriving side
    // min(2 * minimum, capacity / 2) entries, so ascending inserts leave full
    // nodes behind; a split in the middle stays even (D32).
    [[nodiscard]] static SplitPlace of(std::size_t capacity, std::size_t insertion_index, std::size_t minimum) noexcept {
        const std::size_t doubled = 2 * minimum;
        const std::size_t half = capacity / 2;
        const std::size_t arriving = doubled < half ? doubled : half;
        std::size_t separator_index = capacity / 2;
        if (insertion_index == 0) {
            separator_index = arriving - 1;
        } else if (insertion_index == capacity) {
            separator_index = capacity - arriving;
        }
        if (insertion_index <= separator_index) {
            return SplitPlace{.separator_index = separator_index, .right = false, .index = insertion_index};
        }
        return SplitPlace{
            .separator_index = separator_index, .right = true,
            .index = insertion_index - separator_index - 1};
    }
};

// Moves runs of objects of one type between slots a fixed stride apart,
// constructing at the destination and destroying the source; overlapping
// runs move in the direction that preserves them.
template <typename Object>
class SlotRelocation final {
public:
    static void move_run(std::byte* from, std::byte* to, std::size_t count, std::size_t stride) noexcept {
        if (count == 0 || from == to) {
            return;
        }
        std::size_t step = 0;
        if (to < from) {
            for (step = 0; step < count; step += 1) {
                move_one(from + step * stride, to + step * stride);
            }
        } else {
            step = count;
            while (step > 0) {
                step -= 1;
                move_one(from + step * stride, to + step * stride);
            }
        }
    }

    static void move_one(std::byte* from, std::byte* to) noexcept {
        Object* source = std::launder(reinterpret_cast<Object*>(from));
        ::new (static_cast<void*>(to)) Object(std::move(*source));
        (*source).~Object();
    }
};

// A leaf (bbtree tree/node/leaf.rs).
template <typename Shape>
class LeafNode final {
public:
    using Key = typename Shape::KeyType;
    using Value = typename Shape::ValueType;
    using Traits = typename Shape::Traits;
    using Query = typename Shape::Query;
    using Slot = typename Shape::LeafSlot;
    using Header = HeaderAccess<Shape::latched>;
    using Count = InlineKeyCount<Slot, Shape::leaf.capacity>;

    static constexpr std::size_t capacity = Shape::leaf.capacity;
    static constexpr std::size_t width = Shape::leaf_width;
    static constexpr std::size_t minimum = Shape::leaf_minimum;
    static constexpr Slot vacant_slot = static_cast<Slot>(~static_cast<Slot>(0));
    static constexpr unsigned slot_shift = static_cast<unsigned>(64 - 8 * width);

    std::byte* page = nullptr;

    [[nodiscard]] static LeafNode at(NodePointer pointer) noexcept {
        return LeafNode{pointer.page()};
    }
    [[nodiscard]] NodePointer pointer() const noexcept {
        return NodePointer::leaf_at(page);
    }

    // Makes the zeroed or emptied page at `page` an empty leaf.
    [[nodiscard]] static LeafNode initialize(std::byte* page, std::size_t prefix_length) noexcept {
        Header::set_length(page, 0);
        Header::set_has_head(page, false);
        Header::set_prefix_length(page, prefix_length);
        const LeafNode leaf = LeafNode{page};
        std::size_t index = 0;
        Slot* slots = leaf.inline_keys();
        for (index = 0; index < capacity; index += 1) {
            slots[index] = vacant_slot;
        }
        return leaf;
    }

    [[nodiscard]] std::size_t length() const noexcept {
        return Header::length(page);
    }
    [[nodiscard]] bool has_head() const noexcept {
        return Header::has_head(page);
    }
    [[nodiscard]] std::size_t prefix_length() const noexcept {
        return Header::prefix_length(page);
    }
    [[nodiscard]] bool is_underfull() const noexcept {
        return length() < minimum;
    }
    [[nodiscard]] bool keeps_its_minimum() const noexcept {
        return length() > minimum;
    }

    [[nodiscard]] Slot* inline_keys() const noexcept {
        return std::launder(reinterpret_cast<Slot*>(page + Shape::leaf.inline_keys_offset));
    }
    [[nodiscard]] std::uint8_t* hash_tags() const noexcept {
        return reinterpret_cast<std::uint8_t*>(page + Shape::leaf.hash_tags_offset);
    }
    // Entry e: 0 is the head's, e = i + 1 key i's.
    [[nodiscard]] std::byte* entry(std::size_t entry_index) const noexcept {
        return page + Shape::leaf.entries_offset + entry_index * Shape::leaf.entry_stride;
    }
    [[nodiscard]] Key& key(std::size_t index) const noexcept {
        return *std::launder(reinterpret_cast<Key*>(entry(index + 1)));
    }
    [[nodiscard]] Value& value(std::size_t index) const noexcept {
        return *std::launder(reinterpret_cast<Value*>(entry(index + 1) + Shape::leaf.value_offset));
    }
    [[nodiscard]] Key& head_key() const noexcept {
        return *std::launder(reinterpret_cast<Key*>(entry(0)));
    }
    [[nodiscard]] Value& head_value() const noexcept {
        return *std::launder(reinterpret_cast<Value*>(entry(0) + Shape::leaf.value_offset));
    }

    [[nodiscard]] static Slot slot_of(InlineWord word) noexcept {
        return static_cast<Slot>(word >> slot_shift);
    }
    [[nodiscard]] static InlineWord word_of(Slot slot) noexcept {
        return static_cast<InlineWord>(slot) << slot_shift;
    }
    [[nodiscard]] Window window(std::size_t index) const noexcept {
        const InlineWord word = word_of(inline_keys()[index]);
        return Window{.prefix_length = prefix_length(), .word = word};
    }

    // Keys [begin, end) whose inline keys equal the query's window here.
    [[nodiscard]] STX_BTREE_ALWAYS_INLINE EqualRange candidates(const Query& query) const noexcept {
        const InlineWord word = Shape::window_word(query, prefix_length());
        const Slot target = slot_of(word);
        return Count::equal_range(inline_keys(), target, length());
    }

    struct SearchResult final {
        bool found = false;
        // The key's index if found, else where it would go.
        std::size_t index = 0;
    };

    // Binary search among candidates, which share the prefix and `width`
    // more bytes with the query.
    [[nodiscard]] STX_BTREE_NEVER_INLINE SearchResult search_candidates(const Query& query, EqualRange range) const noexcept {
        const std::size_t equal_length = prefix_length() + width;
        std::size_t low = range.begin;
        std::size_t high = range.end;
        while (low < high) {
            const std::size_t middle = low + (high - low) / 2;
            const int order = Traits::compare_from(query, key(middle), equal_length);
            if (order == 0) {
                return SearchResult{.found = true, .index = middle};
            }
            if (order < 0) {
                high = middle;
            } else {
                low = middle + 1;
            }
        }
        return SearchResult{.found = false, .index = low};
    }

    [[nodiscard]] STX_BTREE_ALWAYS_INLINE SearchResult search(const Query& query) const noexcept {
        const EqualRange range = candidates(query);
        if (range.begin == range.end) {
            return SearchResult{.found = false, .index = range.begin};
        }
        return search_candidates(query, range);
    }

    static constexpr std::size_t absent = static_cast<std::size_t>(-1);

    // The key's index, or `absent`: no candidate is absent at once, one is
    // compared once, and several are filtered by hash tag before any binary
    // search (D36, D38).
    [[nodiscard]] STX_BTREE_ALWAYS_INLINE std::size_t find(const Query& query) const noexcept {
        const EqualRange range = candidates(query);
        const std::size_t count = range.end - range.begin;
        if (count == 0) {
            return absent;
        }
        const std::size_t equal_length = prefix_length() + width;
        if (count == 1) {
            const bool equal = Traits::equal_from(query, key(range.begin), equal_length);
            return equal ? range.begin : absent;
        }
        if constexpr (Shape::leaf.has_hash_tags) {
            return find_by_tags(query, range, equal_length);
        } else {
            const SearchResult result = search_candidates(query, range);
            return result.found ? result.index : absent;
        }
    }

    [[nodiscard]] STX_BTREE_NEVER_INLINE std::size_t find_by_tags(const Query& query, EqualRange range, std::size_t equal_length) const noexcept {
        const std::uint8_t tag = Traits::hash_tag(query);
        const std::uint8_t* tags = hash_tags();
        std::size_t matches = 0;
        std::size_t lower = 0;
        std::size_t upper = 0;
        std::size_t index = 0;
        for (index = range.begin; index < range.end; index += 1) {
            if (tags[index] == tag) {
                if (matches == 0) {
                    lower = index;
                }
                upper = index;
                matches += 1;
            }
        }
        if (matches == 0) {
            return absent;
        }
        if (matches == 1) {
            const bool equal = Traits::equal_from(query, key(lower), equal_length);
            return equal ? lower : absent;
        }
        if (matches == 2) {
            const int order = Traits::compare_from(query, key(upper), equal_length);
            if (order == 0) {
                return upper;
            }
            if (order > 0) {
                return absent;
            }
            const bool equal = Traits::equal_from(query, key(lower), equal_length);
            return equal ? lower : absent;
        }
        const SearchResult result = search_candidates(query, range);
        return result.found ? result.index : absent;
    }

    // --- Changes. Each needs exclusive access to the leaf. -----------------

    void write_tag(std::size_t index, const Key& stored) const noexcept {
        if constexpr (Shape::leaf.has_hash_tags) {
            const Query query = Traits::query_of(stored);
            hash_tags()[index] = Traits::hash_tag(query);
        }
    }

    // Moves inline keys and tags [from, from + count) to [to, to + count).
    void move_columns(std::size_t from, std::size_t to, std::size_t count) const noexcept {
        if (count == 0) {
            return;
        }
        Slot* slots = inline_keys();
        std::memmove(slots + to, slots + from, count * sizeof(Slot));
        if constexpr (Shape::leaf.has_hash_tags) {
            std::uint8_t* tags = hash_tags();
            std::memmove(tags + to, tags + from, count);
        }
    }

    static void copy_columns(LeafNode from, std::size_t from_index, LeafNode to, std::size_t to_index, std::size_t count) noexcept {
        if (count == 0) {
            return;
        }
        std::memcpy(to.inline_keys() + to_index, from.inline_keys() + from_index, count * sizeof(Slot));
        if constexpr (Shape::leaf.has_hash_tags) {
            std::memcpy(to.hash_tags() + to_index, from.hash_tags() + from_index, count);
        }
    }

    // Moves entries [from, from + count) to [to, to + count) of `target`,
    // which may be this leaf.
    void move_entries(std::size_t from, LeafNode target, std::size_t to, std::size_t count) const noexcept {
        if constexpr (Shape::trivially_relocatable) {
            if (count != 0) {
                std::memmove(target.entry(to), entry(from), count * Shape::leaf.entry_stride);
            }
        } else {
            SlotRelocation<Key>::move_run(entry(from), target.entry(to), count, Shape::leaf.entry_stride);
            SlotRelocation<Value>::move_run(
                entry(from) + Shape::leaf.value_offset, target.entry(to) + Shape::leaf.value_offset,
                count, Shape::leaf.entry_stride);
        }
    }

    // Sets a shorter or equal length; vacated slots get the greatest key.
    void set_length_refilling(std::size_t new_length) const noexcept {
        const std::size_t old_length = length();
        std::size_t index = 0;
        Slot* slots = inline_keys();
        for (index = new_length; index < old_length; index += 1) {
            slots[index] = vacant_slot;
        }
        Header::set_length(page, new_length);
    }

    void construct_entry(std::size_t entry_index, Key&& new_key, Value&& new_value) const noexcept {
        std::byte* slot = entry(entry_index);
        ::new (static_cast<void*>(slot)) Key(std::move(new_key));
        ::new (static_cast<void*>(slot + Shape::leaf.value_offset)) Value(std::move(new_value));
    }

    // Inserts key at index with its window here; needs room.
    void insert_fit(std::size_t index, Key&& new_key, Value&& new_value, InlineWord window_word) const noexcept {
        const std::size_t old_length = length();
        move_entries(index + 1, *this, index + 2, old_length - index);
        move_columns(index, index + 1, old_length - index);
        construct_entry(index + 1, std::move(new_key), std::move(new_value));
        inline_keys()[index] = slot_of(window_word);
        write_tag(index, key(index));
        Header::set_length(page, old_length + 1);
    }

    // Moves key and value at index out into the caller's storage.
    void remove(std::size_t index, Key* key_out, Value* value_out) const noexcept {
        const std::size_t old_length = length();
        SlotRelocation<Key>::move_one(entry(index + 1), reinterpret_cast<std::byte*>(key_out));
        SlotRelocation<Value>::move_one(
            entry(index + 1) + Shape::leaf.value_offset, reinterpret_cast<std::byte*>(value_out));
        move_entries(index + 2, *this, index + 1, old_length - index - 1);
        move_columns(index + 1, index, old_length - index - 1);
        set_length_refilling(old_length - 1);
    }

    // Moves entries past separator_index to `right`, an empty leaf with this
    // prefix; the separator's key and value become right's head. Returns
    // the separator's window here.
    [[nodiscard]] Window split_around(std::size_t separator_index, LeafNode right) const noexcept {
        const std::size_t old_length = length();
        const std::size_t right_length = old_length - separator_index - 1;
        const Window separator = window(separator_index);
        move_entries(separator_index + 1, right, 0, right_length + 1);
        copy_columns(*this, separator_index + 1, right, 0, right_length);
        Header::set_has_head(right.page, true);
        Header::set_length(right.page, right_length);
        set_length_refilling(separator_index);
        return separator;
    }

    // Takes the head and key 0: key 0 becomes the head, its value the head
    // value (bbtree take_head_and_first_key). Returns the successor's window
    // here. The old head key and value move to the caller's storage.
    [[nodiscard]] Window take_head_for_successor(Key* key_out, Value* value_out) const noexcept {
        const std::size_t old_length = length();
        // A leaf right of a separator keeps its minimum of keys (invariant
        // 1, D22); an empty one is a broken tree, which stops here as
        // bbtree's `expect` does rather than shift a wrapped count.
        if (old_length == 0) {
            // stx_auxlib adaptation: the one fault handler.
            stx_auxlib::fail("stx_btree: a leaf right of a separator is empty");
        }
        const Window successor = window(0);
        SlotRelocation<Key>::move_one(entry(0), reinterpret_cast<std::byte*>(key_out));
        SlotRelocation<Value>::move_one(entry(0) + Shape::leaf.value_offset, reinterpret_cast<std::byte*>(value_out));
        move_entries(1, *this, 0, old_length);
        move_columns(1, 0, old_length - 1);
        set_length_refilling(old_length - 1);
        return successor;
    }

    // Moves every window back to fill's shorter prefix (no key is read).
    void move_prefix_back_to(Window fill) const noexcept {
        const std::size_t own_prefix = prefix_length();
        if (own_prefix <= fill.prefix_length) {
            return;
        }
        const std::size_t live = length();
        Slot* slots = inline_keys();
        std::size_t index = 0;
        for (index = 0; index < live; index += 1) {
            const Window own = Window{.prefix_length = own_prefix, .word = word_of(slots[index])};
            const Window moved = own.moved_back_to(fill);
            slots[index] = slot_of(moved.word);
        }
        Header::set_prefix_length(page, fill.prefix_length);
    }

    // Sets a new prefix length, reading every key for its window.
    void refit_prefix(std::size_t new_prefix) const noexcept {
        if (new_prefix == prefix_length()) {
            return;
        }
        const std::size_t live = length();
        Slot* slots = inline_keys();
        std::size_t index = 0;
        for (index = 0; index < live; index += 1) {
            const Query query = Traits::query_of(key(index));
            slots[index] = slot_of(Traits::window(query, new_prefix));
        }
        Header::set_prefix_length(page, new_prefix);
    }

    // Whether the first and last inline keys differ in their first byte: the
    // fences then share no more than the prefix (05-fence-key.md, "Upkeep").
    [[nodiscard]] bool keys_differ_past_prefix() const noexcept {
        const std::size_t live = length();
        if (live == 0) {
            return false;
        }
        const InlineWord first = word_of(inline_keys()[0]);
        const InlineWord last = word_of(inline_keys()[live - 1]);
        return ByteEncoding::leading_zero_bits(first ^ last) < 8;
    }

    // Destroys every constructed key and value, the head's included.
    void destroy_entries() const noexcept {
        if constexpr (!std::is_trivially_destructible_v<Key> || !std::is_trivially_destructible_v<Value>) {
            const std::size_t live = length();
            std::size_t index = 0;
            if (has_head()) {
                head_key().~Key();
                head_value().~Value();
            }
            for (index = 0; index < live; index += 1) {
                key(index).~Key();
                value(index).~Value();
            }
        }
    }
};

// An internal node (bbtree tree/node/internal.rs): windows and edges, no
// keys; separator i's key is the head of the leftmost leaf under edge i + 1.
template <typename Shape>
class InternalNode final {
public:
    using Key = typename Shape::KeyType;
    using Value = typename Shape::ValueType;
    using Traits = typename Shape::Traits;
    using Query = typename Shape::Query;
    using Slot = typename Shape::InternalSlot;
    using Header = HeaderAccess<Shape::latched>;
    using Count = InlineKeyCount<Slot, Shape::internal.capacity>;
    using Leaf = LeafNode<Shape>;

    static constexpr std::size_t capacity = Shape::internal.capacity;
    static constexpr std::size_t width = Shape::internal_width;
    static constexpr std::size_t minimum = Shape::internal_minimum;
    static constexpr Slot vacant_slot = static_cast<Slot>(~static_cast<Slot>(0));
    static constexpr unsigned slot_shift = static_cast<unsigned>(64 - 8 * width);

    std::byte* page = nullptr;

    [[nodiscard]] static InternalNode at(NodePointer pointer) noexcept {
        return InternalNode{pointer.page()};
    }
    [[nodiscard]] NodePointer pointer() const noexcept {
        return NodePointer::internal_at(page);
    }

    // An empty node whose edges a split writes next.
    [[nodiscard]] static InternalNode initialize_empty(std::byte* page, std::size_t prefix_length) noexcept {
        Header::set_length(page, 0);
        Header::set_has_head(page, false);
        Header::set_prefix_length(page, prefix_length);
        const InternalNode node = InternalNode{page};
        Slot* slots = node.inline_keys();
        std::size_t index = 0;
        for (index = 0; index < capacity; index += 1) {
            slots[index] = vacant_slot;
        }
        return node;
    }

    [[nodiscard]] static InternalNode initialize(std::byte* page, NodePointer only_child, std::size_t prefix_length) noexcept {
        const InternalNode node = initialize_empty(page, prefix_length);
        node.set_edge(0, only_child);
        return node;
    }

    [[nodiscard]] std::size_t length() const noexcept {
        return Header::length(page);
    }
    [[nodiscard]] std::size_t prefix_length() const noexcept {
        return Header::prefix_length(page);
    }
    [[nodiscard]] bool is_underfull() const noexcept {
        return length() < minimum;
    }

    [[nodiscard]] Slot* inline_keys() const noexcept {
        return std::launder(reinterpret_cast<Slot*>(page + Shape::internal.inline_keys_offset));
    }
    [[nodiscard]] std::uintptr_t* edges() const noexcept {
        return std::launder(reinterpret_cast<std::uintptr_t*>(page + Shape::internal.edges_offset));
    }
    [[nodiscard]] NodePointer edge(std::size_t index) const noexcept {
        if constexpr (Shape::latched) {
            const std::atomic_ref<std::uintptr_t> field(edges()[index]);
            return NodePointer{field.load(std::memory_order_relaxed)};
        } else {
            return NodePointer{edges()[index]};
        }
    }
    void set_edge(std::size_t index, NodePointer child) const noexcept {
        if constexpr (Shape::latched) {
            const std::atomic_ref<std::uintptr_t> field(edges()[index]);
            field.store(child.tagged, std::memory_order_relaxed);
        } else {
            edges()[index] = child.tagged;
        }
    }
    [[nodiscard]] bool has_leaf_children() const noexcept {
        return edge(0).is_leaf();
    }

    [[nodiscard]] static Slot slot_of(InlineWord word) noexcept {
        return static_cast<Slot>(word >> slot_shift);
    }
    [[nodiscard]] static InlineWord word_of(Slot slot) noexcept {
        return static_cast<InlineWord>(slot) << slot_shift;
    }

    // The leaf holding separator i's key: down edge i + 1, then first edges.
    [[nodiscard]] Leaf head_leaf(std::size_t index) const noexcept {
        NodePointer node = edge(index + 1);
        while (!node.is_leaf()) {
            node = InternalNode::at(node).edge(0);
        }
        return Leaf::at(node);
    }
    [[nodiscard]] Key& separator_key(std::size_t index) const noexcept {
        return head_leaf(index).head_key();
    }

    // Separator i's window as stored: only `width` bytes are its key's.
    [[nodiscard]] Window stored_window(std::size_t index) const noexcept {
        const InlineWord word = word_of(inline_keys()[index]);
        return Window{.prefix_length = prefix_length(), .word = word};
    }

    // Separator i's window, whole: read again from its key where the node
    // keeps fewer bytes than leaves need (D43).
    [[nodiscard]] Window window(std::size_t index) const noexcept {
        if constexpr (Shape::rehydrates_windows) {
            return Shape::window_of_key(separator_key(index), prefix_length());
        } else {
            return stored_window(index);
        }
    }

    // The window of a separator beside edge `index`, which every key under
    // that edge shares the bytes between this node's prefix and the child's
    // with; none for a node without separators.
    [[nodiscard]] bool fence_window(std::size_t index, Window* out) const noexcept {
        const std::size_t live = length();
        if (index > 0) {
            *out = window(index - 1);
            return true;
        }
        if (index < live) {
            *out = window(index);
            return true;
        }
        return false;
    }

    struct SearchResult final {
        // A separator equals the query: its value is the head of head_leaf.
        bool found = false;
        // The separator's index if found, else the edge to follow.
        std::size_t index = 0;
    };

    [[nodiscard]] STX_BTREE_ALWAYS_INLINE EqualRange candidates(const Query& query) const noexcept {
        const InlineWord word = Shape::window_word(query, prefix_length());
        const Slot target = slot_of(word);
        return Count::equal_range(inline_keys(), target, length());
    }

    // Ties among separators read their keys, walked to (D49).
    [[nodiscard]] STX_BTREE_NEVER_INLINE SearchResult search_ties(const Query& query, EqualRange range) const noexcept {
        const std::size_t equal_length = prefix_length() + width;
        std::size_t low = range.begin;
        std::size_t high = range.end;
        while (low < high) {
            const std::size_t middle = low + (high - low) / 2;
            const int order = Traits::compare_from(query, separator_key(middle), equal_length);
            if (order == 0) {
                return SearchResult{.found = true, .index = middle};
            }
            if (order < 0) {
                high = middle;
            } else {
                low = middle + 1;
            }
        }
        return SearchResult{.found = false, .index = low};
    }

    [[nodiscard]] STX_BTREE_ALWAYS_INLINE SearchResult search(const Query& query) const noexcept {
        const EqualRange range = candidates(query);
        if (range.begin == range.end) {
            return SearchResult{.found = false, .index = range.begin};
        }
        return search_ties(query, range);
    }

    // --- Changes. Each needs exclusive access to the node. -----------------

    void set_length_refilling(std::size_t new_length) const noexcept {
        const std::size_t old_length = length();
        Slot* slots = inline_keys();
        std::size_t index = 0;
        for (index = new_length; index < old_length; index += 1) {
            slots[index] = vacant_slot;
        }
        Header::set_length(page, new_length);
    }

    void move_windows(std::size_t from, InternalNode target, std::size_t to, std::size_t count) const noexcept {
        if (count != 0) {
            std::memmove(target.inline_keys() + to, inline_keys() + from, count * sizeof(Slot));
        }
    }

    void move_edges(std::size_t from, InternalNode target, std::size_t to, std::size_t count) const noexcept {
        std::size_t step = 0;
        if (count == 0) {
            return;
        }
        if constexpr (Shape::latched) {
            // Racing readers load edges as relaxed atomics; move them so.
            if (target.page != page || to < from) {
                for (step = 0; step < count; step += 1) {
                    target.set_edge(to + step, edge(from + step));
                }
            } else {
                step = count;
                while (step > 0) {
                    step -= 1;
                    target.set_edge(to + step, edge(from + step));
                }
            }
        } else {
            std::memmove(target.edges() + to, edges() + from, count * sizeof(std::uintptr_t));
        }
    }

    // A separator coming up from child `index` carries the child's prefix;
    // it moves back to this node's, filled from a separator beside it.
    [[nodiscard]] Window separator_from_child(std::size_t index, Window separator) const noexcept {
        Window fill;
        const bool has_fill = fence_window(index, &fill);
        if (has_fill) {
            return separator.moved_back_to(fill);
        }
        return separator;
    }

    // Inserts separator (with the child's window) at index and right_child
    // at edge index + 1; needs room.
    void insert_fit(std::size_t index, Window separator, NodePointer right_child) const noexcept {
        const std::size_t old_length = length();
        const Window moved = separator_from_child(index, separator);
        move_windows(index, *this, index + 1, old_length - index);
        move_edges(index + 1, *this, index + 2, old_length - index);
        inline_keys()[index] = slot_of(moved.word);
        set_edge(index + 1, right_child);
        Header::set_length(page, old_length + 1);
    }

    // Moves separators past separator_index, and their edges, to `right`,
    // an empty node with this prefix. Returns the middle separator.
    [[nodiscard]] Window split_around(std::size_t separator_index, InternalNode right) const noexcept {
        const std::size_t old_length = length();
        const std::size_t right_length = old_length - separator_index - 1;
        move_windows(separator_index + 1, right, 0, right_length);
        move_edges(separator_index + 1, right, 0, right_length + 1);
        Header::set_length(right.page, right_length);
        const Window middle = stored_window(separator_index);
        set_length_refilling(separator_index);
        return middle;
    }

    // Removes separator index and the edge right of it.
    void remove_with_right_child(std::size_t index) const noexcept {
        const std::size_t old_length = length();
        move_windows(index + 1, *this, index, old_length - index - 1);
        move_edges(index + 2, *this, index + 1, old_length - index - 1);
        set_length_refilling(old_length - 1);
    }

    void replace_separator(std::size_t index, Window separator) const noexcept {
        inline_keys()[index] = slot_of(separator.word);
    }

    void move_prefix_back_to(Window fill) const noexcept {
        const std::size_t own_prefix = prefix_length();
        if (own_prefix <= fill.prefix_length) {
            return;
        }
        const std::size_t live = length();
        Slot* slots = inline_keys();
        std::size_t index = 0;
        for (index = 0; index < live; index += 1) {
            const Window own = Window{.prefix_length = own_prefix, .word = word_of(slots[index])};
            const Window moved = own.moved_back_to(fill);
            slots[index] = slot_of(moved.word);
        }
        Header::set_prefix_length(page, fill.prefix_length);
    }

    // An internal node grows at most to its children's prefixes (05,
    // "Upkeep").
    [[nodiscard]] std::size_t allowed_prefix_length(std::size_t wanted) const noexcept {
        if (wanted <= prefix_length()) {
            return wanted;
        }
        std::size_t allowed = wanted;
        const std::size_t live = length();
        std::size_t index = 0;
        for (index = 0; index <= live; index += 1) {
            const NodePointer child = edge(index);
            const std::size_t child_prefix = Header::prefix_length(child.page());
            if (child_prefix < allowed) {
                allowed = child_prefix;
            }
        }
        return allowed;
    }

    void refit_prefix(std::size_t wanted) const noexcept {
        const std::size_t new_prefix = allowed_prefix_length(wanted);
        if (new_prefix == prefix_length()) {
            return;
        }
        const std::size_t live = length();
        Slot* slots = inline_keys();
        std::size_t index = 0;
        for (index = 0; index < live; index += 1) {
            const Query query = Traits::query_of(separator_key(index));
            slots[index] = slot_of(Traits::window(query, new_prefix));
        }
        Header::set_prefix_length(page, new_prefix);
    }

    [[nodiscard]] bool keys_differ_past_prefix() const noexcept {
        const std::size_t live = length();
        if (live == 0) {
            return false;
        }
        const InlineWord first = word_of(inline_keys()[0]);
        const InlineWord last = word_of(inline_keys()[live - 1]);
        return ByteEncoding::leading_zero_bits(first ^ last) < 8;
    }
};

} // namespace stx_btree::detail
