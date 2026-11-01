// Conformance tests for the atomic pool, written against the reference's C
// interface (tests/reference/atomic_pool.h).
//
// One source, two executables: stx_auxlib_thread_pool_conformance links
// tests/reference/atomic_pool_adapter.cpp, which implements that interface
// over stx_auxlib::AtomicPool, and stx_auxlib_thread_pool_reference_conformance
// links the original C implementation (tests/reference/atomic_pool.c). Both
// must pass every check, so each expectation below is one the reference meets.
//
// Deterministic properties are checked exactly: argument and task
// validation, which thread runs a batch, the caller-path threshold, and the
// order of a one-worker batch. Concurrent batches are checked for exactly
// one execution of every task, worker-only execution, and serialization.
//
// `--bench` prints a timing table instead of testing; compare the two
// executables' tables on one idle machine.

#include "atomic_pool.h"

#include "../src/checked.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void require(const bool condition, const char* const message) {
    if (!condition) throw std::runtime_error(message);
}

// The caller-path threshold is thread_count * 64 tasks (atomic_pool.c,
// MINIMUM_TASKS_PER_WORKER); a synchronous batch at or below it runs on the
// calling thread.
constexpr std::uint32_t minimum_tasks_per_worker = 64;

// A recording task: writes which thread ran it and in what order. `order`
// has one slot per task; `next_slot` hands slots out in execution order.
struct Recorder final {
    std::vector<std::uint32_t> order{};
    std::vector<std::thread::id> runner{};
    std::vector<std::atomic<std::uint32_t>> executions;
    std::atomic<std::uint32_t> next_slot{0};
    std::atomic<std::uint32_t> worker_reports{0};
    gui_forms_atomic_pool_t* pool{nullptr};

    explicit Recorder(const std::uint32_t count)
        : order(count, 0), runner(count), executions(count) {
        for (std::uint32_t index = 0; index < count; ++index)
            executions[index].store(0, std::memory_order_relaxed);
    }
};

struct RecordedTask final {
    Recorder* recorder{nullptr};
    std::uint32_t index{0};
};

void record(void* const address) {
    RecordedTask& task = *static_cast<RecordedTask*>(address);
    Recorder& recorder = *task.recorder;
    const std::uint32_t slot = recorder.next_slot.fetch_add(1, std::memory_order_relaxed);
    recorder.order[slot] = task.index;
    recorder.runner[task.index] = std::this_thread::get_id();
    recorder.executions[task.index].fetch_add(1, std::memory_order_relaxed);
    if (gui_forms_atomic_pool_is_worker(recorder.pool) != 0)
        recorder.worker_reports.fetch_add(1, std::memory_order_relaxed);
}

// Task storage for a recorded batch. `contexts` is filled before `tasks`
// borrows its elements and is never resized afterwards.
struct RecordedBatch final {
    Recorder recorder;
    std::vector<RecordedTask> contexts{};
    std::vector<gui_forms_atomic_pool_task_t> tasks{};

    RecordedBatch(gui_forms_atomic_pool_t* const pool, const std::uint32_t count)
        : recorder(count), contexts(count), tasks(count) {
        recorder.pool = pool;
        for (std::uint32_t index = 0; index < count; ++index) {
            contexts[index].recorder = &recorder;
            contexts[index].index = index;
            tasks[index].function = record;
            tasks[index].argument = &contexts[index];
        }
    }
};

void require_each_once(const Recorder& recorder, const char* const message) {
    const std::size_t count = recorder.executions.size();
    for (std::size_t index = 0; index < count; ++index)
        require(recorder.executions[index].load(std::memory_order_relaxed) == 1, message);
    require(recorder.next_slot.load(std::memory_order_relaxed) == count, message);
}

void require_ascending(const Recorder& recorder, const char* const message) {
    for (std::size_t slot = 0; slot < recorder.order.size(); ++slot)
        require(recorder.order[slot] == slot, message);
}

void require_descending(const Recorder& recorder, const char* const message) {
    const std::size_t count = recorder.order.size();
    for (std::size_t slot = 0; slot < count; ++slot)
        require(recorder.order[slot] == count - 1 - slot, message);
}

void require_runner(const Recorder& recorder, const std::thread::id expected,
                    const char* const message) {
    for (const std::thread::id runner : recorder.runner)
        require(runner == expected, message);
}

void require_never_runner(const Recorder& recorder, const std::thread::id excluded,
                          const char* const message) {
    for (const std::thread::id runner : recorder.runner)
        require(runner != excluded, message);
}

std::size_t distinct_runners(const Recorder& recorder) {
    std::vector<std::thread::id> seen{};
    for (const std::thread::id runner : recorder.runner) {
        bool known = false;
        for (const std::thread::id other : seen) {
            if (other == runner) known = true;
        }
        if (!known) seen.push_back(runner);
    }
    const std::size_t result = seen.size();
    return result;
}

// Owns one pool for a test; destroys it on every exit.
struct PoolOwner final {
    gui_forms_atomic_pool_t* pool{nullptr};

    explicit PoolOwner(const std::uint32_t threads) {
        const gui_forms_atomic_pool_result_t result =
            gui_forms_atomic_pool_create_checked(threads, &pool);
        require(result == THREADPOOL_OK && pool != nullptr, "pool creation failed");
    }
    ~PoolOwner() { gui_forms_atomic_pool_destroy(pool); }
    PoolOwner(const PoolOwner&) = delete;
    PoolOwner& operator=(const PoolOwner&) = delete;
};

void count_call(void* const address) {
    std::atomic<std::uint32_t>& calls = *static_cast<std::atomic<std::uint32_t>*>(address);
    calls.fetch_add(1, std::memory_order_relaxed);
}

void creation_rules() {
    gui_forms_atomic_pool_t* const sentinel = reinterpret_cast<gui_forms_atomic_pool_t*>(
        static_cast<std::uintptr_t>(0x10));
    gui_forms_atomic_pool_t* output = sentinel;
    require(gui_forms_atomic_pool_create_checked(4, nullptr) == THREADPOOL_INVALID_ARGUMENT,
            "creation accepted a null output");
    require(gui_forms_atomic_pool_create_checked(0, &output) == THREADPOOL_INVALID_ARGUMENT,
            "creation accepted zero threads");
    // The reference returns before clearing the output on invalid arguments.
    require(output == sentinel, "rejected creation changed its output");
    require(gui_forms_atomic_pool_create(0) == nullptr, "convenience creation accepted zero");
    require(gui_forms_atomic_pool_thread_count(nullptr) == 0, "null pool reported threads");
    gui_forms_atomic_pool_destroy(nullptr);

    for (std::uint32_t threads = 1; threads <= 8; ++threads) {
        output = sentinel;
        require(gui_forms_atomic_pool_create_checked(threads, &output) == THREADPOOL_OK,
                "creation failed");
        require(output != nullptr && output != sentinel, "creation left no pool");
        require(gui_forms_atomic_pool_thread_count(output) == threads, "thread count changed");
        require(gui_forms_atomic_pool_is_worker(output) == 0, "caller reported as worker");
        gui_forms_atomic_pool_destroy(output);
    }
    gui_forms_atomic_pool_t* const convenient = gui_forms_atomic_pool_create(3);
    require(convenient != nullptr, "convenience creation failed");
    require(gui_forms_atomic_pool_thread_count(convenient) == 3, "convenience count changed");
    gui_forms_atomic_pool_destroy(convenient);
}

void validation_rules() {
    PoolOwner owner(4);
    std::atomic<std::uint32_t> calls{0};
    std::vector<gui_forms_atomic_pool_task_t> tasks(600);
    for (gui_forms_atomic_pool_task_t& task : tasks) {
        task.function = count_call;
        task.argument = &calls;
    }
    require(gui_forms_atomic_pool_run_checked(nullptr, tasks.data(), 1) ==
                THREADPOOL_INVALID_ARGUMENT, "run accepted a null pool");
    // An empty synchronous batch succeeds before its task pointer is read.
    require(gui_forms_atomic_pool_run_checked(owner.pool, nullptr, 0) == THREADPOOL_OK,
            "empty run failed");
    require(gui_forms_atomic_pool_run_checked(owner.pool, nullptr, 1) ==
                THREADPOOL_INVALID_ARGUMENT, "run accepted null tasks");
    require(gui_forms_atomic_pool_begin_checked(nullptr, tasks.data(), 1) ==
                THREADPOOL_INVALID_ARGUMENT, "begin accepted a null pool");
    // Unlike run, an empty asynchronous batch is invalid.
    require(gui_forms_atomic_pool_begin_checked(owner.pool, tasks.data(), 0) ==
                THREADPOOL_INVALID_ARGUMENT, "begin accepted an empty batch");
    require(gui_forms_atomic_pool_begin_checked(owner.pool, nullptr, 1) ==
                THREADPOOL_INVALID_ARGUMENT, "begin accepted null tasks");
    require(gui_forms_atomic_pool_wait_checked(nullptr) == THREADPOOL_INVALID_ARGUMENT,
            "wait accepted a null pool");

    // A null function anywhere rejects the whole batch before any task runs,
    // on both the caller path (3 tasks) and the worker path (600 tasks).
    const std::uint32_t sizes[] = {3, 600};
    for (const std::uint32_t size : sizes) {
        tasks[size - 1].function = nullptr;
        require(gui_forms_atomic_pool_run_checked(owner.pool, tasks.data(), size) ==
                    THREADPOOL_INVALID_TASK, "run accepted a null function");
        require(gui_forms_atomic_pool_begin_checked(owner.pool, tasks.data(), size) ==
                    THREADPOOL_INVALID_TASK, "begin accepted a null function");
        tasks[size - 1].function = count_call;
    }
    require(calls.load(std::memory_order_relaxed) == 0, "a rejected batch ran tasks");

    // The pool is still usable after every rejection.
    require(gui_forms_atomic_pool_run_checked(owner.pool, tasks.data(), 600) == THREADPOOL_OK,
            "run failed after rejections");
    require(calls.load(std::memory_order_relaxed) == 600, "run after rejections lost tasks");
    gui_forms_atomic_pool_run(owner.pool, nullptr, 1);
    gui_forms_atomic_pool_run(nullptr, tasks.data(), 1);
    gui_forms_atomic_pool_run(owner.pool, tasks.data(), 2);
    require(calls.load(std::memory_order_relaxed) == 602, "convenience run miscounted");
}

void caller_path() {
    const std::thread::id caller = std::this_thread::get_id();
    const std::uint32_t thread_counts[] = {2, 3, 4};
    for (const std::uint32_t threads : thread_counts) {
        PoolOwner owner(threads);
        const std::uint32_t threshold = threads * minimum_tasks_per_worker;
        const std::uint32_t sizes[] = {1, 2, threshold - 1, threshold};
        for (const std::uint32_t size : sizes) {
            RecordedBatch batch(owner.pool, size);
            require(gui_forms_atomic_pool_run_checked(owner.pool, batch.tasks.data(), size) ==
                        THREADPOOL_OK, "small run failed");
            require_each_once(batch.recorder, "small run lost or repeated a task");
            require_ascending(batch.recorder, "small run left index order");
            require_runner(batch.recorder, caller, "small run left the caller");
            require(batch.recorder.worker_reports.load(std::memory_order_relaxed) == 0,
                    "caller task reported as worker");
        }
    }
    // A one-thread pool always runs synchronous batches on the caller.
    PoolOwner single(1);
    const std::uint32_t sizes[] = {1, 64, 65, 5000};
    for (const std::uint32_t size : sizes) {
        RecordedBatch batch(single.pool, size);
        require(gui_forms_atomic_pool_run_checked(single.pool, batch.tasks.data(), size) ==
                    THREADPOOL_OK, "one-thread run failed");
        require_each_once(batch.recorder, "one-thread run lost or repeated a task");
        require_ascending(batch.recorder, "one-thread run left index order");
        require_runner(batch.recorder, caller, "one-thread run left the caller");
    }
}

void worker_path() {
    const std::thread::id caller = std::this_thread::get_id();
    const std::uint32_t thread_counts[] = {2, 3, 4, 8};
    for (const std::uint32_t threads : thread_counts) {
        PoolOwner owner(threads);
        const std::uint32_t threshold = threads * minimum_tasks_per_worker;
        const std::uint32_t sizes[] = {threshold + 1, threshold * 2 + 7, 20000};
        for (const std::uint32_t size : sizes) {
            RecordedBatch batch(owner.pool, size);
            require(gui_forms_atomic_pool_run_checked(owner.pool, batch.tasks.data(), size) ==
                        THREADPOOL_OK, "large run failed");
            require_each_once(batch.recorder, "large run lost or repeated a task");
            require_never_runner(batch.recorder, caller, "large run used the caller");
            require(batch.recorder.worker_reports.load(std::memory_order_relaxed) == size,
                    "worker task not reported as worker");
            require(distinct_runners(batch.recorder) <= threads, "more runners than workers");
        }
    }
}

void asynchronous_path() {
    const std::thread::id caller = std::this_thread::get_id();
    // One worker claims from the highest ticket down, so the order is exact.
    PoolOwner single(1);
    const std::uint32_t sizes[] = {1, 2, 7, 8, 9, 64, 300, 2049, 5000};
    for (const std::uint32_t size : sizes) {
        RecordedBatch batch(single.pool, size);
        require(gui_forms_atomic_pool_begin_checked(single.pool, batch.tasks.data(), size) ==
                    THREADPOOL_OK, "begin failed");
        require(gui_forms_atomic_pool_wait_checked(single.pool) == THREADPOOL_OK,
                "wait failed");
        require_each_once(batch.recorder, "async batch lost or repeated a task");
        require_descending(batch.recorder, "one-worker async batch left ticket order");
        require_never_runner(batch.recorder, caller, "async batch used the caller");
    }
    // Every asynchronous batch runs on workers, including one-task batches.
    PoolOwner owner(4);
    const std::uint32_t pooled_sizes[] = {1, 3, 256, 257, 10000};
    for (const std::uint32_t size : pooled_sizes) {
        RecordedBatch batch(owner.pool, size);
        require(gui_forms_atomic_pool_begin_checked(owner.pool, batch.tasks.data(), size) ==
                    THREADPOOL_OK, "pooled begin failed");
        require(gui_forms_atomic_pool_wait_checked(owner.pool) == THREADPOOL_OK,
                "pooled wait failed");
        require_each_once(batch.recorder, "pooled async batch lost or repeated a task");
        require_never_runner(batch.recorder, caller, "pooled async batch used the caller");
        require(batch.recorder.worker_reports.load(std::memory_order_relaxed) == size,
                "async task not reported as worker");
    }
}

// Two owners submit batches to one pool at once. A task of either batch must
// never see a task of the other batch running.
struct Serialization final {
    std::atomic<std::uint32_t> active[2]{};
    std::atomic<std::uint32_t> overlaps{0};
    std::atomic<std::uint32_t> completed{0};
};

struct SerializedTask final {
    Serialization* shared{nullptr};
    std::uint32_t batch{0};
};

void serialized(void* const address) {
    SerializedTask& task = *static_cast<SerializedTask*>(address);
    Serialization& shared = *task.shared;
    const std::uint32_t other = 1 - task.batch;
    shared.active[task.batch].fetch_add(1, std::memory_order_acq_rel);
    if (shared.active[other].load(std::memory_order_acquire) != 0)
        shared.overlaps.fetch_add(1, std::memory_order_relaxed);
    shared.active[task.batch].fetch_sub(1, std::memory_order_acq_rel);
    shared.completed.fetch_add(1, std::memory_order_relaxed);
}

struct Submitter final {
    gui_forms_atomic_pool_t* pool{nullptr};
    std::vector<gui_forms_atomic_pool_task_t> tasks{};
    std::uint32_t rounds{0};
    std::atomic<std::uint32_t> failures{0};
};

void submit_rounds(Submitter* const submitter) {
    Submitter& owner = *submitter;
    const std::uint32_t count = static_cast<std::uint32_t>(owner.tasks.size());
    for (std::uint32_t round = 0; round < owner.rounds; ++round) {
        if (gui_forms_atomic_pool_run_checked(owner.pool, owner.tasks.data(), count) !=
            THREADPOOL_OK)
            owner.failures.fetch_add(1, std::memory_order_relaxed);
    }
}

void concurrent_runs_serialize() {
    PoolOwner owner(4);
    Serialization shared{};
    SerializedTask contexts[2]{};
    Submitter submitters[2]{};
    const std::uint32_t count = 2000;
    const std::uint32_t rounds = 50;
    for (std::uint32_t batch = 0; batch < 2; ++batch) {
        contexts[batch].shared = &shared;
        contexts[batch].batch = batch;
        submitters[batch].pool = owner.pool;
        submitters[batch].rounds = rounds;
        submitters[batch].tasks.resize(count);
        for (gui_forms_atomic_pool_task_t& task : submitters[batch].tasks) {
            task.function = serialized;
            task.argument = &contexts[batch];
        }
    }
    std::thread first(submit_rounds, &submitters[0]);
    std::thread second(submit_rounds, &submitters[1]);
    first.join();
    second.join();
    require(submitters[0].failures.load() == 0 && submitters[1].failures.load() == 0,
            "a concurrent run failed");
    require(shared.overlaps.load() == 0, "concurrent batches overlapped");
    require(shared.completed.load() == 2 * count * rounds, "concurrent runs lost tasks");
}

void increment(void* const address) {
    std::uint32_t& value = *static_cast<std::uint32_t*>(address);
    ++value;
}

void reuse_and_lifetime() {
    // One pool, many rounds of both paths: each value is touched once a round.
    PoolOwner owner(4);
    std::vector<std::uint32_t> values(9000, 0);
    std::vector<gui_forms_atomic_pool_task_t> tasks(values.size());
    for (std::size_t index = 0; index < values.size(); ++index) {
        tasks[index].function = increment;
        tasks[index].argument = &values[index];
    }
    const std::uint32_t count = static_cast<std::uint32_t>(tasks.size());
    for (std::uint32_t round = 1; round <= 200; ++round) {
        const bool asynchronous = round % 2 == 0;
        if (asynchronous) {
            require(gui_forms_atomic_pool_begin_checked(owner.pool, tasks.data(), count) ==
                        THREADPOOL_OK, "reuse begin failed");
            require(gui_forms_atomic_pool_wait_checked(owner.pool) == THREADPOOL_OK,
                    "reuse wait failed");
        } else {
            require(gui_forms_atomic_pool_run_checked(owner.pool, tasks.data(), count) ==
                        THREADPOOL_OK, "reuse run failed");
        }
        for (const std::uint32_t value : values)
            require(value == round, "reuse lost or repeated a task");
    }
    // Creation and destruction in quick succession, idle and after work.
    for (std::uint32_t cycle = 0; cycle < 200; ++cycle) {
        gui_forms_atomic_pool_t* pool = nullptr;
        require(gui_forms_atomic_pool_create_checked(1 + cycle % 6, &pool) == THREADPOOL_OK,
                "cycle creation failed");
        if (cycle % 2 == 1) {
            require(gui_forms_atomic_pool_run_checked(pool, tasks.data(), count) == THREADPOOL_OK,
                    "cycle run failed");
        }
        gui_forms_atomic_pool_destroy(pool);
    }
}

// A linear congruential step, modulo 2^64.
STX_AUXLIB_MODULAR void benchmark_task(void* const address) {
    std::uint64_t& value = *static_cast<std::uint64_t*>(address);
    value = value * 6364136223846793005ULL + 1442695040888963407ULL;
}

void benchmark() {
    const std::uint32_t threads = std::thread::hardware_concurrency() == 0
                                      ? 4
                                      : std::thread::hardware_concurrency();
    PoolOwner owner(threads);
    std::printf("threads %u\n%10s %10s %14s %14s\n", threads, "tasks", "rounds",
                "run ns/batch", "async ns/batch");
    const std::uint32_t sizes[] = {16, 1024, 4096, 65536, 1048576};
    for (const std::uint32_t size : sizes) {
        std::vector<std::uint64_t> values(size, 1);
        std::vector<gui_forms_atomic_pool_task_t> tasks(size);
        for (std::uint32_t index = 0; index < size; ++index) {
            tasks[index].function = benchmark_task;
            tasks[index].argument = &values[index];
        }
        const std::uint32_t rounds = size >= 65536 ? 50 : 2000;
        const std::chrono::steady_clock::time_point run_start = std::chrono::steady_clock::now();
        for (std::uint32_t round = 0; round < rounds; ++round)
            (void)gui_forms_atomic_pool_run_checked(owner.pool, tasks.data(), size);
        const std::chrono::steady_clock::time_point run_end = std::chrono::steady_clock::now();
        for (std::uint32_t round = 0; round < rounds; ++round) {
            (void)gui_forms_atomic_pool_begin_checked(owner.pool, tasks.data(), size);
            (void)gui_forms_atomic_pool_wait_checked(owner.pool);
        }
        const std::chrono::steady_clock::time_point async_end = std::chrono::steady_clock::now();
        const std::chrono::nanoseconds run_time =
            std::chrono::duration_cast<std::chrono::nanoseconds>(run_end - run_start);
        const std::chrono::nanoseconds async_time =
            std::chrono::duration_cast<std::chrono::nanoseconds>(async_end - run_end);
        std::printf("%10u %10u %14lld %14lld\n", size, rounds,
                    static_cast<long long>(run_time.count() / rounds),
                    static_cast<long long>(async_time.count() / rounds));
    }
}

} // namespace

int main(const int argument_count, char** const arguments) {
    if (argument_count == 2 && std::strcmp(arguments[1], "--bench") == 0) {
        benchmark();
        return 0;
    }
    try {
        creation_rules();
        validation_rules();
        caller_path();
        worker_path();
        asynchronous_path();
        concurrent_runs_serialize();
        reuse_and_lifetime();
    } catch (const std::exception& failure) {
        std::fprintf(stderr, "atomic pool conformance failed: %s\n", failure.what());
        return 1;
    }
    std::puts("atomic pool conformance passed");
    return 0;
}
