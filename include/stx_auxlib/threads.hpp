#pragma once

// Threads and synchronization. No platform type appears here: Mutex and
// ConditionVariable keep their host objects in fixed private storage (src/hosted/threads.cpp checks that they fit), and Thread keeps
// its host thread behind a pointer.
//
// Failure: operations a correct program cannot see fail (locking a mutex
// the thread already holds, a host error in a condition wait) terminate the
// program; they are defects, not results. Starting a thread can fail for
// lack of resources, and reports it.

#include "stx_auxlib/clocks.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <variant>

namespace stx_auxlib {

// A thread's identity. Each thread receives a distinct value on first use;
// a value is never reused, even after its thread ends (std::thread::id can
// be). The default value names no thread.
class ThreadIdentity final {
public:
    ThreadIdentity() noexcept = default;
    [[nodiscard]] bool names_thread() const noexcept;
    // The identity as a number, for logs and hashing; zero names no thread.
    [[nodiscard]] std::uint64_t value() const noexcept;
    [[nodiscard]] bool operator==(const ThreadIdentity& other) const noexcept;
    [[nodiscard]] bool operator!=(const ThreadIdentity& other) const noexcept;

private:
    friend class Thread;
    friend ThreadIdentity current_thread() noexcept;
    // Issues a new identity; terminates when 2^64 - 1 have been issued.
    [[nodiscard]] static ThreadIdentity issue() noexcept;
    explicit ThreadIdentity(std::uint64_t value) noexcept;
    std::uint64_t value_{0};
};

// The calling thread's identity, issued on its first call.
[[nodiscard]] ThreadIdentity current_thread() noexcept;

// Blocks the calling thread for at least `interval`, or until `deadline`;
// a past deadline or a non-positive interval returns at once.
void sleep_for(MonotonicClock::duration interval) noexcept;
void sleep_until(MonotonicClock::time_point deadline) noexcept;
// Offers the rest of the calling thread's time slice to other threads.
void yield_thread() noexcept;
// The number of threads the host can run at once; at least 1 (the host's
// answer when it reports none).
[[nodiscard]] std::uint32_t processor_count() noexcept;

// A non-recursive mutual-exclusion lock. Locking it twice on one thread, or
// unlocking it from a thread that does not hold it, is a defect. Prefer
// ScopedLock; lock, try_lock and unlock are public so that standard lock
// adaptors also accept it.
class Mutex final {
public:
    Mutex() noexcept;
    ~Mutex();
    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;
    Mutex(Mutex&&) = delete;
    Mutex& operator=(Mutex&&) = delete;

    void lock() noexcept;
    [[nodiscard]] bool try_lock() noexcept;
    void unlock() noexcept;

private:
    friend class ConditionVariable;
    alignas(16) std::byte storage_[64];
};

// Holds a Mutex for its own lifetime. A condition wait releases and
// reacquires it, and it is held again when the wait returns.
class ScopedLock final {
public:
    explicit ScopedLock(Mutex& mutex) noexcept : mutex_(mutex) { mutex_.lock(); }
    ~ScopedLock() { mutex_.unlock(); }
    ScopedLock(const ScopedLock&) = delete;
    ScopedLock& operator=(const ScopedLock&) = delete;
    ScopedLock(ScopedLock&&) = delete;
    ScopedLock& operator=(ScopedLock&&) = delete;

private:
    friend class ConditionVariable;
    Mutex& mutex_;
};

enum class WaitResult : std::uint8_t {
    // Woken by a notification or spuriously: recheck the waited-for state.
    woken = 0,
    timed_out = 1,
};

// Waits are always made under a ScopedLock of the mutex that guards the
// waited-for state, in a loop that rechecks that state, since a wait may
// end spuriously.
class ConditionVariable final {
public:
    ConditionVariable() noexcept;
    ~ConditionVariable();
    ConditionVariable(const ConditionVariable&) = delete;
    ConditionVariable& operator=(const ConditionVariable&) = delete;
    ConditionVariable(ConditionVariable&&) = delete;
    ConditionVariable& operator=(ConditionVariable&&) = delete;

    void wait(ScopedLock& lock) noexcept;
    [[nodiscard]] WaitResult wait_until(ScopedLock& lock,
                                        MonotonicClock::time_point deadline) noexcept;
    [[nodiscard]] WaitResult wait_for(ScopedLock& lock,
                                      MonotonicClock::duration interval) noexcept;
    void notify_one() noexcept;
    void notify_all() noexcept;

private:
    alignas(16) std::byte storage_[64];
};

// Runs an initialization once per flag. Every caller returns only after the
// one call of `entry` has finished; concurrent callers wait for it. `entry`
// must not throw, and must not call call_once on the same flag.
//
// Built on Mutex and an atomic rather than std::call_once, so that every
// synchronizing operation is visible to thread sanitizers (libc++ publishes
// call_once's completion inside its uninstrumented runtime).
class OnceFlag final {
public:
    OnceFlag() noexcept = default;
    OnceFlag(const OnceFlag&) = delete;
    OnceFlag& operator=(const OnceFlag&) = delete;
    OnceFlag(OnceFlag&&) = delete;
    OnceFlag& operator=(OnceFlag&&) = delete;

    // True once the initialization has run: the inline fast path of
    // call_once, one acquire load.
    [[nodiscard]] bool done() const noexcept { return done_.load(std::memory_order_acquire); }

private:
    friend void call_once_slow(OnceFlag& flag, void (*entry)(void* context) noexcept,
                               void* context) noexcept;
    // True once `entry` has returned; set with release ordering under mutex_.
    std::atomic<bool> done_{false};
    Mutex mutex_{};
};

// The locked path of call_once, for a flag not yet done.
void call_once_slow(OnceFlag& flag, void (*entry)(void* context) noexcept, void* context) noexcept;

inline void call_once(OnceFlag& flag, void (*const entry)(void* context) noexcept,
                      void* const context) noexcept {
    if (flag.done()) return;
    call_once_slow(flag, entry, context);
}

// A permanent request for a thread to stop, observed cooperatively by its
// entry. A request never interrupts a blocking call. Observing it acquires
// the writes made before request_stop.
class StopSignal final {
public:
    StopSignal() noexcept = default;
    StopSignal(const StopSignal&) = delete;
    StopSignal& operator=(const StopSignal&) = delete;
    [[nodiscard]] bool requested() const noexcept;
    void request() noexcept;

private:
    std::atomic<bool> requested_{false};
};

enum class ThreadFailure : std::uint8_t {
    invalid_entry = 1,
    resources_exhausted = 2,
};

enum class JoinResult : std::uint8_t {
    joined = 0,
    // An empty thread, or one already joined.
    not_joinable = 1,
    // The thread tried to join itself; nothing was done.
    self_join = 2,
};

class ThreadStart;

// One started thread, owned. Its entry receives the thread's StopSignal and
// the context it was started with, and must not throw. The context is
// borrowed until the thread is joined: declare the Thread after what its
// context points to, so it is joined first.
//
// Destruction and move assignment request a stop and join, like
// std::jthread; a thread never outlives its owner. Moving transfers the
// thread and leaves the source empty. identity() is known before the thread
// runs and equals current_thread() inside it.
class Thread final {
public:
    using Entry = void (*)(const StopSignal& stop, void* context) noexcept;

    [[nodiscard]] static ThreadStart start(Entry entry, void* context) noexcept;
    Thread() noexcept;
    ~Thread();
    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;
    Thread(Thread&& other) noexcept;
    Thread& operator=(Thread&& other) noexcept;

    // Empty and joined threads report no identity.
    [[nodiscard]] ThreadIdentity identity() const noexcept;
    // Safe from any thread while the owner keeps the Thread alive.
    void request_stop() noexcept;
    // Blocks until the thread's entry returns. Only the owner calls it.
    [[nodiscard]] JoinResult join() noexcept;

    // The started thread's shared state, defined in src/hosted/threads.cpp.
    struct State;

private:
    explicit Thread(std::unique_ptr<State> state) noexcept;
    std::unique_ptr<State> state_{};
};

// The result of Thread::start: the thread, or the failure. Never both.
class ThreadStart final {
public:
    [[nodiscard]] static ThreadStart started(Thread thread) noexcept;
    [[nodiscard]] static ThreadStart failed(ThreadFailure failure) noexcept;
    [[nodiscard]] bool succeeded() const noexcept;
    // The failure; meaningful only when succeeded() is false.
    [[nodiscard]] const ThreadFailure* failure() const noexcept;
    // Moves the thread out; the record then holds an empty thread. A failed
    // start yields an empty thread.
    [[nodiscard]] Thread take_thread() noexcept;

private:
    using State = std::variant<Thread, ThreadFailure>;
    explicit ThreadStart(State state) noexcept;
    State state_;
};

} // namespace stx_auxlib
