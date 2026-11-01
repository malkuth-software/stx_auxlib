// The reference's C interface (atomic_pool.h) implemented over
// stx_auxlib::AtomicPool, so that tests/thread_pool_conformance.cpp runs one
// set of checks against both the library and atomic_pool.c. Test-only: it
// copies each batch's tasks into PoolTask storage it owns.

#include "atomic_pool.h"

#include "stx_auxlib/thread_pool.hpp"

#include <cstdint>
#include <new>
#include <span>
#include <vector>

struct gui_forms_atomic_pool_t {
    stx_auxlib::AtomicPool pool{};
    // The asynchronous batch's tasks, borrowed by the pool from begin until
    // wait. Only the thread that called begin touches it.
    std::vector<stx_auxlib::PoolTask> asynchronous_tasks{};
};

namespace {

gui_forms_atomic_pool_result_t to_reference(const stx_auxlib::PoolResult result) {
    switch (result) {
    case stx_auxlib::PoolResult::ok:
        return THREADPOOL_OK;
    case stx_auxlib::PoolResult::invalid_argument:
        return THREADPOOL_INVALID_ARGUMENT;
    case stx_auxlib::PoolResult::allocation_failed:
        return THREADPOOL_ALLOCATION_FAILED;
    case stx_auxlib::PoolResult::system_error:
        return THREADPOOL_SYSTEM_ERROR;
    case stx_auxlib::PoolResult::invalid_task:
        return THREADPOOL_INVALID_TASK;
    }
    return THREADPOOL_SYSTEM_ERROR;
}

void copy_tasks(const gui_forms_atomic_pool_task_t* const tasks, const uint32_t count,
                std::vector<stx_auxlib::PoolTask>& output) {
    output.resize(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        output[index].function = tasks[index].function;
        output[index].argument = tasks[index].argument;
    }
}

} // namespace

gui_forms_atomic_pool_result_t gui_forms_atomic_pool_create_checked(
    const uint32_t thread_count, gui_forms_atomic_pool_t** const output) {
    if (output == nullptr || thread_count == 0) return THREADPOOL_INVALID_ARGUMENT;
    *output = nullptr;
    stx_auxlib::PoolCreation creation = stx_auxlib::AtomicPool::create(thread_count);
    if (!creation.succeeded()) return to_reference(creation.failure());
    gui_forms_atomic_pool_t* const adapter = new (std::nothrow) gui_forms_atomic_pool_t();
    if (adapter == nullptr) return THREADPOOL_ALLOCATION_FAILED;
    (*adapter).pool = creation.take_pool();
    *output = adapter;
    return THREADPOOL_OK;
}

gui_forms_atomic_pool_result_t gui_forms_atomic_pool_run_checked(
    gui_forms_atomic_pool_t* const adapter, const gui_forms_atomic_pool_task_t* const tasks,
    const uint32_t count) {
    if (adapter == nullptr) return THREADPOOL_INVALID_ARGUMENT;
    if (count == 0) return THREADPOOL_OK;
    if (tasks == nullptr) return THREADPOOL_INVALID_ARGUMENT;
    // Per-call storage: concurrent runs on one pool must not share it.
    std::vector<stx_auxlib::PoolTask> copied{};
    copy_tasks(tasks, count, copied);
    const stx_auxlib::PoolResult result = (*adapter).pool.run(copied);
    return to_reference(result);
}

uint32_t gui_forms_atomic_pool_thread_count(const gui_forms_atomic_pool_t* const adapter) {
    if (adapter == nullptr) return 0;
    const std::uint32_t result = (*adapter).pool.thread_count();
    return result;
}

gui_forms_atomic_pool_result_t gui_forms_atomic_pool_begin_checked(
    gui_forms_atomic_pool_t* const adapter, const gui_forms_atomic_pool_task_t* const tasks,
    const uint32_t count) {
    if (adapter == nullptr || tasks == nullptr || count == 0) return THREADPOOL_INVALID_ARGUMENT;
    copy_tasks(tasks, count, (*adapter).asynchronous_tasks);
    const stx_auxlib::PoolResult result = (*adapter).pool.begin((*adapter).asynchronous_tasks);
    return to_reference(result);
}

gui_forms_atomic_pool_result_t gui_forms_atomic_pool_wait_checked(
    gui_forms_atomic_pool_t* const adapter) {
    if (adapter == nullptr) return THREADPOOL_INVALID_ARGUMENT;
    const stx_auxlib::PoolResult result = (*adapter).pool.wait();
    return to_reference(result);
}

int gui_forms_atomic_pool_is_worker(const gui_forms_atomic_pool_t* const adapter) {
    if (adapter == nullptr) return 0;
    if ((*adapter).pool.is_worker()) return 1;
    return 0;
}

void gui_forms_atomic_pool_destroy(gui_forms_atomic_pool_t* const adapter) {
    delete adapter;
}

gui_forms_atomic_pool_t* gui_forms_atomic_pool_create(const uint32_t thread_count) {
    gui_forms_atomic_pool_t* pool = nullptr;
    const gui_forms_atomic_pool_result_t result =
        gui_forms_atomic_pool_create_checked(thread_count, &pool);
    if (result != THREADPOOL_OK) return nullptr;
    return pool;
}

void gui_forms_atomic_pool_run(gui_forms_atomic_pool_t* const pool,
                               gui_forms_atomic_pool_task_t* const tasks, const uint32_t count) {
    (void)gui_forms_atomic_pool_run_checked(pool, tasks, count);
}
