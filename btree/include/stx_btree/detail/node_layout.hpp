#pragma once

// Compile-time byte layouts of node pages (bbtree layout.rs, D9, D13, D15,
// D21, D29, D34, D36, D49).
//
//   leaf      | header | w0 .. w_c | [tags] | (hk head) (k0 v0) (k1 v1) .. |
//   internal  | header | w0 .. w_c | e0 e1 .. e_c+1                         |
//
// Every node has an 8-byte header (16 with a latch word). Inline keys of the
// kind's width follow from offset 8 (16), `capacity` of them. A leaf pairs
// each key with its value in an entry; entry 0 holds the head, the value of
// the separator left of the leaf, beside that separator's key (D17, D49).
// Key i is in entry i + 1. Power-of-two entries up to a cache line start at
// a multiple of their size, so none crosses a line (D34). Internal nodes keep
// no keys, only windows and edges, in 512-byte pages. A leaf whose key type
// has hash tags keeps a byte per key after its inline keys where room allows.

#include <cstddef>
#include <cstdint>

namespace stx_btree::detail {

inline constexpr std::size_t cache_line_bytes = 64;
inline constexpr std::size_t minimum_node_capacity = 12;
inline constexpr std::size_t maximum_node_capacity = 1023;
inline constexpr std::size_t leaf_page_step = 1024;
inline constexpr std::size_t internal_page_step = 512;
inline constexpr std::size_t edge_bytes = sizeof(std::uintptr_t);

// Non-root internal nodes keep at least 5 children and leaves are at least
// 1 KiB, so leaves lie at most 23 levels below the root (D22).
inline constexpr std::size_t maximum_depth = 23;
inline constexpr std::size_t node_stack_levels = maximum_depth + 1;

// What every key shares, kept after the root node in the root page (D50).
inline constexpr std::size_t shared_prefix_capacity = 64;

class LayoutArithmetic final {
public:
    [[nodiscard]] static constexpr std::size_t round_up(std::size_t value, std::size_t step) noexcept {
        const std::size_t remainder = value % step;
        if (remainder == 0) {
            return value;
        }
        return value + (step - remainder);
    }

    [[nodiscard]] static constexpr std::size_t larger(std::size_t a, std::size_t b) noexcept {
        return a > b ? a : b;
    }

    [[nodiscard]] static constexpr std::size_t smaller(std::size_t a, std::size_t b) noexcept {
        return a < b ? a : b;
    }

    [[nodiscard]] static constexpr bool is_power_of_two(std::size_t value) noexcept {
        return value != 0 && (value & (value - 1)) == 0;
    }
};

struct LeafLayout final {
    std::size_t page_bytes = 0;
    std::size_t page_alignment = 0;
    std::size_t capacity = 0;
    std::size_t inline_keys_offset = 0;
    std::size_t inline_key_bytes = 0;
    bool has_hash_tags = false;
    std::size_t hash_tags_offset = 0;
    // Entry 0, the head's; key i is in entry i + 1.
    std::size_t entries_offset = 0;
    std::size_t entry_stride = 0;
    // From an entry's start to its value.
    std::size_t value_offset = 0;
};

struct InternalLayout final {
    std::size_t page_bytes = 0;
    std::size_t page_alignment = 0;
    std::size_t capacity = 0;
    std::size_t inline_keys_offset = 0;
    std::size_t inline_key_bytes = 0;
    std::size_t edges_offset = 0;
};

struct RootPageLayout final {
    std::size_t page_bytes = 0;
    std::size_t page_alignment = 0;
    // Where the shared prefix's length byte, then its bytes, sit.
    std::size_t shared_prefix_offset = 0;
};

// Layouts for keys of KeyBytes/KeyAlignment and values of
// ValueBytes/ValueAlignment, with a header of HeaderBytes.
class LayoutPlanner final {
public:
    struct EntryShape final {
        std::size_t size = 0;
        std::size_t alignment = 0;
        std::size_t value_offset = 0;
    };

    [[nodiscard]] static constexpr EntryShape entry_shape(
        std::size_t key_bytes, std::size_t key_alignment,
        std::size_t value_bytes, std::size_t value_alignment) noexcept {
        const std::size_t alignment = LayoutArithmetic::larger(key_alignment, value_alignment);
        const std::size_t value_offset = LayoutArithmetic::round_up(key_bytes, value_alignment);
        const std::size_t size = LayoutArithmetic::round_up(value_offset + value_bytes, alignment);
        return EntryShape{.size = size, .alignment = alignment, .value_offset = value_offset};
    }

    [[nodiscard]] static constexpr std::size_t inline_keys_offset(std::size_t header_bytes) noexcept {
        return LayoutArithmetic::round_up(header_bytes, sizeof(std::uint64_t));
    }

    [[nodiscard]] static constexpr std::size_t leaf_bytes_needed(
        std::size_t header_bytes, std::size_t column_bytes, EntryShape entry, std::size_t capacity) noexcept {
        const std::size_t columns_end = inline_keys_offset(header_bytes) + capacity * column_bytes;
        return LayoutArithmetic::round_up(columns_end, entry.alignment) + (capacity + 1) * entry.size;
    }

    [[nodiscard]] static constexpr std::size_t maximum_capacity_for(
        std::size_t page_step, std::size_t inline_key_bytes) noexcept {
        const std::size_t inline_bound = LayoutArithmetic::larger(page_step, 1024) / inline_key_bytes;
        return LayoutArithmetic::smaller(maximum_node_capacity, inline_bound);
    }

    struct LeafShape final {
        std::size_t page_bytes = 0;
        std::size_t page_alignment = 0;
        std::size_t capacity = 0;
        std::size_t entries_offset = 0;
    };

    [[nodiscard]] static constexpr LeafShape leaf_shape(
        std::size_t header_bytes, std::size_t inline_key_bytes, std::size_t column_bytes, EntryShape entry) noexcept {
        const std::size_t alignment = LayoutArithmetic::larger(leaf_page_step, entry.alignment);
        const std::size_t page_bytes = LayoutArithmetic::round_up(
            leaf_bytes_needed(header_bytes, column_bytes, entry, minimum_node_capacity), alignment);
        std::size_t capacity = maximum_capacity_for(leaf_page_step, inline_key_bytes);
        const std::size_t per_key = column_bytes + entry.size;
        const std::size_t size_bound = page_bytes / per_key;
        capacity = LayoutArithmetic::smaller(capacity, size_bound);
        while (leaf_bytes_needed(header_bytes, column_bytes, entry, capacity) > page_bytes) {
            capacity -= 1;
        }
        const std::size_t columns_end = inline_keys_offset(header_bytes) + capacity * column_bytes;
        const bool aligned_to_size =
            LayoutArithmetic::is_power_of_two(entry.size) && entry.size <= cache_line_bytes;
        const std::size_t entries_alignment = aligned_to_size ? entry.size : entry.alignment;
        const std::size_t entries_offset = LayoutArithmetic::round_up(columns_end, entries_alignment);
        return LeafShape{
            .page_bytes = page_bytes, .page_alignment = alignment,
            .capacity = capacity, .entries_offset = entries_offset};
    }

    // A leaf keeps hash tags where they fit in the padding before entry 0;
    // where they fit only by taking the head key's place, it gives up an
    // entry to keep them; otherwise it keeps none (D36).
    [[nodiscard]] static constexpr LeafLayout leaf(
        std::size_t header_bytes, std::size_t inline_key_bytes, bool hash_tags,
        std::size_t key_bytes, std::size_t key_alignment,
        std::size_t value_bytes, std::size_t value_alignment) noexcept {
        const EntryShape entry = entry_shape(key_bytes, key_alignment, value_bytes, value_alignment);
        const LeafShape plain = leaf_shape(header_bytes, inline_key_bytes, inline_key_bytes, entry);
        const std::size_t keys_offset = inline_keys_offset(header_bytes);
        LeafLayout layout = LeafLayout{
            .page_bytes = plain.page_bytes, .page_alignment = plain.page_alignment,
            .capacity = plain.capacity, .inline_keys_offset = keys_offset,
            .inline_key_bytes = inline_key_bytes, .has_hash_tags = false, .hash_tags_offset = 0,
            .entries_offset = plain.entries_offset, .entry_stride = entry.size,
            .value_offset = entry.value_offset};
        if (!hash_tags) {
            return layout;
        }
        const std::size_t tags_start = keys_offset + plain.capacity * inline_key_bytes;
        const std::size_t tags_end = tags_start + plain.capacity;
        if (tags_end <= plain.entries_offset) {
            layout.has_hash_tags = true;
            layout.hash_tags_offset = tags_start;
            return layout;
        }
        const std::size_t head_value_offset = plain.entries_offset + entry.value_offset;
        if (tags_end <= head_value_offset) {
            const LeafShape tagged = leaf_shape(header_bytes, inline_key_bytes, inline_key_bytes + 1, entry);
            layout.page_bytes = tagged.page_bytes;
            layout.page_alignment = tagged.page_alignment;
            layout.capacity = tagged.capacity;
            layout.entries_offset = tagged.entries_offset;
            layout.has_hash_tags = true;
            layout.hash_tags_offset = keys_offset + tagged.capacity * inline_key_bytes;
            return layout;
        }
        return layout;
    }

    [[nodiscard]] static constexpr std::size_t internal_bytes_needed(
        std::size_t header_bytes, std::size_t inline_key_bytes, std::size_t capacity) noexcept {
        const std::size_t columns_end = inline_keys_offset(header_bytes) + capacity * inline_key_bytes;
        return LayoutArithmetic::round_up(columns_end, edge_bytes) + (capacity + 1) * edge_bytes;
    }

    [[nodiscard]] static constexpr InternalLayout internal(
        std::size_t header_bytes, std::size_t inline_key_bytes) noexcept {
        const std::size_t page_bytes = LayoutArithmetic::round_up(
            internal_bytes_needed(header_bytes, inline_key_bytes, minimum_node_capacity), internal_page_step);
        std::size_t capacity = maximum_capacity_for(internal_page_step, inline_key_bytes);
        const std::size_t size_bound = page_bytes / (inline_key_bytes + edge_bytes);
        capacity = LayoutArithmetic::smaller(capacity, size_bound);
        while (internal_bytes_needed(header_bytes, inline_key_bytes, capacity) > page_bytes) {
            capacity -= 1;
        }
        const std::size_t keys_offset = inline_keys_offset(header_bytes);
        const std::size_t edges_offset =
            LayoutArithmetic::round_up(keys_offset + capacity * inline_key_bytes, edge_bytes);
        return InternalLayout{
            .page_bytes = page_bytes, .page_alignment = internal_page_step, .capacity = capacity,
            .inline_keys_offset = keys_offset, .inline_key_bytes = inline_key_bytes,
            .edges_offset = edges_offset};
    }

    // Room for a node of either kind, then the shared prefix (D52).
    [[nodiscard]] static constexpr RootPageLayout root_page(LeafLayout leaf, InternalLayout internal) noexcept {
        const std::size_t node_bytes = LayoutArithmetic::larger(leaf.page_bytes, internal.page_bytes);
        const std::size_t alignment = LayoutArithmetic::larger(leaf.page_alignment, internal.page_alignment);
        const std::size_t page_bytes = LayoutArithmetic::round_up(node_bytes + 1 + shared_prefix_capacity, 64);
        return RootPageLayout{
            .page_bytes = page_bytes, .page_alignment = alignment, .shared_prefix_offset = node_bytes};
    }
};

} // namespace stx_btree::detail
