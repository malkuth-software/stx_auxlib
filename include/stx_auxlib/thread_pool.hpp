#pragma once

// The owner's threadpool_atomic_fast algorithm as a C++ owner type. Workers
// claim tickets from one atomic counter in blocks sized to the batch; the
// caller blocks until the batch finishes (run) or until it waits (begin and
// wait). tests/reference/atomic_pool.c is the C implementation this one
// replaced, kept as the reference that tests/thread_pool_conformance.cpp
// checks both against. No platform type appears in this header.

#include <cstdint>
#include <memory>
#include <span>
#include <variant>

namespace stx_auxlib {

// One unit of work. `function` must return normally: a task that throws on a
// worker terminates the program. `argument` is borrowed until the batch that
// holds the task finishes.
struct PoolTask final {
    void (*function)(void* argument){nullptr};
    void* argument{nullptr};
};

enum class PoolResult : std::uint8_t {
    ok = 0,
    invalid_argument = 1,
    allocation_failed = 2,
    system_error = 3,
    invalid_task = 4,
};

class PoolCreation;

// A fixed set of worker threads that run batches of PoolTask.
//
// Ownership: create returns the only owner. Moving transfers the workers and
// leaves the source empty; every call on an empty pool returns
// PoolResult::invalid_argument (thread_count() is 0, is_worker() false).
// Destruction stops and joins every worker. It must not overlap another
// call on the pool, and an asynchronous batch must be finished by wait
// first.
//
// Execution: batches are serialized. run returns after every task of the
// batch has finished. A batch of at most thread_count() * 64 tasks, and
// every batch of a one-thread pool, runs on the calling thread in index
// order; larger batches run on the workers only, in unspecified order.
// begin always uses the workers, including for one task; the thread that
// called begin must call wait. Tasks stay borrowed until run or wait
// returns. A task must not call into its own pool.
//
// Validation happens before any task runs: an empty run succeeds; an empty
// begin, a batch of more than UINT32_MAX tasks, or a task without a function
// is rejected and runs nothing.
class AtomicPool final {
public:
    // thread_count must be non-zero.
    [[nodiscard]] static PoolCreation create(std::uint32_t thread_count) noexcept;
    AtomicPool() noexcept;
    ~AtomicPool();
    AtomicPool(const AtomicPool&) = delete;
    AtomicPool& operator=(const AtomicPool&) = delete;
    AtomicPool(AtomicPool&& other) noexcept;
    // Destroys this pool's workers first, as the destructor does.
    AtomicPool& operator=(AtomicPool&& other) noexcept;

    [[nodiscard]] std::uint32_t thread_count() const noexcept;
    [[nodiscard]] PoolResult run(std::span<const PoolTask> tasks) noexcept;
    [[nodiscard]] PoolResult begin(std::span<const PoolTask> tasks) noexcept;
    [[nodiscard]] PoolResult wait() noexcept;
    // True on one of this pool's worker threads.
    [[nodiscard]] bool is_worker() const noexcept;

    // The workers' shared state, defined in src/hosted/thread_pool.cpp.
    struct State;

private:
    explicit AtomicPool(std::unique_ptr<State> state) noexcept;
    std::unique_ptr<State> state_{};
};

// The result of AtomicPool::create: the pool, or the failure. Never both.
class PoolCreation final {
public:
    [[nodiscard]] static PoolCreation created(AtomicPool pool) noexcept;
    [[nodiscard]] static PoolCreation failed(PoolResult failure) noexcept;
    [[nodiscard]] bool succeeded() const noexcept;
    // The failure; PoolResult::ok when the pool was created.
    [[nodiscard]] PoolResult failure() const noexcept;
    // Moves the pool out; the record then holds an empty pool. A failed
    // creation yields an empty pool.
    [[nodiscard]] AtomicPool take_pool() noexcept;

private:
    using State = std::variant<AtomicPool, PoolResult>;
    explicit PoolCreation(State state) noexcept;
    State state_;
};

} // namespace stx_auxlib
