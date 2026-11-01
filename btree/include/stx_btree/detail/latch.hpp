#pragma once

// The latch word of a shared tree's node (bbtree node/latch.rs, D54): from
// the low bit, an exclusive bit, a count of shared holders (20 bits) and a
// version (43 bits) that only grows. Internal nodes are read optimistically:
// read the version, read the node, check the version again. Leaves are
// latched shared or exclusive to change them or run a visitor on an entry.
// Writers that change more than a leaf take internal nodes by
// compare-and-swap at the version their descent read, never waiting.
//
// One word lives in every page at byte 8 for the page's whole life, the root
// page's included, so a version is never seen twice.

#include "stx_btree/detail/inlining.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include "stx_auxlib/threads.hpp"

namespace stx_btree::detail {

// Trivially constructible, so that arrays of held versions are not zeroed
// on every operation; every use site initializes its own.
struct Version final {
    std::uint64_t word;

    [[nodiscard]] bool operator==(const Version& other) const noexcept {
        return word == other.word;
    }
};

// Spinning with exponential backoff, then yielding to the scheduler.
class Backoff final {
public:
    void spin() noexcept {
        if (round < spin_rounds) {
            std::uint32_t step = 0;
            const std::uint32_t steps = static_cast<std::uint32_t>(1) << round;
            for (step = 0; step < steps; step += 1) {
                relax();
            }
            round += 1;
            return;
        }
        // stx_auxlib adaptation: the host yield through stx_auxlib.
        stx_auxlib::yield_thread();
    }

    static void relax() noexcept {
#if defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
        __asm__ __volatile__("yield");
#elif (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
        __asm__ __volatile__("pause");
#endif
    }

private:
    static constexpr std::uint32_t spin_rounds = 10;
    std::uint32_t round = 0;
};

// Operations on the latch word at a page's byte 8. Every access is atomic.
class NodeLatch final {
public:
    static constexpr std::uint64_t exclusive_bit = 1;
    static constexpr std::uint64_t shared_unit = static_cast<std::uint64_t>(1) << 1;
    static constexpr std::uint64_t shared_mask = ((static_cast<std::uint64_t>(1) << 20) - 1) << 1;
    static constexpr std::uint64_t version_unit = static_cast<std::uint64_t>(1) << 21;

    std::uint64_t* word = nullptr;

    [[nodiscard]] static NodeLatch of(std::byte* page) noexcept {
        return NodeLatch{reinterpret_cast<std::uint64_t*>(page + 8)};
    }

    [[nodiscard]] std::atomic_ref<std::uint64_t> atomic() const noexcept {
        return std::atomic_ref<std::uint64_t>(*word);
    }

    // The version, waiting while a writer holds the node.
    [[nodiscard]] STX_BTREE_ALWAYS_INLINE Version read_version() const noexcept {
        const std::uint64_t value = atomic().load(std::memory_order_acquire);
        if ((value & exclusive_bit) == 0) {
            return Version{value & ~shared_mask};
        }
        return read_version_contended();
    }

    [[nodiscard]] STX_BTREE_NEVER_INLINE Version read_version_contended() const noexcept {
        Backoff backoff;
        while (true) {
            backoff.spin();
            const std::uint64_t value = atomic().load(std::memory_order_acquire);
            if ((value & exclusive_bit) == 0) {
                return Version{value & ~shared_mask};
            }
        }
    }

    // Whether nothing changed since `version` was read: what was read in
    // between stands.
    [[nodiscard]] STX_BTREE_ALWAYS_INLINE bool is_unchanged(Version version) const noexcept {
        std::atomic_thread_fence(std::memory_order_acquire);
        const std::uint64_t value = atomic().load(std::memory_order_relaxed);
        return (value & ~shared_mask) == version.word;
    }

    // Takes the node exclusively if it is still at `version` with no
    // holder; never waits.
    [[nodiscard]] bool try_lock_at(Version version) const noexcept {
        std::uint64_t expected = version.word;
        const bool locked = atomic().compare_exchange_strong(
            expected, version.word | exclusive_bit, std::memory_order_acquire, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        return locked;
    }

    // Lets go of an exclusive hold; the version moves on.
    void unlock_exclusive(Version version) const noexcept {
        atomic().store(version.word + version_unit, std::memory_order_release);
    }

    [[nodiscard]] bool try_read_version(Version* out) const noexcept {
        const std::uint64_t value = atomic().load(std::memory_order_acquire);
        if ((value & exclusive_bit) != 0) {
            return false;
        }
        *out = Version{value & ~shared_mask};
        return true;
    }

    // Exclusive without waiting, when nobody holds the node.
    [[nodiscard]] bool try_lock_exclusive(Version* out) const noexcept {
        const std::uint64_t value = atomic().load(std::memory_order_relaxed);
        if ((value & (exclusive_bit | shared_mask)) != 0) {
            return false;
        }
        const Version version{value};
        if (!try_lock_at(version)) {
            return false;
        }
        *out = version;
        return true;
    }

    [[nodiscard]] Version lock_exclusive() const noexcept {
        Version version{0};
        if (try_lock_exclusive(&version)) {
            return version;
        }
        return lock_exclusive_contended();
    }

    [[nodiscard]] STX_BTREE_NEVER_INLINE Version lock_exclusive_contended() const noexcept {
        Backoff backoff;
        while (true) {
            backoff.spin();
            Version version{0};
            if (try_lock_exclusive(&version)) {
                return version;
            }
        }
    }

    void lock_shared() const noexcept {
        std::uint64_t value = atomic().load(std::memory_order_relaxed);
        if ((value & exclusive_bit) == 0 &&
            atomic().compare_exchange_weak(value, value + shared_unit, std::memory_order_acquire, std::memory_order_relaxed)) {
            return;
        }
        lock_shared_contended();
    }

    STX_BTREE_NEVER_INLINE void lock_shared_contended() const noexcept {
        Backoff backoff;
        while (true) {
            std::uint64_t value = atomic().load(std::memory_order_relaxed);
            if ((value & exclusive_bit) == 0 &&
                atomic().compare_exchange_weak(value, value + shared_unit, std::memory_order_acquire, std::memory_order_relaxed)) {
                return;
            }
            backoff.spin();
        }
    }

    void unlock_shared() const noexcept {
        atomic().fetch_sub(shared_unit, std::memory_order_release);
    }
};

// A free list of one kind of page, linked through bytes 16..24 of each
// page (the first inline-key slots), which a freed page no longer needs. A
// page on it keeps its latch word (type-stable nodes, D54).
class FreeList final {
public:
    static constexpr std::size_t link_offset = 16;

    void push(std::byte* page) noexcept {
        lock();
        std::byte* const top_page = top.load(std::memory_order_relaxed);
        *reinterpret_cast<std::byte**>(page + link_offset) = top_page;
        top.store(page, std::memory_order_relaxed);
        unlock();
    }

    [[nodiscard]] std::byte* pop() noexcept {
        if (top.load(std::memory_order_relaxed) == nullptr) {
            return nullptr;
        }
        lock();
        std::byte* const page = top.load(std::memory_order_relaxed);
        if (page != nullptr) {
            std::byte* const next = *reinterpret_cast<std::byte**>(page + link_offset);
            top.store(next, std::memory_order_relaxed);
        }
        unlock();
        return page;
    }

private:
    void lock() noexcept {
        while (true) {
            bool expected = false;
            if (locked.compare_exchange_weak(expected, true, std::memory_order_acquire, std::memory_order_relaxed)) {
                return;
            }
            Backoff::relax();
        }
    }
    void unlock() noexcept {
        locked.store(false, std::memory_order_release);
    }

    std::atomic<bool> locked{false};
    std::atomic<std::byte*> top{nullptr};
};

} // namespace stx_btree::detail
