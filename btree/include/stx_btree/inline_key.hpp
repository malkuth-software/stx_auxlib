#pragma once

// Inline keys: fixed-width windows of an order-preserving byte encoding of
// each key, which nodes keep in a dense array beside their keys and search
// before reading any key (bbtree decisions D18, D26, D31, D32, D36, D43).
//
// A key type takes part by specializing InlineKeyTraits. The contract,
// unchanged from bbtree's `InlineKey`: each key has a byte-string encoding,
// zero-padded past its end, and if a's encoding orders before b's, then
// a < b. Equal encodings decide nothing. A wrong encoding gives wrong
// answers, never out-of-bounds access.

#include "stx_btree/detail/inlining.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace stx_btree {

// Eight bytes of an encoding, big-endian, so that unsigned comparison of two
// windows at one offset orders like the bytes.
using InlineWord = std::uint64_t;

inline constexpr std::size_t inline_window_bytes = sizeof(InlineWord);

// How many leading bytes of each window a node keeps.
enum class InlineWidth : std::uint8_t { two = 2, four = 4, eight = 8 };

// Shared byte-string arithmetic for encodings that are a byte sequence:
// big-endian loads, windows, common prefixes and comparisons from a known
// equal length. Every load stays inside [bytes, bytes + length).
class ByteEncoding final {
public:
    [[nodiscard]] static InlineWord load_big_endian_64(const std::uint8_t* bytes) noexcept {
        InlineWord word = 0;
        std::size_t index = 0;
        for (index = 0; index < 8; index += 1) {
            word = (word << 8) | static_cast<InlineWord>(bytes[index]);
        }
        return word;
    }

    [[nodiscard]] static InlineWord load_big_endian_32(const std::uint8_t* bytes) noexcept {
        InlineWord word = 0;
        std::size_t index = 0;
        for (index = 0; index < 4; index += 1) {
            word = (word << 8) | static_cast<InlineWord>(bytes[index]);
        }
        return word;
    }

    // Bytes [offset, offset + 8) of bytes[0, length), zero past the end. A
    // tail shorter than a word is read with overlapping fixed-size loads, as
    // bbtree's SearchBytes does, so no copy into a padded buffer is needed.
    [[nodiscard]] STX_BTREE_ALWAYS_INLINE static InlineWord window(
        const std::uint8_t* bytes, std::size_t length, std::size_t offset) noexcept {
        if (offset >= length) {
            return 0;
        }
        if (length - offset >= inline_window_bytes) {
            return load_big_endian_64(bytes + offset);
        }
        if (length >= inline_window_bytes) {
            const InlineWord last_word = load_big_endian_64(bytes + length - inline_window_bytes);
            const std::size_t shift_bytes = offset - (length - inline_window_bytes);
            return last_word << (8 * shift_bytes);
        }
        const InlineWord short_word = short_prefix_word(bytes, length);
        return short_word << (8 * offset);
    }

    // The first difference between two encodings, in bytes, searched over
    // `limit` bytes; `no_difference` if none is found there.
    [[nodiscard]] static std::size_t first_difference(
        const std::uint8_t* a, std::size_t a_length,
        const std::uint8_t* b, std::size_t b_length, std::size_t limit) noexcept {
        std::size_t offset = 0;
        while (offset < limit) {
            const InlineWord differing = window(a, a_length, offset) ^ window(b, b_length, offset);
            if (differing != 0) {
                const std::size_t leading_bytes = leading_zero_bits(differing) / 8;
                return offset + leading_bytes;
            }
            offset += inline_window_bytes;
        }
        return no_difference;
    }

    // Lexicographic order of a and b (then by length), given that their
    // first `equal_length` bytes are known equal. -1, 0 or 1.
    [[nodiscard]] static int compare_from(
        const std::uint8_t* a, std::size_t a_length,
        const std::uint8_t* b, std::size_t b_length, std::size_t equal_length) noexcept {
        const std::size_t both_length = a_length < b_length ? a_length : b_length;
        std::size_t compared = equal_length;
        while (compared + inline_window_bytes <= both_length) {
            const InlineWord a_word = load_big_endian_64(a + compared);
            const InlineWord b_word = load_big_endian_64(b + compared);
            if (a_word != b_word) {
                return a_word < b_word ? -1 : 1;
            }
            compared += inline_window_bytes;
        }
        if (compared < both_length) {
            // Both zero-padded windows: a difference before both_length is
            // the bytes' order; one at or past it, a longer string's nonzero
            // byte, agrees with ordering by length.
            const InlineWord a_word = window(a, a_length, compared);
            const InlineWord b_word = window(b, b_length, compared);
            if (a_word != b_word) {
                return a_word < b_word ? -1 : 1;
            }
        }
        if (a_length == b_length) {
            return 0;
        }
        return a_length < b_length ? -1 : 1;
    }

    // Number of leading bytes of a that equal bytes[0, length), compared a
    // word at a time; the tail, by overlapping fixed-size windows.
    [[nodiscard]] static std::size_t shared_length(
        const std::uint8_t* a, std::size_t a_length,
        const std::uint8_t* bytes, std::size_t length) noexcept {
        const std::size_t limit = a_length < length ? a_length : length;
        std::size_t offset = 0;
        while (offset + inline_window_bytes <= limit) {
            const InlineWord differing = load_big_endian_64(a + offset) ^ load_big_endian_64(bytes + offset);
            if (differing != 0) {
                return offset + leading_zero_bits(differing) / 8;
            }
            offset += inline_window_bytes;
        }
        if (offset < limit) {
            const InlineWord differing = window(a, limit, offset) ^ window(bytes, limit, offset);
            if (differing != 0) {
                const std::size_t difference = offset + leading_zero_bits(differing) / 8;
                return difference < limit ? difference : limit;
            }
        }
        return limit;
    }

    static constexpr std::size_t no_difference = static_cast<std::size_t>(-1);

    [[nodiscard]] static unsigned leading_zero_bits(InlineWord word) noexcept {
        const int count = std::countl_zero(word);
        return static_cast<unsigned>(count);
    }

private:
    // The first `length` (< 8) bytes in the word's top bytes, zero below.
    [[nodiscard]] static InlineWord short_prefix_word(
        const std::uint8_t* bytes, std::size_t length) noexcept {
        if (length >= 4) {
            const InlineWord first = load_big_endian_32(bytes);
            const InlineWord last = load_big_endian_32(bytes + length - 4);
            return (first << 32) | (last << (8 * (8 - length)));
        }
        if (length >= 2) {
            const InlineWord first = (static_cast<InlineWord>(bytes[0]) << 8) | bytes[1];
            const InlineWord last = bytes[length - 1];
            return (first << 48) | (last << (8 * (8 - length)));
        }
        if (length == 1) {
            return static_cast<InlineWord>(bytes[0]) << 56;
        }
        return 0;
    }
};

// Operations every encoding derives from its windows. `Traits` supplies
// Query and `window(query, offset)`; a user specialization of
// InlineKeyTraits may inherit these to get the remaining members.
template <typename Traits>
class InlineKeyWindows {
public:
    // The longest common prefix of two encodings, capped at the most a node
    // records (65535).
    template <typename QueryType>
    [[nodiscard]] static std::size_t common_prefix_by_windows(
        const QueryType& a, const QueryType& b, std::size_t limit) noexcept {
        std::size_t offset = 0;
        while (offset < limit) {
            const InlineWord differing = Traits::window(a, offset) ^ Traits::window(b, offset);
            if (differing != 0) {
                const std::size_t leading_bytes = ByteEncoding::leading_zero_bits(differing) / 8;
                const std::size_t difference = offset + leading_bytes;
                return difference < maximum_prefix_length ? difference : maximum_prefix_length;
            }
            offset += inline_window_bytes;
        }
        return maximum_prefix_length;
    }

    // Leading bytes of the encoding the query shares with bytes[0, length).
    template <typename QueryType>
    [[nodiscard]] static std::size_t shared_length_by_windows(
        const QueryType& query, const std::uint8_t* bytes, std::size_t length) noexcept {
        std::size_t offset = 0;
        while (offset < length) {
            const InlineWord differing =
                Traits::window(query, offset) ^ ByteEncoding::window(bytes, length, offset);
            if (differing != 0) {
                const std::size_t leading_bytes = ByteEncoding::leading_zero_bits(differing) / 8;
                const std::size_t difference = offset + leading_bytes;
                return difference < length ? difference : length;
            }
            offset += inline_window_bytes;
        }
        return length;
    }

    // Order of the query's first `length` encoding bytes against bytes.
    template <typename QueryType>
    [[nodiscard]] static int compare_shared_by_windows(
        const QueryType& query, const std::uint8_t* bytes, std::size_t length) noexcept {
        std::size_t offset = 0;
        while (offset < length) {
            InlineWord query_word = Traits::window(query, offset);
            InlineWord shared_word = ByteEncoding::window(bytes, length, offset);
            const std::size_t remaining = length - offset;
            if (remaining < inline_window_bytes) {
                const unsigned dropped_bits = static_cast<unsigned>(8 * (inline_window_bytes - remaining));
                query_word >>= dropped_bits;
                shared_word >>= dropped_bits;
            }
            if (query_word != shared_word) {
                return query_word < shared_word ? -1 : 1;
            }
            offset += inline_window_bytes;
        }
        return 0;
    }

    // Writes the first encoding bytes, up to `capacity`, for a tree's shared
    // prefix; returns how many.
    template <typename QueryType>
    [[nodiscard]] static std::size_t leading_bytes_by_windows(
        const QueryType& query, std::size_t encoded_length,
        std::uint8_t* out, std::size_t capacity) noexcept {
        const std::size_t length = encoded_length < capacity ? encoded_length : capacity;
        std::size_t offset = 0;
        while (offset < length) {
            const InlineWord word = Traits::window(query, offset);
            std::size_t index = 0;
            for (index = 0; index < inline_window_bytes && offset + index < length; index += 1) {
                out[offset + index] = static_cast<std::uint8_t>(word >> (56 - 8 * index));
            }
            offset += inline_window_bytes;
        }
        return length;
    }

    static constexpr std::size_t maximum_prefix_length = 65535;
};

// The shared-prefix check of an 8-byte encoding: the leading `length`
// (at most 8) bytes of the encoding against the shared bytes, as one word.
class IntegerPrefix final {
public:
    [[nodiscard]] static int compare(InlineWord encoding, const std::uint8_t* bytes, std::size_t length) noexcept {
        if (length == 0) {
            return 0;
        }
        const std::size_t bounded = length < 8 ? length : 8;
        const unsigned dropped_bits = static_cast<unsigned>(64 - 8 * bounded);
        const InlineWord shared_word = ByteEncoding::window(bytes, bounded, 0);
        const InlineWord query_top = dropped_bits == 64 ? 0 : encoding >> dropped_bits;
        const InlineWord shared_top = dropped_bits == 64 ? 0 : shared_word >> dropped_bits;
        if (query_top == shared_top) {
            return 0;
        }
        return query_top < shared_top ? -1 : 1;
    }
};

// The primary template is undefined: a key without an encoding cannot be a
// key of these maps (keys without inline keys are left out of this port).
template <typename Key>
struct InlineKeyTraits;

// Unsigned 64-bit integers: their 8 big-endian bytes; nodes keep 2 (D31).
template <>
struct InlineKeyTraits<std::uint64_t> final : InlineKeyWindows<InlineKeyTraits<std::uint64_t>> {
    using Key = std::uint64_t;
    using Query = std::uint64_t;
    static constexpr InlineWidth leaf_width = InlineWidth::two;
    static constexpr InlineWidth internal_width = InlineWidth::two;
    static constexpr bool has_hash_tags = false;
    // The key is its own bytes: a racing copy compared after a version check
    // is meaningful (D57).
    static constexpr bool orders_by_own_bytes = true;
    static constexpr bool tag_is_length = false;
    static constexpr std::size_t encoded_bytes = 8;

    [[nodiscard]] static Query query_of(const Key& key) noexcept {
        return key;
    }
    [[nodiscard]] static InlineWord window(const Query& query, std::size_t offset) noexcept {
        if (offset >= encoded_bytes) {
            return 0;
        }
        return query << (8 * offset);
    }
    [[nodiscard]] static int compare_from(const Query& query, const Key& stored, std::size_t) noexcept {
        if (query == stored) {
            return 0;
        }
        return query < stored ? -1 : 1;
    }
    [[nodiscard]] static bool equal_from(const Query& query, const Key& stored, std::size_t) noexcept {
        return query == stored;
    }
    [[nodiscard]] static std::uint8_t hash_tag(const Query&) noexcept {
        return 0;
    }
    [[nodiscard]] static std::size_t encoded_length(const Query&) noexcept {
        return encoded_bytes;
    }
    [[nodiscard]] static std::size_t common_prefix_length(const Key& a, const Key& b) noexcept {
        return common_prefix_by_windows(a, b, encoded_bytes);
    }
    [[nodiscard]] static std::size_t leading_bytes(
        const Query& query, std::uint8_t* out, std::size_t capacity) noexcept {
        return leading_bytes_by_windows(query, encoded_bytes, out, capacity);
    }
    [[nodiscard]] static std::size_t shared_length(
        const Query& query, const std::uint8_t* bytes, std::size_t length) noexcept {
        return shared_length_by_windows(query, bytes, length);
    }
    // A key's shared prefix is at most its 8 bytes: one word compare.
    [[nodiscard]] static int compare_shared(
        const Query& query, const std::uint8_t* bytes, std::size_t length) noexcept {
        return IntegerPrefix::compare(query, bytes, length);
    }
};

// Signed 64-bit integers: the unsigned encoding with the sign bit flipped,
// so that negative numbers order first.
template <>
struct InlineKeyTraits<std::int64_t> final : InlineKeyWindows<InlineKeyTraits<std::int64_t>> {
    using Key = std::int64_t;
    using Query = std::int64_t;
    static constexpr InlineWidth leaf_width = InlineWidth::two;
    static constexpr InlineWidth internal_width = InlineWidth::two;
    static constexpr bool has_hash_tags = false;
    static constexpr bool orders_by_own_bytes = true;
    static constexpr bool tag_is_length = false;
    static constexpr std::size_t encoded_bytes = 8;

    [[nodiscard]] static Query query_of(const Key& key) noexcept {
        return key;
    }
    [[nodiscard]] static InlineWord encoding(const Query& query) noexcept {
        const InlineWord bits = static_cast<InlineWord>(query);
        return bits ^ (static_cast<InlineWord>(1) << 63);
    }
    [[nodiscard]] static InlineWord window(const Query& query, std::size_t offset) noexcept {
        if (offset >= encoded_bytes) {
            return 0;
        }
        return encoding(query) << (8 * offset);
    }
    [[nodiscard]] static int compare_from(const Query& query, const Key& stored, std::size_t) noexcept {
        if (query == stored) {
            return 0;
        }
        return query < stored ? -1 : 1;
    }
    [[nodiscard]] static bool equal_from(const Query& query, const Key& stored, std::size_t) noexcept {
        return query == stored;
    }
    [[nodiscard]] static std::uint8_t hash_tag(const Query&) noexcept {
        return 0;
    }
    [[nodiscard]] static std::size_t encoded_length(const Query&) noexcept {
        return encoded_bytes;
    }
    [[nodiscard]] static std::size_t common_prefix_length(const Key& a, const Key& b) noexcept {
        return common_prefix_by_windows(a, b, encoded_bytes);
    }
    [[nodiscard]] static std::size_t leading_bytes(
        const Query& query, std::uint8_t* out, std::size_t capacity) noexcept {
        return leading_bytes_by_windows(query, encoded_bytes, out, capacity);
    }
    [[nodiscard]] static std::size_t shared_length(
        const Query& query, const std::uint8_t* bytes, std::size_t length) noexcept {
        return shared_length_by_windows(query, bytes, length);
    }
    [[nodiscard]] static int compare_shared(
        const Query& query, const std::uint8_t* bytes, std::size_t length) noexcept {
        return IntegerPrefix::compare(encoding(query), bytes, length);
    }
};

// Fixed byte arrays (UUIDs, hashes): their bytes; nodes keep 2 (D32). A tie
// reads a key from the node being searched, never the heap.
template <std::size_t Length>
struct InlineKeyTraits<std::array<std::uint8_t, Length>> final
    : InlineKeyWindows<InlineKeyTraits<std::array<std::uint8_t, Length>>> {
    using Key = std::array<std::uint8_t, Length>;
    using Query = std::array<std::uint8_t, Length>;
    using Windows = InlineKeyWindows<InlineKeyTraits<std::array<std::uint8_t, Length>>>;
    static constexpr InlineWidth leaf_width = InlineWidth::two;
    static constexpr InlineWidth internal_width = InlineWidth::two;
    static constexpr bool has_hash_tags = false;
    static constexpr bool orders_by_own_bytes = true;
    static constexpr bool tag_is_length = false;
    static constexpr std::size_t encoded_bytes = Length;

    [[nodiscard]] static const Query& query_of(const Key& key) noexcept {
        return key;
    }
    [[nodiscard]] static InlineWord window(const Query& query, std::size_t offset) noexcept {
        return ByteEncoding::window(query.data(), Length, offset);
    }
    [[nodiscard]] static int compare_from(
        const Query& query, const Key& stored, std::size_t equal_length) noexcept {
        return ByteEncoding::compare_from(query.data(), Length, stored.data(), Length, equal_length);
    }
    // One candidate compares every byte at once (D38).
    [[nodiscard]] static bool equal_from(const Query& query, const Key& stored, std::size_t) noexcept {
        return std::memcmp(query.data(), stored.data(), Length) == 0;
    }
    [[nodiscard]] static std::uint8_t hash_tag(const Query&) noexcept {
        return 0;
    }
    [[nodiscard]] static std::size_t encoded_length(const Query&) noexcept {
        return Length;
    }
    [[nodiscard]] static std::size_t common_prefix_length(const Key& a, const Key& b) noexcept {
        const std::size_t difference =
            ByteEncoding::first_difference(a.data(), Length, b.data(), Length, Length);
        if (difference == ByteEncoding::no_difference || difference > Windows::maximum_prefix_length) {
            return Windows::maximum_prefix_length;
        }
        return difference;
    }
    [[nodiscard]] static std::size_t leading_bytes(
        const Query& query, std::uint8_t* out, std::size_t capacity) noexcept {
        const std::size_t length = Length < capacity ? Length : capacity;
        std::memcpy(out, query.data(), length);
        return length;
    }
    [[nodiscard]] static std::size_t shared_length(
        const Query& query, const std::uint8_t* bytes, std::size_t length) noexcept {
        return ByteEncoding::shared_length(query.data(), Length, bytes, length);
    }
    [[nodiscard]] static int compare_shared(
        const Query& query, const std::uint8_t* bytes, std::size_t length) noexcept {
        return Windows::compare_shared_by_windows(query, bytes, length);
    }
};

// Heap strings: their bytes. Leaves keep 8 bytes of each window and a hash
// tag per key, its length saturated at 255; internal nodes keep 4, since a
// tie there compares from where keys differ (D36, D43). Queries are views.
template <>
struct InlineKeyTraits<std::string> final : InlineKeyWindows<InlineKeyTraits<std::string>> {
    using Key = std::string;
    using Query = std::string_view;
    static constexpr InlineWidth leaf_width = InlineWidth::eight;
    static constexpr InlineWidth internal_width = InlineWidth::four;
    static constexpr bool has_hash_tags = true;
    // A key points at its bytes: no racing copy may be compared (D57).
    static constexpr bool orders_by_own_bytes = false;
    // A tag below 255 is the key's exact length: a key that ends within its
    // leaf window is settled by its window and tag, reading no heap byte.
    static constexpr bool tag_is_length = true;

    [[nodiscard]] static const std::uint8_t* bytes_of(const Query& query) noexcept {
        return reinterpret_cast<const std::uint8_t*>(query.data());
    }
    [[nodiscard]] static Query query_of(const Key& key) noexcept {
        return Query(key.data(), key.size());
    }
    [[nodiscard]] static InlineWord window(const Query& query, std::size_t offset) noexcept {
        return ByteEncoding::window(bytes_of(query), query.size(), offset);
    }
    [[nodiscard]] static int compare_from(
        const Query& query, const Key& stored, std::size_t equal_length) noexcept {
        const Query stored_view = query_of(stored);
        return ByteEncoding::compare_from(
            bytes_of(query), query.size(), bytes_of(stored_view), stored_view.size(), equal_length);
    }
    [[nodiscard]] static bool equal_from(
        const Query& query, const Key& stored, std::size_t equal_length) noexcept {
        if (query.size() != stored.size()) {
            return false;
        }
        const int order = compare_from(query, stored, equal_length);
        return order == 0;
    }
    [[nodiscard]] static std::uint8_t hash_tag(const Query& query) noexcept {
        if (query.size() >= 255) {
            return 255;
        }
        return static_cast<std::uint8_t>(query.size());
    }
    [[nodiscard]] static std::size_t encoded_length(const Query& query) noexcept {
        return query.size();
    }
    [[nodiscard]] static std::size_t common_prefix_length(const Key& a, const Key& b) noexcept {
        const std::size_t limit = a.size() > b.size() ? a.size() : b.size();
        const std::size_t difference = ByteEncoding::first_difference(
            reinterpret_cast<const std::uint8_t*>(a.data()), a.size(),
            reinterpret_cast<const std::uint8_t*>(b.data()), b.size(), limit);
        if (difference == ByteEncoding::no_difference || difference > maximum_prefix_length) {
            return maximum_prefix_length;
        }
        return difference;
    }
    [[nodiscard]] static std::size_t leading_bytes(
        const Query& query, std::uint8_t* out, std::size_t capacity) noexcept {
        const std::size_t length = query.size() < capacity ? query.size() : capacity;
        std::memcpy(out, query.data(), length);
        return length;
    }
    [[nodiscard]] static std::size_t shared_length(
        const Query& query, const std::uint8_t* bytes, std::size_t length) noexcept {
        return ByteEncoding::shared_length(bytes_of(query), query.size(), bytes, length);
    }
    // A query shorter than the shared bytes, and otherwise equal, sorts
    // before every key, which all start with them.
    [[nodiscard]] static int compare_shared(
        const Query& query, const std::uint8_t* bytes, std::size_t length) noexcept {
        const std::size_t limit = query.size() < length ? query.size() : length;
        const int order = limit == 0 ? 0 : std::memcmp(query.data(), bytes, limit);
        if (order != 0) {
            return order < 0 ? -1 : 1;
        }
        if (query.size() < length) {
            return -1;
        }
        return 0;
    }
};

} // namespace stx_btree
