#pragma once

// Process events (Capability::interrupt_events, resize_events and
// suspend_events), replacing signal, sigaction, sigprocmask and
// SetConsoleCtrlHandler.
//
// An EventWatch records events as they arrive; the program collects them
// with take() at a point of its choosing, so no program code runs inside a
// signal handler. While a watch is active, blocking reads and waits may
// return early when an event arrives (POSIX: EINTR), so a loop that reads
// standard input notices events promptly.
//
// Host mapping. POSIX: interrupt SIGINT, terminate SIGTERM, hangup SIGHUP,
// terminal_resized SIGWINCH, suspend SIGTSTP. Windows: interrupt
// CTRL_C_EVENT and CTRL_BREAK_EVENT, terminate CTRL_CLOSE_EVENT and
// CTRL_SHUTDOWN_EVENT, hangup CTRL_LOGOFF_EVENT; resize and suspend are not
// signals there and are unsupported. A Windows close or shutdown still ends
// the process a few seconds after the event, whatever the program does.

#include "stx_auxlib/capabilities.hpp"

#include <cstdint>

namespace stx_auxlib {

enum class ProcessEvent : std::uint8_t {
    interrupt = 1,
    terminate = 2,
    hangup = 4,
    terminal_resized = 8,
    suspend = 16,
};

// A set of ProcessEvent values.
class EventSet final {
public:
    constexpr EventSet() noexcept = default;
    constexpr explicit EventSet(const std::uint8_t bits) noexcept : bits_(bits) {}
    [[nodiscard]] constexpr bool contains(const ProcessEvent event) const noexcept {
        const bool result = (bits_ & static_cast<std::uint8_t>(event)) != 0;
        return result;
    }
    [[nodiscard]] constexpr EventSet with(const ProcessEvent event) const noexcept {
        const EventSet result(static_cast<std::uint8_t>(bits_ | static_cast<std::uint8_t>(event)));
        return result;
    }
    [[nodiscard]] constexpr bool empty() const noexcept { return bits_ == 0; }
    [[nodiscard]] constexpr std::uint8_t bits() const noexcept { return bits_; }
    [[nodiscard]] constexpr bool operator==(const EventSet& other) const noexcept {
        const bool result = bits_ == other.bits_;
        return result;
    }

private:
    std::uint8_t bits_{0};
};

// Records the watched events from creation until destruction, replacing
// their default actions (a watched interrupt no longer ends the program),
// and restores the earlier handling when destroyed. One watch at a time:
// a second reports busy. unsupported when `events` includes one the host
// lacks. Move-only.
class EventWatch final {
public:
    [[nodiscard]] static CapabilityValue<EventWatch> watch(EventSet events);
    EventWatch() noexcept;
    ~EventWatch();
    EventWatch(const EventWatch&) = delete;
    EventWatch& operator=(const EventWatch&) = delete;
    EventWatch(EventWatch&& other) noexcept;
    EventWatch& operator=(EventWatch&& other) noexcept;

    // The events that arrived since the last take(), and clears them.
    [[nodiscard]] EventSet take() noexcept;

private:
    explicit EventWatch(EventSet events) noexcept;
    EventSet events_{};
};

// Defers the given events on the calling thread for this object's
// lifetime (a critical section that must not be interrupted); they are
// delivered when it ends. POSIX only; on Windows it has no effect, since
// console events arrive on their own thread.
class EventDeferral final {
public:
    explicit EventDeferral(EventSet events) noexcept;
    ~EventDeferral();
    EventDeferral(const EventDeferral&) = delete;
    EventDeferral& operator=(const EventDeferral&) = delete;

private:
    // The thread's earlier signal mask, as stored by the host (sigset_t is
    // at most 128 bytes on the supported POSIX hosts).
    alignas(8) unsigned char previous_[128]{};
    bool active_{false};
};

// Makes writes to a closed pipe report an error instead of ending the
// process (POSIX: SIGPIPE is ignored). Windows has no such signal; there it
// does nothing.
void ignore_broken_pipes() noexcept;

} // namespace stx_auxlib
