// Process events on POSIX hosts (include/stx_auxlib/events.hpp), over
// sigaction and pthread_sigmask. The handler only sets bits in one
// lock-free atomic, which is async-signal-safe; handlers are installed
// without SA_RESTART so a blocking read returns EINTR when an event
// arrives.

#include "stx_auxlib/events.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

#include <pthread.h>
#include <signal.h>

namespace stx_auxlib {

namespace {

static_assert(std::atomic<std::uint8_t>::is_always_lock_free,
              "the signal handler needs a lock-free atomic");
static_assert(sizeof(sigset_t) <= 128, "EventDeferral's storage is too small for sigset_t");

struct EventSignal final {
    ProcessEvent event;
    int signal_number;
};

constexpr std::array<EventSignal, 5> event_signals = {{
    {ProcessEvent::interrupt, SIGINT},
    {ProcessEvent::terminate, SIGTERM},
    {ProcessEvent::hangup, SIGHUP},
    {ProcessEvent::terminal_resized, SIGWINCH},
    {ProcessEvent::suspend, SIGTSTP},
}};

std::atomic<std::uint8_t> pending_events{0};
std::atomic<bool> watch_active{false};
// The handling each watched signal had before the watch; written and read
// only by the thread that creates and destroys the watch.
std::array<struct sigaction, 5> previous_actions{};

void record_event(const int signal_number) {
    for (const EventSignal& mapping : event_signals) {
        if (mapping.signal_number == signal_number)
            pending_events.fetch_or(static_cast<std::uint8_t>(mapping.event), std::memory_order_relaxed);
    }
}

sigset_t signals_of(const EventSet events) noexcept {
    sigset_t set{};
    (void)sigemptyset(&set);
    for (const EventSignal& mapping : event_signals) {
        if (events.contains(mapping.event)) (void)sigaddset(&set, mapping.signal_number);
    }
    return set;
}

void restore_watched(const EventSet events) noexcept {
    for (std::size_t index = 0; index < event_signals.size(); ++index) {
        if (events.contains(event_signals[index].event))
            (void)::sigaction(event_signals[index].signal_number, &previous_actions[index], nullptr);
    }
}

} // namespace

EventWatch::EventWatch() noexcept = default;

EventWatch::EventWatch(const EventSet events) noexcept : events_(events) {}

EventWatch::~EventWatch() {
    if (events_.empty()) return;
    restore_watched(events_);
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
    if (watch_active.exchange(true, std::memory_order_acq_rel))
        return CapabilityValue<EventWatch>::failure(CapabilityStatus::busy);
    pending_events.store(0, std::memory_order_relaxed);
    EventSet installed{};
    for (std::size_t index = 0; index < event_signals.size(); ++index) {
        const EventSignal& mapping = event_signals[index];
        if (!events.contains(mapping.event)) continue;
        struct sigaction action{};
        action.sa_handler = record_event;
        (void)sigemptyset(&action.sa_mask);
        action.sa_flags = 0;
        if (::sigaction(mapping.signal_number, &action, &previous_actions[index]) != 0) {
            restore_watched(installed);
            watch_active.store(false, std::memory_order_release);
            return CapabilityValue<EventWatch>::failure(CapabilityStatus::io_error);
        }
        installed = installed.with(mapping.event);
    }
    return CapabilityValue<EventWatch>::success(EventWatch(events));
}

EventSet EventWatch::take() noexcept {
    const std::uint8_t arrived = pending_events.exchange(0, std::memory_order_acquire);
    const EventSet result(static_cast<std::uint8_t>(arrived & events_.bits()));
    return result;
}

EventDeferral::EventDeferral(const EventSet events) noexcept {
    const sigset_t blocked = signals_of(events);
    sigset_t previous{};
    if (::pthread_sigmask(SIG_BLOCK, &blocked, &previous) != 0) return;
    std::memcpy(previous_, &previous, sizeof previous);
    active_ = true;
}

EventDeferral::~EventDeferral() {
    if (!active_) return;
    sigset_t previous{};
    std::memcpy(&previous, previous_, sizeof previous);
    (void)::pthread_sigmask(SIG_SETMASK, &previous, nullptr);
}

void ignore_broken_pipes() noexcept {
    struct sigaction action{};
    action.sa_handler = SIG_IGN;
    (void)sigemptyset(&action.sa_mask);
    (void)::sigaction(SIGPIPE, &action, nullptr);
}

} // namespace stx_auxlib
