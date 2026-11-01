// The hosted threads and synchronization (include/stx_auxlib/threads.hpp),
// over std::thread, std::mutex and std::condition_variable.
//
// The host objects live in the public types' fixed storage, constructed by
// placement new and reached through std::launder; the static assertions
// below check that each fits. Host operations that report errors by
// exception (a failed lock or wait) are defects here: every function is
// noexcept, so such an exception terminates the program.
//
// MonotonicClock reads std::chrono::steady_clock (clocks.cpp), so its time
// points convert to steady_clock time points by duration alone.

#include "stx_auxlib/threads.hpp"

#include "stx_auxlib/clocks.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>

namespace stx_auxlib {

namespace {

static_assert(sizeof(std::mutex) <= 64 && alignof(std::mutex) <= 16,
              "Mutex storage is too small for the host mutex");
static_assert(sizeof(std::condition_variable) <= 64 && alignof(std::condition_variable) <= 16,
              "ConditionVariable storage is too small for the host condition");

std::mutex& host_mutex(std::byte* const storage) noexcept {
    std::mutex& result = *std::launder(reinterpret_cast<std::mutex*>(storage));
    return result;
}

std::condition_variable& host_condition(std::byte* const storage) noexcept {
    std::condition_variable& result =
        *std::launder(reinterpret_cast<std::condition_variable*>(storage));
    return result;
}

std::chrono::steady_clock::time_point host_deadline(
    const MonotonicClock::time_point deadline) noexcept {
    const std::chrono::steady_clock::duration since_epoch =
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            deadline.time_since_epoch());
    const std::chrono::steady_clock::time_point result(since_epoch);
    return result;
}

// now + interval, saturating at the clock's range instead of overflowing.
MonotonicClock::time_point deadline_after(const MonotonicClock::duration interval) noexcept {
    const MonotonicClock::time_point now = MonotonicClock::now();
    const MonotonicClock::rep elapsed = now.time_since_epoch().count();
    const MonotonicClock::rep step = interval.count();
    const MonotonicClock::rep largest = std::numeric_limits<MonotonicClock::rep>::max();
    const MonotonicClock::rep smallest = std::numeric_limits<MonotonicClock::rep>::min();
    MonotonicClock::rep sum = 0;
    if (step > 0 && elapsed > largest - step) {
        sum = largest;
    } else if (step < 0 && elapsed < smallest - step) {
        sum = smallest;
    } else {
        sum = elapsed + step;
    }
    const MonotonicClock::time_point result{MonotonicClock::duration(sum)};
    return result;
}

std::atomic<std::uint64_t> next_identity{1};

// The calling thread's identity; zero until issued. A Thread's host thread
// adopts the identity its owner issued before calling the entry.
thread_local std::uint64_t calling_identity = 0;

} // namespace

ThreadIdentity::ThreadIdentity(const std::uint64_t value) noexcept : value_(value) {}

bool ThreadIdentity::names_thread() const noexcept {
    const bool result = value_ != 0;
    return result;
}

std::uint64_t ThreadIdentity::value() const noexcept { return value_; }

bool ThreadIdentity::operator==(const ThreadIdentity& other) const noexcept {
    const bool result = value_ == other.value_;
    return result;
}

bool ThreadIdentity::operator!=(const ThreadIdentity& other) const noexcept {
    const bool result = value_ != other.value_;
    return result;
}

ThreadIdentity ThreadIdentity::issue() noexcept {
    std::uint64_t value = next_identity.load(std::memory_order_relaxed);
    for (;;) {
        if (value == std::numeric_limits<std::uint64_t>::max()) std::terminate();
        if (next_identity.compare_exchange_weak(value, value + 1, std::memory_order_relaxed))
            break;
    }
    const ThreadIdentity result(value);
    return result;
}

ThreadIdentity current_thread() noexcept {
    if (calling_identity == 0) calling_identity = ThreadIdentity::issue().value();
    const ThreadIdentity result(calling_identity);
    return result;
}

void sleep_for(const MonotonicClock::duration interval) noexcept {
    if (interval <= MonotonicClock::duration::zero()) return;
    std::this_thread::sleep_for(interval);
}

void sleep_until(const MonotonicClock::time_point deadline) noexcept {
    std::this_thread::sleep_until(host_deadline(deadline));
}

void yield_thread() noexcept { std::this_thread::yield(); }

std::uint32_t processor_count() noexcept {
    const unsigned int reported = std::thread::hardware_concurrency();
    if (reported == 0) return 1;
    const std::uint32_t result = static_cast<std::uint32_t>(reported);
    return result;
}

Mutex::Mutex() noexcept { ::new (static_cast<void*>(storage_)) std::mutex(); }

Mutex::~Mutex() { host_mutex(storage_).~mutex(); }

void Mutex::lock() noexcept { host_mutex(storage_).lock(); }

bool Mutex::try_lock() noexcept {
    const bool result = host_mutex(storage_).try_lock();
    return result;
}

void Mutex::unlock() noexcept { host_mutex(storage_).unlock(); }

ConditionVariable::ConditionVariable() noexcept {
    ::new (static_cast<void*>(storage_)) std::condition_variable();
}

ConditionVariable::~ConditionVariable() { host_condition(storage_).~condition_variable(); }

// Each wait adopts the ScopedLock's hold for the host wait and releases the
// adopted hold afterwards, so the ScopedLock still owns the mutex.
void ConditionVariable::wait(ScopedLock& lock) noexcept {
    std::unique_lock<std::mutex> hold(host_mutex(lock.mutex_.storage_), std::adopt_lock);
    host_condition(storage_).wait(hold);
    (void)hold.release();
}

WaitResult ConditionVariable::wait_until(ScopedLock& lock,
                                         const MonotonicClock::time_point deadline) noexcept {
    std::unique_lock<std::mutex> hold(host_mutex(lock.mutex_.storage_), std::adopt_lock);
    const std::cv_status status = host_condition(storage_).wait_until(hold, host_deadline(deadline));
    (void)hold.release();
    if (status == std::cv_status::timeout) return WaitResult::timed_out;
    return WaitResult::woken;
}

WaitResult ConditionVariable::wait_for(ScopedLock& lock,
                                       const MonotonicClock::duration interval) noexcept {
    const WaitResult result = wait_until(lock, deadline_after(interval));
    return result;
}

void ConditionVariable::notify_one() noexcept { host_condition(storage_).notify_one(); }

void ConditionVariable::notify_all() noexcept { host_condition(storage_).notify_all(); }

// The first caller to take the mutex runs `entry`; later callers wait on
// the mutex and then see done_. (call_once's inline acquire load lets
// finished callers skip the mutex.)
void call_once_slow(OnceFlag& flag, void (*const entry)(void* context) noexcept,
                    void* const context) noexcept {
    const ScopedLock lock(flag.mutex_);
    if (flag.done_.load(std::memory_order_relaxed)) return;
    entry(context);
    flag.done_.store(true, std::memory_order_release);
}

bool StopSignal::requested() const noexcept {
    const bool result = requested_.load(std::memory_order_acquire);
    return result;
}

void StopSignal::request() noexcept { requested_.store(true, std::memory_order_release); }

// Owned by the Thread; the host thread borrows it until joined. `joined` is
// the owner's alone.
struct Thread::State final {
    Entry entry{nullptr};
    void* context{nullptr};
    StopSignal stop{};
    ThreadIdentity identity{};
    std::thread host{};
    bool joined{false};
};

namespace {

void run_thread(Thread::State* const address) noexcept {
    Thread::State& state = *address;
    calling_identity = state.identity.value();
    state.entry(state.stop, state.context);
}

// Requests a stop and joins; a thread disposing of its own Thread cannot
// join itself, which is a defect.
void finish_thread(Thread& thread) noexcept {
    thread.request_stop();
    const JoinResult result = thread.join();
    if (result == JoinResult::self_join) std::terminate();
}

} // namespace

Thread::Thread() noexcept = default;

Thread::Thread(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}

Thread::~Thread() { finish_thread(*this); }

Thread::Thread(Thread&& other) noexcept : state_(std::move(other.state_)) {}

Thread& Thread::operator=(Thread&& other) noexcept {
    if (this == &other) return *this;
    finish_thread(*this);
    state_ = std::move(other.state_);
    return *this;
}

ThreadStart Thread::start(const Entry entry, void* const context) noexcept {
    if (entry == nullptr) return ThreadStart::failed(ThreadFailure::invalid_entry);
    std::unique_ptr<State> state(new (std::nothrow) State());
    if (!state) return ThreadStart::failed(ThreadFailure::resources_exhausted);
    State& created = *state;
    created.entry = entry;
    created.context = context;
    created.identity = ThreadIdentity::issue();
    try {
        created.host = std::thread(run_thread, state.get());
    } catch (const std::system_error&) {
        return ThreadStart::failed(ThreadFailure::resources_exhausted);
    } catch (const std::bad_alloc&) {
        return ThreadStart::failed(ThreadFailure::resources_exhausted);
    }
    Thread thread(std::move(state));
    return ThreadStart::started(std::move(thread));
}

ThreadIdentity Thread::identity() const noexcept {
    if (!state_ || (*state_).joined) return ThreadIdentity();
    const ThreadIdentity result = (*state_).identity;
    return result;
}

void Thread::request_stop() noexcept {
    if (!state_) return;
    (*state_).stop.request();
}

JoinResult Thread::join() noexcept {
    if (!state_ || (*state_).joined) return JoinResult::not_joinable;
    State& state = *state_;
    if (current_thread() == state.identity) return JoinResult::self_join;
    state.host.join();
    state.joined = true;
    return JoinResult::joined;
}

ThreadStart::ThreadStart(State state) noexcept : state_(std::move(state)) {}

ThreadStart ThreadStart::started(Thread thread) noexcept {
    ThreadStart result(State(std::in_place_index<0>, std::move(thread)));
    return result;
}

ThreadStart ThreadStart::failed(const ThreadFailure failure) noexcept {
    ThreadStart result(State(std::in_place_index<1>, failure));
    return result;
}

bool ThreadStart::succeeded() const noexcept {
    const bool result = std::holds_alternative<Thread>(state_);
    return result;
}

const ThreadFailure* ThreadStart::failure() const noexcept {
    const ThreadFailure* const result = std::get_if<ThreadFailure>(&state_);
    return result;
}

Thread ThreadStart::take_thread() noexcept {
    Thread* const thread = std::get_if<Thread>(&state_);
    if (thread == nullptr) return Thread();
    Thread result(std::move(*thread));
    return result;
}

} // namespace stx_auxlib
