// BLAKE2s-256 (include/stx_auxlib/hash.hpp), after RFC 7693 sections 2 and
// 3: ten rounds of the G function over a 16-word block, little-endian
// words, parameter block for a 32-byte digest, no salt or personalization.

#include "stx_auxlib/hash.hpp"

#include "checked.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace stx_auxlib::hash {

namespace {

constexpr std::array<std::uint32_t, 8> initial = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                                                  0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};

constexpr std::array<std::array<std::uint8_t, 16>, 10> sigma = {{
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3},
    {11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4},
    {7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8},
    {9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13},
    {2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9},
    {12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11},
    {13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10},
    {6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5},
    {10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0},
}};

std::uint32_t load_little(const std::byte* const bytes) noexcept {
    const std::uint32_t result = std::to_integer<std::uint32_t>(bytes[0]) |
                                 std::to_integer<std::uint32_t>(bytes[1]) << 8 |
                                 std::to_integer<std::uint32_t>(bytes[2]) << 16 |
                                 std::to_integer<std::uint32_t>(bytes[3]) << 24;
    return result;
}

// RFC 7693 section 3.1; BLAKE2s is defined modulo 2^32.
STX_AUXLIB_MODULAR void mix(std::array<std::uint32_t, 16>& v, const std::size_t a, const std::size_t b,
                            const std::size_t c, const std::size_t d, const std::uint32_t x,
                            const std::uint32_t y) noexcept {
    v[a] = v[a] + v[b] + x;
    v[d] = std::rotr(v[d] ^ v[a], 16);
    v[c] = v[c] + v[d];
    v[b] = std::rotr(v[b] ^ v[c], 12);
    v[a] = v[a] + v[b] + y;
    v[d] = std::rotr(v[d] ^ v[a], 8);
    v[c] = v[c] + v[d];
    v[b] = std::rotr(v[b] ^ v[c], 7);
}

} // namespace

bool Digest::operator==(const Digest& other) const noexcept {
    const bool result = bytes == other.bytes;
    return result;
}

bool Digest::operator!=(const Digest& other) const noexcept {
    const bool result = bytes != other.bytes;
    return result;
}

Blake2s::Blake2s() noexcept { reset(); }

Blake2s::Blake2s(const std::span<const std::byte> key) noexcept {
    key_length_ = key.size() < key_.size() ? key.size() : key_.size();
    std::memcpy(key_.data(), key.data(), key_length_);
    reset();
}

Blake2s::~Blake2s() {
    secure_erase(key_);
    secure_erase(buffer_);
    secure_erase(std::as_writable_bytes(std::span<std::uint32_t>(state_)));
}

// Parameter block word 0: digest length 32, key length, fanout 1, depth 1.
// A keyed hash starts with the key padded to one block (RFC 7693 3.3).
void Blake2s::reset() noexcept {
    state_ = initial;
    state_[0] ^= 0x01010000U ^ (static_cast<std::uint32_t>(key_length_) << 8) ^ 32U;
    buffer_.fill(std::byte{0});
    buffered_ = 0;
    counted_ = 0;
    if (key_length_ > 0) {
        std::memcpy(buffer_.data(), key_.data(), key_length_);
        buffered_ = buffer_.size();
    }
}

// Compresses buffer_, counting `buffered_` more bytes. The counter cannot
// wrap: a 64-bit byte count would need 16 EiB of input.
void Blake2s::compress(const bool last) noexcept {
    counted_ += buffered_;
    std::array<std::uint32_t, 16> message{};
    for (std::size_t word = 0; word < 16; ++word) message[word] = load_little(buffer_.data() + 4 * word);
    std::array<std::uint32_t, 16> v{};
    for (std::size_t word = 0; word < 8; ++word) {
        v[word] = state_[word];
        v[word + 8] = initial[word];
    }
    v[12] ^= static_cast<std::uint32_t>(counted_);
    v[13] ^= static_cast<std::uint32_t>(counted_ >> 32);
    if (last) v[14] = ~v[14];
    for (const std::array<std::uint8_t, 16>& order : sigma) {
        mix(v, 0, 4, 8, 12, message[order[0]], message[order[1]]);
        mix(v, 1, 5, 9, 13, message[order[2]], message[order[3]]);
        mix(v, 2, 6, 10, 14, message[order[4]], message[order[5]]);
        mix(v, 3, 7, 11, 15, message[order[6]], message[order[7]]);
        mix(v, 0, 5, 10, 15, message[order[8]], message[order[9]]);
        mix(v, 1, 6, 11, 12, message[order[10]], message[order[11]]);
        mix(v, 2, 7, 8, 13, message[order[12]], message[order[13]]);
        mix(v, 3, 4, 9, 14, message[order[14]], message[order[15]]);
    }
    for (std::size_t word = 0; word < 8; ++word) state_[word] ^= v[word] ^ v[word + 8];
}

// A full buffer is compressed only when more input follows, since the last
// block is compressed by finish with the final flag.
void Blake2s::update(const std::span<const std::byte> input) noexcept {
    std::size_t offset = 0;
    while (offset < input.size()) {
        if (buffered_ == buffer_.size()) {
            compress(false);
            buffered_ = 0;
        }
        const std::size_t room = buffer_.size() - buffered_;
        const std::size_t left = input.size() - offset;
        const std::size_t taken = left < room ? left : room;
        std::memcpy(buffer_.data() + buffered_, input.data() + offset, taken);
        buffered_ += taken;
        offset += taken;
    }
}

Digest Blake2s::finish() noexcept {
    std::memset(buffer_.data() + buffered_, 0, buffer_.size() - buffered_);
    compress(true);
    Digest result{};
    for (std::size_t word = 0; word < 8; ++word) {
        for (std::size_t byte = 0; byte < 4; ++byte)
            result.bytes[4 * word + byte] = static_cast<std::byte>((state_[word] >> (8 * byte)) & 0xffU);
    }
    secure_erase(buffer_);
    return result;
}

Digest blake2s(const std::span<const std::byte> input) noexcept {
    Blake2s hasher{};
    hasher.update(input);
    const Digest result = hasher.finish();
    return result;
}

Digest blake2s_keyed(const std::span<const std::byte> key, const std::span<const std::byte> input) noexcept {
    Blake2s hasher(key);
    hasher.update(input);
    const Digest result = hasher.finish();
    return result;
}

void secure_erase(const std::span<std::byte> bytes) noexcept {
    volatile std::byte* target = bytes.data();
    for (std::size_t index = 0; index < bytes.size(); ++index) target[index] = std::byte{0};
}

} // namespace stx_auxlib::hash
