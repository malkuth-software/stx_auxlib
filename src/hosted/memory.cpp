// stx_auxlib::memory over the linked backend (memory_backend.hpp).
//
// With STX_AUXLIB_ALLOCATION_TAGS (CMakeLists.txt: on in Debug and sanitizer
// builds by default), every block carries a 16-byte header just below the
// address allocate returns:
//
//   backend block ... [offset: 8 bytes][tag: 8 bytes][block of `size` bytes]
//                                                    ^ address
//
// offset is the distance from the backend block's start to the address (a
// power of two, at least 16 and at least the alignment, so the address keeps
// the requested alignment). tag is the address XOR a per-process secret
// whose low four bits are set, so a tag never equals an aligned address.
// release checks the tag, clears it and releases the backend block, so the
// release of a block from anywhere else, or a second release, finds no tag
// and fails with a message instead of corrupting a heap. A foreign free of a
// tagged block hands that allocator an address none of its allocations
// starts at, and it traps (tests/ownership_death_tests.cpp checks all three).
//
// A second release of an address that has since been allocated again is
// indistinguishable from a release of the new block. Without the tags the
// address is the backend block's own and nothing is checked.

#include "stx_auxlib/memory.hpp"

#include "memory_backend.hpp"

#include "stx_auxlib/process.hpp"
#include "stx_auxlib/random.hpp"

#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>

#if defined(STX_AUXLIB_ALLOCATION_TAGS)
#if defined(__SANITIZE_ADDRESS__)
#define STX_AUXLIB_TAGS_UNDER_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define STX_AUXLIB_TAGS_UNDER_ASAN 1
#endif
#endif
#if defined(STX_AUXLIB_TAGS_UNDER_ASAN)
#include <sanitizer/asan_interface.h>
#endif
#endif

namespace stx_auxlib::memory {

#if defined(STX_AUXLIB_ALLOCATION_TAGS)
namespace {

constexpr std::size_t header_bytes = 16;
constexpr std::size_t offset_slot = 16;  // the offset is at address - 16
constexpr std::size_t tag_slot = 8;      // the tag is at address - 8
constexpr std::uint64_t tag_low_bits = 0xf;

std::uint64_t make_process_secret() noexcept {
    std::uint64_t secret = 0;
    std::byte bytes[sizeof(secret)]{};
    const bool drawn = random::fill_entropy(std::span<std::byte>(bytes, sizeof(bytes)));
    if (drawn) {
        std::memcpy(&secret, bytes, sizeof(secret));
    } else {
        // No host entropy: the secret then only has to differ from aligned
        // addresses, which the low bits below guarantee.
        const std::uint64_t ticks =
            static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        secret = ticks * 0x9e3779b97f4a7c15ULL;
    }
    const std::uint64_t result = secret | tag_low_bits;
    return result;
}

// Initialized once, on the first allocation, by a thread-safe static.
std::uint64_t process_secret() noexcept {
    static const std::uint64_t secret = make_process_secret();
    return secret;
}

std::uint64_t tag_for(const std::byte* const address) noexcept {
    const std::uint64_t value = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(address));
    const std::uint64_t result = value ^ process_secret();
    return result;
}

[[noreturn]] void reject_release() noexcept {
    fail("stx_auxlib::memory::release: the block is not from memory::allocate, or was already released");
}

void* allocate_tagged(const std::size_t size, const std::size_t alignment) noexcept {
    const std::size_t offset = alignment < header_bytes ? header_bytes : alignment;
    if (size > std::numeric_limits<std::size_t>::max() - offset) return nullptr;
    void* const block = backend_detail::allocate_aligned_block(size + offset, offset);
    if (block == nullptr) return nullptr;
    std::byte* const address = static_cast<std::byte*>(block) + offset;
    const std::uint64_t offset_value = static_cast<std::uint64_t>(offset);
    const std::uint64_t tag = tag_for(address);
    std::memcpy(address - offset_slot, &offset_value, sizeof(offset_value));
    std::memcpy(address - tag_slot, &tag, sizeof(tag));
    return address;
}

void release_tagged(void* const block) noexcept {
    std::byte* const address = static_cast<std::byte*>(block);
#if defined(STX_AUXLIB_TAGS_UNDER_ASAN)
    // The header of a foreign block is its allocator's red zone, and the
    // header of a released block is quarantined: both are poisoned, and
    // reading them would end in the sanitizer's report instead of ours.
    if (__asan_region_is_poisoned(address - header_bytes, header_bytes) != nullptr) reject_release();
#endif
    std::uint64_t tag = 0;
    std::memcpy(&tag, address - tag_slot, sizeof(tag));
    if (tag != tag_for(address)) reject_release();
    std::uint64_t offset_value = 0;
    std::memcpy(&offset_value, address - offset_slot, sizeof(offset_value));
    const bool offset_valid = offset_value >= header_bytes && std::has_single_bit(offset_value);
    if (!offset_valid) reject_release();
    const std::uint64_t cleared = 0;
    std::memcpy(address - tag_slot, &cleared, sizeof(cleared));
    backend_detail::release_aligned_block(address - static_cast<std::size_t>(offset_value));
}

} // namespace
#endif

Backend backend() noexcept {
    const Backend result = backend_detail::linked_backend;
    return result;
}

bool tags_checked() noexcept {
#if defined(STX_AUXLIB_ALLOCATION_TAGS)
    const bool result = true;
#else
    const bool result = false;
#endif
    return result;
}

// Every block from allocate is an aligned block, whatever its alignment, so
// release needs no record of how it was requested.
void* allocate(const std::size_t size, const std::size_t alignment) noexcept {
    if (!std::has_single_bit(alignment)) return nullptr;
#if defined(STX_AUXLIB_ALLOCATION_TAGS)
    void* const result = allocate_tagged(size, alignment);
#else
    void* const result = backend_detail::allocate_aligned_block(size, alignment);
#endif
    return result;
}

void release(void* const address) noexcept {
    if (address == nullptr) return;
#if defined(STX_AUXLIB_ALLOCATION_TAGS)
    release_tagged(address);
#else
    backend_detail::release_aligned_block(address);
#endif
}

bool in_backend_heap(const void* const address) noexcept {
    const bool result = backend_detail::owns_block(address);
    return result;
}

} // namespace stx_auxlib::memory
