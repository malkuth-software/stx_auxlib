#pragma once

// Deterministic randomness and key generation for the tests.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace stx_btree_tests {

class TestFailure final {
public:
    static void expect(bool condition, const char* label, const char* what) {
        if (!condition) {
            std::fprintf(stderr, "FAILED [%s]: %s\n", label, what);
            std::abort();
        }
    }
};

// SplitMix64 (Steele, Lea, Flood), whose finalizer is a bijection of 64-bit
// words: distinct indexes give distinct keys.
class SplitMix64 final {
public:
    explicit SplitMix64(std::uint64_t seed) noexcept : state(seed) {}

    [[nodiscard]] static std::uint64_t mix(std::uint64_t value) noexcept {
        std::uint64_t z = value + 0x9e3779b97f4a7c15ULL;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }
    [[nodiscard]] std::uint64_t next() noexcept {
        const std::uint64_t value = mix(state);
        state += 0x9e3779b97f4a7c15ULL;
        return value;
    }
    [[nodiscard]] std::uint64_t below(std::uint64_t bound) noexcept {
        const std::uint64_t value = next();
        return value % bound;
    }
    template <typename Element>
    void shuffle(std::vector<Element>& elements) noexcept {
        std::size_t index = elements.size();
        while (index > 1) {
            index -= 1;
            const std::size_t other = static_cast<std::size_t>(below(index + 1));
            std::swap(elements[index], elements[other]);
        }
    }

private:
    std::uint64_t state = 0;
};

template <typename Key>
class KeyMaker;

template <>
class KeyMaker<std::uint64_t> final {
public:
    explicit KeyMaker(std::uint64_t seed) noexcept : salt(seed) {}
    [[nodiscard]] std::uint64_t make(std::uint64_t index) const noexcept {
        return SplitMix64::mix(index ^ salt);
    }
    [[nodiscard]] std::uint64_t make_ordered(std::uint64_t rank) const noexcept {
        return rank;
    }

private:
    std::uint64_t salt = 0;
};

template <>
class KeyMaker<std::int64_t> final {
public:
    explicit KeyMaker(std::uint64_t seed) noexcept : salt(seed) {}
    [[nodiscard]] std::int64_t make(std::uint64_t index) const noexcept {
        return static_cast<std::int64_t>(SplitMix64::mix(index ^ salt));
    }
    [[nodiscard]] std::int64_t make_ordered(std::uint64_t rank) const noexcept {
        return static_cast<std::int64_t>(rank) - 1000000;
    }

private:
    std::uint64_t salt = 0;
};

template <std::size_t Length>
class KeyMaker<std::array<std::uint8_t, Length>> final {
public:
    explicit KeyMaker(std::uint64_t seed) noexcept : salt(seed) {}
    [[nodiscard]] std::array<std::uint8_t, Length> make(std::uint64_t index) const noexcept {
        std::array<std::uint8_t, Length> key{};
        std::uint64_t word = SplitMix64::mix(index ^ salt);
        std::size_t position = 0;
        for (position = 0; position < Length; position += 1) {
            if (position % 8 == 0 && position != 0) {
                word = SplitMix64::mix(word);
            }
            key[position] = static_cast<std::uint8_t>(word >> (56 - 8 * (position % 8)));
        }
        // Version-7-like keys: a shared leading timestamp for a quarter.
        if (index % 4 == 0) {
            key[0] = 0x01;
            key[1] = 0x8f;
        }
        return key;
    }
    [[nodiscard]] std::array<std::uint8_t, Length> make_ordered(std::uint64_t rank) const noexcept {
        std::array<std::uint8_t, Length> key{};
        std::size_t position = 0;
        for (position = 0; position < 8 && position < Length; position += 1) {
            key[Length - 8 + position] = static_cast<std::uint8_t>(rank >> (56 - 8 * position));
        }
        key[0] = 0x42;
        return key;
    }

private:
    std::uint64_t salt = 0;
};

// Strings of varied shape: short words, keys with a common head, paths that
// share long prefixes (some longer than the 64-byte shared prefix cap), and
// keys holding zero bytes.
template <>
class KeyMaker<std::string> final {
public:
    explicit KeyMaker(std::uint64_t seed) noexcept : salt(seed) {}
    [[nodiscard]] std::string make(std::uint64_t index) const {
        const std::uint64_t word = SplitMix64::mix(index ^ salt);
        const std::string tail = hex(word, 4 + static_cast<std::size_t>(word % 13));
        switch (index % 5) {
        case 0:
            return tail;
        case 1:
            return std::string("user:") + tail;
        case 2:
            return std::string("/home/someone/projects/btree/src/") + tail;
        case 3:
            return std::string("https://example.org/a/very/long/common/path/segment/that/exceeds/sixty-four/bytes/") + tail;
        default: {
            std::string with_zero("k");
            with_zero.push_back('\0');
            with_zero += tail;
            return with_zero;
        }
        }
    }
    [[nodiscard]] std::string make_ordered(std::uint64_t rank) const {
        return std::string("item/") + decimal(rank, 12);
    }

private:
    [[nodiscard]] static std::string hex(std::uint64_t word, std::size_t length) {
        static constexpr char digits[] = "0123456789abcdef";
        std::string text;
        std::size_t index = 0;
        std::uint64_t value = word;
        for (index = 0; index < length; index += 1) {
            text.push_back(digits[value & 15]);
            value = (value >> 4) | (value << 60);
        }
        return text;
    }
    [[nodiscard]] static std::string decimal(std::uint64_t value, std::size_t width) {
        std::string text(width, '0');
        std::size_t index = width;
        std::uint64_t remaining = value;
        while (index > 0) {
            index -= 1;
            text[index] = static_cast<char>('0' + remaining % 10);
            remaining /= 10;
        }
        return text;
    }
    std::uint64_t salt = 0;
};

} // namespace stx_btree_tests
