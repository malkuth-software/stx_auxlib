// Process events on Windows (include/stx_auxlib/events.hpp), over
// SetConsoleCtrlHandler. The handler runs on a thread the system creates
// and only sets bits in one atomic. Resize and suspend are not console
// control events, so watching them is unsupported.

#include "stx_auxlib/events.hpp"

#include <atomic>
#include <cstdint>
#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace stx_auxlib {

namespace {

std::atomic<std::uint8_t> pending_events{0};
std::atomic<std::uint8_t> watched_events{0};
std::atomic<bool> watch_active{false};

constexpr std::uint8_t supported_bits = static_cast<std::uint8_t>(ProcessEvent::interrupt) |
                                        static_cast<std::uint8_t>(ProcessEvent::terminate) |
                                        static_cast<std::uint8_t>(ProcessEvent::hangup);

ProcessEvent event_of(const DWORD control) noexcept {
    switch (control) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
        return ProcessEvent::interrupt;
    case CTRL_LOGOFF_EVENT:
        return ProcessEvent::hangup;
    case CTRL_CLOSE_EVENT:
    case CTRL_SHUTDOWN_EVENT:
    default:
        break;
    }
    return ProcessEvent::terminate;
}

// Records a watched event and reports it handled; an unwatched one falls
// through to the next handler (by default, ending the process).
BOOL WINAPI record_event(const DWORD control) {
    const std::uint8_t bit = static_cast<std::uint8_t>(event_of(control));
    if ((watched_events.load(std::memory_order_acquire) & bit) == 0) return FALSE;
    pending_events.fetch_or(bit, std::memory_order_relaxed);
    return TRUE;
}

} // namespace

EventWatch::EventWatch() noexcept = default;

EventWatch::EventWatch(const EventSet events) noexcept : events_(events) {}

EventWatch::~EventWatch() {
    if (events_.empty()) return;
    (void)::SetConsoleCtrlHandler(record_event, FALSE);
    watched_events.store(0, std::memory_order_release);
    pending_events.store(0, std::memory_order_relaxed);
    watch_active.store(false, std::memory_order_release);
}

EventWatch::EventWatch(EventWatch&& other) noexcept : events_(other.events_) {
    other.events_ = EventSet();
}

EventWatch& EventWatch::operator=(EventWatch&& other) noexcept {
    if (this == &other) return *this;
    EventWatch released(std::move(*this));
    events_ = other.events_;
    other.events_ = EventSet();
    return *this;
}

CapabilityValue<EventWatch> EventWatch::watch(const EventSet events) {
    if (events.empty()) return CapabilityValue<EventWatch>::failure(CapabilityStatus::invalid_request);
    if ((events.bits() & ~supported_bits) != 0)
        return CapabilityValue<EventWatch>::failure(CapabilityStatus::unsupported);
    if (watch_active.exchange(true, std::memory_order_acq_rel))
        return CapabilityValue<EventWatch>::failure(CapabilityStatus::busy);
    pending_events.store(0, std::memory_order_relaxed);
    watched_events.store(events.bits(), std::memory_order_release);
    if (!::SetConsoleCtrlHandler(record_event, TRUE)) {
        watched_events.store(0, std::memory_order_release);
        watch_active.store(false, std::memory_order_release);
        return CapabilityValue<EventWatch>::failure(CapabilityStatus::io_error);
    }
    return CapabilityValue<EventWatch>::success(EventWatch(events));
}

EventSet EventWatch::take() noexcept {
    const std::uint8_t arrived = pending_events.exchange(0, std::memory_order_acquire);
    const EventSet result(static_cast<std::uint8_t>(arrived & events_.bits()));
    return result;
}

// Console events arrive on their own thread, so deferring them on the
// calling thread has nothing to do.
EventDeferral::EventDeferral(const EventSet) noexcept {}

EventDeferral::~EventDeferral() {}

void ignore_broken_pipes() noexcept {}

} // namespace stx_auxlib
