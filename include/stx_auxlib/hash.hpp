#pragma once

// BLAKE2s-256 (RFC 7693): the library's cryptographic hash, used by the
// precedence chain (precedence.hpp). BLAKE2s is fast in portable scalar
// code, which keeps it within reach of hardware-accelerated SHA-256 on
// every host. Output is identical on every host.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace stx_auxlib::hash {

// A 32-byte digest.
struct Digest final {
    std::array<std::byte, 32> bytes{};

    [[nodiscard]] bool operator==(const Digest& other) const noexcept;
    [[nodiscard]] bool operator!=(const Digest& other) const noexcept;
};

// Incremental BLAKE2s with a 32-byte output and an optional key of up to
// 32 bytes (keyed BLAKE2s is a MAC). Feed with update, then finish once;
// finish leaves the object unusable until reset.
class Blake2s final {
public:
    Blake2s() noexcept;
    // `key` longer than 32 bytes is cut to 32.
    explicit Blake2s(std::span<const std::byte> key) noexcept;
    // Erases the key, the state and the buffered input.
    ~Blake2s();
    Blake2s(const Blake2s&) = delete;
    Blake2s& operator=(const Blake2s&) = delete;
    void reset() noexcept;
    void update(std::span<const std::byte> input) noexcept;
    [[nodiscard]] Digest finish() noexcept;

private:
    void compress(bool last) noexcept;
    std::array<std::uint32_t, 8> state_{};
    std::array<std::byte, 64> buffer_{};
    std::size_t buffered_{0};
    // Bytes compressed so far (RFC 7693's t), as a 64-bit counter.
    std::uint64_t counted_{0};
    std::array<std::byte, 32> key_{};
    std::size_t key_length_{0};
};

[[nodiscard]] Digest blake2s(std::span<const std::byte> input) noexcept;
[[nodiscard]] Digest blake2s_keyed(std::span<const std::byte> key, std::span<const std::byte> input) noexcept;

// Overwrites `bytes` with zeros in a way the compiler may not remove, for
// secrets that must not outlive their use.
void secure_erase(std::span<std::byte> bytes) noexcept;

} // namespace stx_auxlib::hash
