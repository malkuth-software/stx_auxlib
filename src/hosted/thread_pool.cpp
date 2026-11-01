// Copyright (c) 2026 Joshuah Rainstar
// SPDX-License-Identifier: MIT

// The hosted implementation of stx_auxlib::AtomicPool, over std::thread,
// std::mutex and std::condition_variable. It keeps the reference's
// (tests/reference/atomic_pool.c) algorithm, memory orders, result codes and
// lock order.
//
// No exception leaves this file. A failed lock in a call with a result
// returns PoolResult::system_error, as a failed pthread lock does in the
// reference; in a worker or in destruction, which have no result, it
// terminates instead of continuing unlocked.

#include "stx_auxlib/thread_pool.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <span>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>

namespace stx_auxlib {

// Lock order: run_lock, then sleep_lock. run_lock serializes batches and is
// held for a whole batch, across begin and wait for an asynchronous one.
// sleep_lock guards `terminate` and `tasks` and pairs with both conditions.
// Workers hold a pointer to this state, never to the AtomicPool, so moving
// the owner does not disturb them.
struct AtomicPool::State final {
    std::mutex sleep_lock{};
    std::mutex run_lock{};
    std::condition_variable work_cv{};
    std::condition_variable done_cv{};
    std::unique_ptr<std::thread[]> threads{};
    std::uint32_t thread_count{0};
    bool terminate{false};
    // The current batch, borrowed from its caller until run or wait returns.
    const PoolTask* tasks{nullptr};
    // Tickets left to claim. A ticket is a task index plus one; zero or a
    // value above UINT32_MAX means no ticket (claims overshoot past zero).
    std::atomic<std::uint64_t> remaining{0};
    // Tasks of the current batch not yet finished.
    std::atomic<std::uint32_t> pending{0};
    std::atomic<std::uint32_t> claim_size{1};
    // run_lock, held from begin until wait. Only the thread that called
    // begin touches it.
    std::unique_lock<std::mutex> asynchronous_batch{};
};

namespace {

constexpr std::uint64_t claims_per_worker_target = 8;
constexpr std::uint64_t maximum_claim_size = 256;
constexpr std::uint64_t minimum_tasks_per_worker = 64;

bool valid_ticket(const std::uint64_t ticket) noexcept {
    const bool result = ticket != 0 && ticket <= std::numeric_limits<std::uint32_t>::max();
    return result;
}

// Locks `lock`; false when the mutex reports a failure.
[[nodiscard]] bool acquire(std::unique_lock<std::mutex>& lock) noexcept {
    try {
        lock.lock();
    } catch (const std::system_error&) {
        return false;
    }
    return true;
}

void run_worker(AtomicPool::State* const address) noexcept {
    AtomicPool::State& pool = *address;
    std::unique_lock<std::mutex> sleep(pool.sleep_lock);
    for (;;) {
        while (!pool.terminate &&
               !valid_ticket(pool.remaining.load(std::memory_order_relaxed))) {
            pool.work_cv.wait(sleep);
        }
        if (pool.terminate) return;
        sleep.unlock();

        const std::uint64_t ticket = pool.remaining.fetch_sub(1, std::memory_order_acquire);
        if (!valid_ticket(ticket)) {
            sleep.lock();
            continue;
        }

        const PoolTask* const tasks = pool.tasks;
        const std::uint64_t claim_size = pool.claim_size.load(std::memory_order_relaxed);
        const PoolTask& first_task = tasks[ticket - 1];
        first_task.function(first_task.argument);
        std::uint32_t local_done = 1;
        // Claim further tickets in blocks of claim_size, highest first, until
        // the batch's tickets run out.
        while (ticket != 1) {
            const std::uint64_t claim_end =
                pool.remaining.fetch_sub(claim_size, std::memory_order_relaxed);
            if (!valid_ticket(claim_end)) break;
            const std::uint64_t claimed = std::min(claim_end, claim_size);
            const std::uint64_t claim_start = claim_end - claimed;
            for (std::uint64_t claimed_ticket = claim_end; claimed_ticket > claim_start;
                 --claimed_ticket) {
                const PoolTask& task = tasks[claimed_ticket - 1];
                task.function(task.argument);
            }
            local_done += static_cast<std::uint32_t>(claimed);
            if (claim_end <= claim_size) break;
        }

        const std::uint32_t before =
            pool.pending.fetch_sub(local_done, std::memory_order_release);
        assert(before >= local_done);
        sleep.lock();
        if (before == local_done) pool.done_cv.notify_one();
    }
}

// Sets terminate, wakes every worker and joins the first `started` threads.
// The caller holds run_lock (or is creating the pool) and not sleep_lock.
void stop_workers(AtomicPool::State& pool, const std::uint32_t started) noexcept {
    {
        const std::lock_guard<std::mutex> sleep(pool.sleep_lock);
        pool.terminate = true;
        pool.work_cv.notify_all();
    }
    for (std::uint32_t index = 0; index < started; ++index) pool.threads[index].join();
}

// Publishes a validated batch of `count` tasks to the workers. The caller
// holds run_lock and sleep_lock; workers wake once sleep_lock is released.
void publish_batch(AtomicPool::State& pool, const PoolTask* const tasks,
                   const std::uint32_t count) noexcept {
    pool.tasks = tasks;
    const std::uint64_t target_claims =
        static_cast<std::uint64_t>(pool.thread_count) * claims_per_worker_target;
    std::uint64_t claim_size = (static_cast<std::uint64_t>(count) + target_claims - 1) /
                               target_claims;
    claim_size = std::clamp<std::uint64_t>(claim_size, 1, maximum_claim_size);
    pool.pending.store(count, std::memory_order_relaxed);
    pool.claim_size.store(static_cast<std::uint32_t>(claim_size), std::memory_order_relaxed);
    pool.remaining.store(count, std::memory_order_release);
    pool.work_cv.notify_all();
}

// The checks shared by run and begin, which differ only on empty batches.
PoolResult validate_batch(const std::span<const PoolTask> tasks) noexcept {
    if (tasks.size() > std::numeric_limits<std::uint32_t>::max())
        return PoolResult::invalid_argument;
    for (const PoolTask& task : tasks) {
        if (task.function == nullptr) return PoolResult::invalid_task;
    }
    return PoolResult::ok;
}

// Stops the workers of `state` and releases it; null is a no-op.
void destroy_state(std::unique_ptr<AtomicPool::State> state) noexcept {
    if (!state) return;
    AtomicPool::State& owned = *state;
    try {
        const std::lock_guard<std::mutex> run(owned.run_lock);
        stop_workers(owned, owned.thread_count);
    } catch (const std::system_error&) {
        std::terminate();
    }
}

} // namespace

AtomicPool::AtomicPool() noexcept = default;

AtomicPool::AtomicPool(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}

AtomicPool::~AtomicPool() { destroy_state(std::move(state_)); }

AtomicPool::AtomicPool(AtomicPool&& other) noexcept : state_(std::move(other.state_)) {}

AtomicPool& AtomicPool::operator=(AtomicPool&& other) noexcept {
    if (this == &other) return *this;
    destroy_state(std::move(state_));
    state_ = std::move(other.state_);
    return *this;
}

PoolCreation AtomicPool::create(const std::uint32_t thread_count) noexcept {
    if (thread_count == 0) return PoolCreation::failed(PoolResult::invalid_argument);
    std::unique_ptr<State> state{};
    try {
        state.reset(new (std::nothrow) State());
    } catch (const std::system_error&) {
        return PoolCreation::failed(PoolResult::system_error);
    }
    if (!state) return PoolCreation::failed(PoolResult::allocation_failed);
    State& pool = *state;
    pool.threads.reset(new (std::nothrow) std::thread[thread_count]);
    if (!pool.threads) return PoolCreation::failed(PoolResult::allocation_failed);
    pool.thread_count = thread_count;

    std::uint32_t started = 0;
    try {
        for (; started < thread_count; ++started)
            pool.threads[started] = std::thread(run_worker, state.get());
    } catch (const std::system_error&) {
    } catch (const std::bad_alloc&) {
    }
    if (started != thread_count) {
        stop_workers(pool, started);
        return PoolCreation::failed(PoolResult::system_error);
    }
    AtomicPool created(std::move(state));
    return PoolCreation::created(std::move(created));
}

std::uint32_t AtomicPool::thread_count() const noexcept {
    if (!state_) return 0;
    const std::uint32_t result = (*state_).thread_count;
    return result;
}

PoolResult AtomicPool::run(const std::span<const PoolTask> tasks) noexcept {
    if (!state_) return PoolResult::invalid_argument;
    if (tasks.empty()) return PoolResult::ok;
    const PoolResult validation = validate_batch(tasks);
    if (validation != PoolResult::ok) return validation;
    State& pool = *state_;
    const std::uint32_t count = static_cast<std::uint32_t>(tasks.size());

    std::unique_lock<std::mutex> run_hold(pool.run_lock, std::defer_lock);
    if (!acquire(run_hold)) return PoolResult::system_error;
    // Small batches, and every batch of a one-thread pool, run on the caller
    // in index order; waking workers would cost more than the batch.
    const std::uint64_t caller_limit =
        static_cast<std::uint64_t>(pool.thread_count) * minimum_tasks_per_worker;
    if (pool.thread_count == 1 || static_cast<std::uint64_t>(count) <= caller_limit) {
        for (std::uint32_t index = 0; index < count; ++index)
            tasks[index].function(tasks[index].argument);
        return PoolResult::ok;
    }
    std::unique_lock<std::mutex> sleep(pool.sleep_lock, std::defer_lock);
    if (!acquire(sleep)) return PoolResult::system_error;
    publish_batch(pool, tasks.data(), count);
    while (pool.pending.load(std::memory_order_acquire) != 0) pool.done_cv.wait(sleep);
    pool.tasks = nullptr;
    return PoolResult::ok;
}

PoolResult AtomicPool::begin(const std::span<const PoolTask> tasks) noexcept {
    if (!state_ || tasks.empty()) return PoolResult::invalid_argument;
    const PoolResult validation = validate_batch(tasks);
    if (validation != PoolResult::ok) return validation;
    State& pool = *state_;
    const std::uint32_t count = static_cast<std::uint32_t>(tasks.size());

    std::unique_lock<std::mutex> run_hold(pool.run_lock, std::defer_lock);
    if (!acquire(run_hold)) return PoolResult::system_error;
    {
        std::unique_lock<std::mutex> sleep(pool.sleep_lock, std::defer_lock);
        if (!acquire(sleep)) return PoolResult::system_error;
        publish_batch(pool, tasks.data(), count);
    }
    pool.asynchronous_batch = std::move(run_hold);
    return PoolResult::ok;
}

PoolResult AtomicPool::wait() noexcept {
    if (!state_) return PoolResult::invalid_argument;
    State& pool = *state_;
    {
        std::unique_lock<std::mutex> sleep(pool.sleep_lock, std::defer_lock);
        if (!acquire(sleep)) return PoolResult::system_error;
        while (pool.pending.load(std::memory_order_acquire) != 0) pool.done_cv.wait(sleep);
        pool.tasks = nullptr;
    }
    // Releases run_lock after sleep_lock, as the reference does. A wait with
    // no batch begun holds no run_lock and releases nothing (the reference
    // would unlock a mutex it does not hold).
    const std::unique_lock<std::mutex> finished = std::move(pool.asynchronous_batch);
    return PoolResult::ok;
}

bool AtomicPool::is_worker() const noexcept {
    if (!state_) return false;
    const State& pool = *state_;
    const std::thread::id caller = std::this_thread::get_id();
    for (std::uint32_t index = 0; index < pool.thread_count; ++index) {
        if (pool.threads[index].get_id() == caller) return true;
    }
    return false;
}

PoolCreation::PoolCreation(State state) noexcept : state_(std::move(state)) {}

PoolCreation PoolCreation::created(AtomicPool pool) noexcept {
    PoolCreation result(State(std::in_place_index<0>, std::move(pool)));
    return result;
}

PoolCreation PoolCreation::failed(const PoolResult failure) noexcept {
    PoolCreation result(State(std::in_place_index<1>, failure));
    return result;
}

bool PoolCreation::succeeded() const noexcept {
    const bool result = std::holds_alternative<AtomicPool>(state_);
    return result;
}

PoolResult PoolCreation::failure() const noexcept {
    const PoolResult* const failure = std::get_if<PoolResult>(&state_);
    if (failure == nullptr) return PoolResult::ok;
    return *failure;
}

AtomicPool PoolCreation::take_pool() noexcept {
    AtomicPool* const pool = std::get_if<AtomicPool>(&state_);
    if (pool == nullptr) return AtomicPool();
    AtomicPool result(std::move(*pool));
    return result;
}

} // namespace stx_auxlib
