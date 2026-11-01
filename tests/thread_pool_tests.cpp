// Tests of stx_auxlib::AtomicPool's C++ surface that the reference's C
// interface cannot express: the creation record, moving the owner, and the
// empty (moved-from) pool. Scheduling behaviour is covered by
// thread_pool_conformance.cpp.

#include "stx_auxlib/thread_pool.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <utility>
#include <vector>

namespace {

void require(const bool condition, const char* const message) {
    if (condition) return;
    std::fprintf(stderr, "stx_auxlib thread pool: %s\n", message);
    std::abort();
}

void count_call(void* const address) {
    std::atomic<std::uint32_t>& calls = *static_cast<std::atomic<std::uint32_t>*>(address);
    calls.fetch_add(1, std::memory_order_relaxed);
}

std::vector<stx_auxlib::PoolTask> counting_tasks(std::atomic<std::uint32_t>& calls,
                                                 const std::size_t count) {
    std::vector<stx_auxlib::PoolTask> tasks(count);
    for (stx_auxlib::PoolTask& task : tasks) {
        task.function = count_call;
        task.argument = &calls;
    }
    return tasks;
}

void creation_record() {
    stx_auxlib::PoolCreation rejected = stx_auxlib::AtomicPool::create(0);
    require(!rejected.succeeded(), "zero threads created a pool");
    require(rejected.failure() == stx_auxlib::PoolResult::invalid_argument,
            "zero threads reported the wrong failure");
    stx_auxlib::AtomicPool nothing = rejected.take_pool();
    require(nothing.thread_count() == 0, "a failed creation yielded workers");

    stx_auxlib::PoolCreation creation = stx_auxlib::AtomicPool::create(3);
    require(creation.succeeded(), "creation failed");
    require(creation.failure() == stx_auxlib::PoolResult::ok, "success reported a failure");
    stx_auxlib::AtomicPool pool = creation.take_pool();
    require(pool.thread_count() == 3, "taken pool lost its workers");
    stx_auxlib::AtomicPool again = creation.take_pool();
    require(again.thread_count() == 0, "a second take yielded workers");
}

void empty_pool() {
    stx_auxlib::AtomicPool empty{};
    std::atomic<std::uint32_t> calls{0};
    const std::vector<stx_auxlib::PoolTask> tasks = counting_tasks(calls, 4);
    require(empty.thread_count() == 0, "empty pool reported workers");
    require(!empty.is_worker(), "caller of empty pool reported as worker");
    require(empty.run(tasks) == stx_auxlib::PoolResult::invalid_argument,
            "empty pool ran a batch");
    require(empty.begin(tasks) == stx_auxlib::PoolResult::invalid_argument,
            "empty pool began a batch");
    require(empty.wait() == stx_auxlib::PoolResult::invalid_argument, "empty pool waited");
    require(calls.load() == 0, "empty pool ran tasks");
}

void moving_the_owner() {
    std::atomic<std::uint32_t> calls{0};
    const std::vector<stx_auxlib::PoolTask> tasks = counting_tasks(calls, 1000);
    stx_auxlib::PoolCreation creation = stx_auxlib::AtomicPool::create(4);
    stx_auxlib::AtomicPool first = creation.take_pool();
    require(first.run(tasks) == stx_auxlib::PoolResult::ok, "run before move failed");

    stx_auxlib::AtomicPool second(std::move(first));
    require(first.thread_count() == 0, "moved-from pool kept workers");
    require(first.run(tasks) == stx_auxlib::PoolResult::invalid_argument,
            "moved-from pool ran a batch");
    require(second.thread_count() == 4, "moved-to pool lost workers");
    require(second.begin(tasks) == stx_auxlib::PoolResult::ok, "begin after move failed");
    require(second.wait() == stx_auxlib::PoolResult::ok, "wait after move failed");

    // Assignment destroys the target's own workers before adopting the source's.
    stx_auxlib::PoolCreation other_creation = stx_auxlib::AtomicPool::create(2);
    stx_auxlib::AtomicPool target = other_creation.take_pool();
    target = std::move(second);
    require(target.thread_count() == 4, "assigned pool has the wrong workers");
    require(second.thread_count() == 0, "assigned-from pool kept workers");
    require(target.run(tasks) == stx_auxlib::PoolResult::ok, "run after assignment failed");
    require(calls.load() == 3000, "moves lost or repeated tasks");

    // A wait with no batch begun returns without releasing anything.
    require(target.wait() == stx_auxlib::PoolResult::ok, "idle wait failed");
    require(target.run(tasks) == stx_auxlib::PoolResult::ok, "run after idle wait failed");
    require(calls.load() == 4000, "idle wait disturbed the next batch");
}

} // namespace

int main() {
    creation_record();
    empty_pool();
    moving_the_owner();
    std::puts("stx_auxlib thread pool tests passed");
    return 0;
}
