// Tests of stx_auxlib's threads and synchronization
// (include/stx_auxlib/threads.hpp).

#include "stx_auxlib/clocks.hpp"
#include "stx_auxlib/threads.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

namespace {

void require(const bool condition, const char* const message) {
    if (condition) return;
    std::fprintf(stderr, "stx_auxlib threads: %s\n", message);
    std::abort();
}

stx_auxlib::Thread start_or_fail(const stx_auxlib::Thread::Entry entry, void* const context) {
    stx_auxlib::ThreadStart start = stx_auxlib::Thread::start(entry, context);
    require(start.succeeded(), "a thread failed to start");
    stx_auxlib::Thread result = start.take_thread();
    return result;
}

// Identities ----------------------------------------------------------------

struct IdentityReport final {
    stx_auxlib::ThreadIdentity seen{};
    stx_auxlib::ThreadIdentity seen_again{};
};

void report_identity(const stx_auxlib::StopSignal&, void* const address) noexcept {
    IdentityReport& report = *static_cast<IdentityReport*>(address);
    report.seen = stx_auxlib::current_thread();
    report.seen_again = stx_auxlib::current_thread();
}

void identities() {
    const stx_auxlib::ThreadIdentity none{};
    require(!none.names_thread() && none.value() == 0, "the default identity names a thread");
    const stx_auxlib::ThreadIdentity main_thread = stx_auxlib::current_thread();
    require(main_thread.names_thread(), "the main thread has no identity");
    require(main_thread == stx_auxlib::current_thread(), "the main thread's identity changed");

    // Sequential threads: each identity is new, never one an ended thread had.
    std::vector<std::uint64_t> issued{main_thread.value()};
    for (int index = 0; index < 64; ++index) {
        IdentityReport report{};
        stx_auxlib::Thread thread = start_or_fail(report_identity, &report);
        const stx_auxlib::ThreadIdentity owner_view = thread.identity();
        require(thread.join() == stx_auxlib::JoinResult::joined, "join failed");
        require(report.seen == owner_view, "the owner and the thread disagree on identity");
        require(report.seen == report.seen_again, "a thread's identity changed");
        for (const std::uint64_t earlier : issued)
            require(earlier != report.seen.value(), "an identity was reused");
        issued.push_back(report.seen.value());
        require(!thread.identity().names_thread(), "a joined thread kept its identity");
    }
}

// Thread lifetime -------------------------------------------------------------

struct StopReport final {
    std::atomic<bool> entered{false};
    std::atomic<bool> left{false};
};

void run_until_stopped(const stx_auxlib::StopSignal& stop, void* const address) noexcept {
    StopReport& report = *static_cast<StopReport*>(address);
    report.entered.store(true, std::memory_order_release);
    while (!stop.requested()) stx_auxlib::yield_thread();
    report.left.store(true, std::memory_order_release);
}

struct SelfJoin final {
    std::atomic<stx_auxlib::Thread*> thread{nullptr};
    std::atomic<int> result{-1};
};

void join_self(const stx_auxlib::StopSignal&, void* const address) noexcept {
    SelfJoin& context = *static_cast<SelfJoin*>(address);
    stx_auxlib::Thread* thread = context.thread.load(std::memory_order_acquire);
    while (thread == nullptr) {
        stx_auxlib::yield_thread();
        thread = context.thread.load(std::memory_order_acquire);
    }
    const stx_auxlib::JoinResult result = (*thread).join();
    context.result.store(static_cast<int>(result), std::memory_order_release);
}

void thread_lifetime() {
    stx_auxlib::ThreadStart rejected = stx_auxlib::Thread::start(nullptr, nullptr);
    require(!rejected.succeeded(), "a null entry started");
    require(rejected.failure() != nullptr &&
                *rejected.failure() == stx_auxlib::ThreadFailure::invalid_entry,
            "a null entry reported the wrong failure");
    stx_auxlib::Thread empty = rejected.take_thread();
    require(empty.join() == stx_auxlib::JoinResult::not_joinable, "an empty thread joined");
    require(!empty.identity().names_thread(), "an empty thread has an identity");
    empty.request_stop();

    // Destruction requests a stop and joins.
    StopReport report{};
    {
        stx_auxlib::Thread thread = start_or_fail(run_until_stopped, &report);
        while (!report.entered.load(std::memory_order_acquire)) stx_auxlib::yield_thread();
    }
    require(report.left.load(std::memory_order_acquire), "destruction did not stop and join");

    // Moving transfers the thread; move assignment finishes the target's own.
    StopReport first{};
    StopReport second{};
    stx_auxlib::Thread a = start_or_fail(run_until_stopped, &first);
    stx_auxlib::Thread b = start_or_fail(run_until_stopped, &second);
    const stx_auxlib::ThreadIdentity b_identity = b.identity();
    stx_auxlib::Thread moved(std::move(a));
    require(!a.identity().names_thread(), "a moved-from thread kept its identity");
    moved = std::move(b);
    require(first.left.load(std::memory_order_acquire), "move assignment left a thread running");
    require(moved.identity() == b_identity, "move assignment adopted the wrong thread");
    moved.request_stop();
    require(moved.join() == stx_auxlib::JoinResult::joined, "the adopted thread did not join");
    require(moved.join() == stx_auxlib::JoinResult::not_joinable, "a thread joined twice");
    require(second.left.load(std::memory_order_acquire), "the adopted thread did not stop");

    // A thread cannot join itself.
    SelfJoin self{};
    stx_auxlib::Thread joiner = start_or_fail(join_self, &self);
    self.thread.store(&joiner, std::memory_order_release);
    while (self.result.load(std::memory_order_acquire) < 0) stx_auxlib::yield_thread();
    require(self.result.load() == static_cast<int>(stx_auxlib::JoinResult::self_join),
            "a self-join was not refused");
    require(joiner.join() == stx_auxlib::JoinResult::joined, "the owner could not join");
}

// Mutual exclusion ------------------------------------------------------------

struct Counter final {
    stx_auxlib::Mutex mutex{};
    std::uint64_t value{0};
};

void count_up(const stx_auxlib::StopSignal&, void* const address) noexcept {
    Counter& counter = *static_cast<Counter*>(address);
    for (int step = 0; step < 20000; ++step) {
        const stx_auxlib::ScopedLock lock(counter.mutex);
        ++counter.value;
    }
}

struct Holder final {
    stx_auxlib::Mutex* mutex{nullptr};
    std::atomic<bool> other_acquired{true};
};

void try_held(const stx_auxlib::StopSignal&, void* const address) noexcept {
    Holder& holder = *static_cast<Holder*>(address);
    const bool acquired = (*holder.mutex).try_lock();
    if (acquired) (*holder.mutex).unlock();
    holder.other_acquired.store(acquired, std::memory_order_release);
}

void mutual_exclusion() {
    Counter counter{};
    {
        std::vector<stx_auxlib::Thread> threads{};
        for (int index = 0; index < 8; ++index)
            threads.push_back(start_or_fail(count_up, &counter));
    }
    require(counter.value == 8 * 20000, "a locked increment was lost");

    stx_auxlib::Mutex mutex{};
    Holder holder{};
    holder.mutex = &mutex;
    {
        const stx_auxlib::ScopedLock lock(mutex);
        stx_auxlib::Thread thread = start_or_fail(try_held, &holder);
        require(thread.join() == stx_auxlib::JoinResult::joined, "join failed");
    }
    require(!holder.other_acquired.load(std::memory_order_acquire),
            "try_lock acquired a held mutex");
    require(mutex.try_lock(), "try_lock failed on a free mutex");
    mutex.unlock();
}

// Conditions ------------------------------------------------------------------

struct Handshake final {
    stx_auxlib::Mutex mutex{};
    stx_auxlib::ConditionVariable changed{};
    int stage{0}; // Guarded by mutex.
};

void answer_handshake(const stx_auxlib::StopSignal&, void* const address) noexcept {
    Handshake& handshake = *static_cast<Handshake*>(address);
    stx_auxlib::ScopedLock lock(handshake.mutex);
    while (handshake.stage != 1) handshake.changed.wait(lock);
    handshake.stage = 2;
    handshake.changed.notify_all();
}

void conditions() {
    Handshake handshake{};
    stx_auxlib::Thread thread = start_or_fail(answer_handshake, &handshake);
    {
        stx_auxlib::ScopedLock lock(handshake.mutex);
        handshake.stage = 1;
        handshake.changed.notify_all();
        // An effectively unbounded wait: the interval saturates the deadline.
        while (handshake.stage != 2) {
            const stx_auxlib::WaitResult result =
                handshake.changed.wait_for(lock, stx_auxlib::MonotonicClock::duration::max());
            require(result == stx_auxlib::WaitResult::woken, "an unbounded wait timed out");
        }
    }
    require(thread.join() == stx_auxlib::JoinResult::joined, "join failed");

    // Timeouts with nobody notifying.
    stx_auxlib::Mutex mutex{};
    stx_auxlib::ConditionVariable never{};
    stx_auxlib::ScopedLock lock(mutex);
    const stx_auxlib::MonotonicClock::time_point before = stx_auxlib::MonotonicClock::now();
    stx_auxlib::WaitResult result = stx_auxlib::WaitResult::woken;
    while (result == stx_auxlib::WaitResult::woken)
        result = never.wait_for(lock, std::chrono::milliseconds(20));
    const stx_auxlib::MonotonicClock::duration waited = stx_auxlib::MonotonicClock::now() - before;
    require(waited >= std::chrono::milliseconds(20), "a timed wait ended early");
    const stx_auxlib::MonotonicClock::time_point past = before - std::chrono::seconds(1);
    require(never.wait_until(lock, past) == stx_auxlib::WaitResult::timed_out,
            "a past deadline did not time out");
    require(never.wait_for(lock, -std::chrono::seconds(1)) == stx_auxlib::WaitResult::timed_out,
            "a negative interval did not time out");
    require(never.wait_for(lock, stx_auxlib::MonotonicClock::duration::min()) ==
                stx_auxlib::WaitResult::timed_out,
            "the most negative interval did not time out");
}

// Once ------------------------------------------------------------------------

struct OnceState final {
    stx_auxlib::OnceFlag flag{};
    std::atomic<int> calls{0};
    int value{0}; // Written once inside call_once; read after it returns.
    std::atomic<int> wrong_reads{0};
};

void initialize_once(void* const address) noexcept {
    OnceState& state = *static_cast<OnceState*>(address);
    state.calls.fetch_add(1, std::memory_order_relaxed);
    stx_auxlib::sleep_for(std::chrono::milliseconds(5));
    state.value = 42;
}

void call_initializer(const stx_auxlib::StopSignal&, void* const address) noexcept {
    OnceState& state = *static_cast<OnceState*>(address);
    stx_auxlib::call_once(state.flag, initialize_once, &state);
    if (state.value != 42) state.wrong_reads.fetch_add(1, std::memory_order_relaxed);
}

void once() {
    OnceState state{};
    {
        std::vector<stx_auxlib::Thread> threads{};
        for (int index = 0; index < 8; ++index)
            threads.push_back(start_or_fail(call_initializer, &state));
    }
    stx_auxlib::call_once(state.flag, initialize_once, &state);
    require(state.calls.load() == 1, "call_once ran its entry more than once");
    require(state.wrong_reads.load() == 0, "a caller returned before initialization finished");
}

// Sleeping and the host -----------------------------------------------------

void sleeping_and_host() {
    const stx_auxlib::MonotonicClock::time_point before = stx_auxlib::MonotonicClock::now();
    stx_auxlib::sleep_for(std::chrono::milliseconds(15));
    const stx_auxlib::MonotonicClock::time_point after = stx_auxlib::MonotonicClock::now();
    require(after - before >= std::chrono::milliseconds(15), "sleep_for returned early");
    stx_auxlib::sleep_until(after + std::chrono::milliseconds(10));
    require(stx_auxlib::MonotonicClock::now() - after >= std::chrono::milliseconds(10),
            "sleep_until returned early");
    const stx_auxlib::MonotonicClock::time_point quick = stx_auxlib::MonotonicClock::now();
    stx_auxlib::sleep_for(-std::chrono::seconds(5));
    stx_auxlib::sleep_until(quick - std::chrono::seconds(5));
    require(stx_auxlib::MonotonicClock::now() - quick < std::chrono::seconds(1),
            "a past sleep blocked");
    require(stx_auxlib::processor_count() >= 1, "no processors reported");
}

} // namespace

int main() {
    identities();
    thread_lifetime();
    mutual_exclusion();
    conditions();
    once();
    sleeping_and_host();
    std::puts("stx_auxlib threads tests passed");
    return 0;
}
