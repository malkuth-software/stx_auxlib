// Copyright (c) 2026 Joshuah Rainstar
// SPDX-License-Identifier: MIT

#include "atomic_pool.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

struct gui_forms_atomic_pool_t {
    pthread_mutex_t sleep_lock;
    pthread_mutex_t run_lock;
    pthread_cond_t work_cv;
    pthread_cond_t done_cv;
    pthread_t *threads;
    uint32_t thread_count;
    bool terminate;
    const gui_forms_atomic_pool_task_t *tasks;
    _Atomic uint64_t remaining;
    _Atomic uint32_t pending;
    _Atomic uint32_t claim_size;
};

enum {
    CLAIMS_PER_WORKER_TARGET = 8,
    MAXIMUM_CLAIM_SIZE = 256,
    MINIMUM_TASKS_PER_WORKER = 64
};

static bool valid_ticket(uint64_t ticket)
{
    return ticket != 0 && ticket <= UINT32_MAX;
}

static void *gui_forms_atomic_pool_worker(void *argument)
{
    gui_forms_atomic_pool_t *pool = argument;
    (void)pthread_mutex_lock(&pool->sleep_lock);
    for (;;) {
        while (!pool->terminate &&
               !valid_ticket(atomic_load_explicit(&pool->remaining,
                                                  memory_order_relaxed))) {
            (void)pthread_cond_wait(&pool->work_cv, &pool->sleep_lock);
        }
        if (pool->terminate) {
            (void)pthread_mutex_unlock(&pool->sleep_lock);
            return NULL;
        }
        (void)pthread_mutex_unlock(&pool->sleep_lock);

        const uint64_t ticket = atomic_fetch_sub_explicit(
            &pool->remaining, 1, memory_order_acquire);
        if (!valid_ticket(ticket)) {
            (void)pthread_mutex_lock(&pool->sleep_lock);
            continue;
        }

        const gui_forms_atomic_pool_task_t *tasks = pool->tasks;
        const uint64_t claim_size = atomic_load_explicit(
            &pool->claim_size, memory_order_relaxed);
        const gui_forms_atomic_pool_task_t *first_task = &tasks[ticket - 1];
        first_task->function(first_task->argument);
        uint32_t local_done = 1;
        while (ticket != 1) {
            const uint64_t claim_end = atomic_fetch_sub_explicit(
                &pool->remaining, claim_size, memory_order_relaxed);
            if (!valid_ticket(claim_end))
                break;
            const uint64_t claimed = claim_end < claim_size
                                         ? claim_end
                                         : claim_size;
            const uint64_t claim_start = claim_end - claimed;
            for (uint64_t claimed_ticket = claim_end;
                 claimed_ticket > claim_start; --claimed_ticket) {
                const gui_forms_atomic_pool_task_t *task = &tasks[claimed_ticket - 1];
                task->function(task->argument);
            }
            local_done += (uint32_t)claimed;
            if (claim_end <= claim_size)
                break;
        }

        const uint32_t before = atomic_fetch_sub_explicit(
            &pool->pending, local_done, memory_order_release);
        assert(before >= local_done);
        (void)pthread_mutex_lock(&pool->sleep_lock);
        if (before == local_done)
            (void)pthread_cond_signal(&pool->done_cv);
    }
}

gui_forms_atomic_pool_result_t gui_forms_atomic_pool_create_checked(uint32_t thread_count,
                                              gui_forms_atomic_pool_t **output)
{
    if (!output || thread_count == 0)
        return THREADPOOL_INVALID_ARGUMENT;
    *output = NULL;
    gui_forms_atomic_pool_t *pool = calloc(1, sizeof(*pool));
    if (!pool)
        return THREADPOOL_ALLOCATION_FAILED;
    pool->threads = calloc(thread_count, sizeof(*pool->threads));
    if (!pool->threads) {
        free(pool);
        return THREADPOOL_ALLOCATION_FAILED;
    }
    pool->thread_count = thread_count;
    atomic_init(&pool->remaining, 0);
    atomic_init(&pool->pending, 0);
    atomic_init(&pool->claim_size, 1);

    bool sleep_lock_ready = false;
    bool run_lock_ready = false;
    bool work_cv_ready = false;
    bool done_cv_ready = false;
    if (pthread_mutex_init(&pool->sleep_lock, NULL) == 0)
        sleep_lock_ready = true;
    if (sleep_lock_ready && pthread_mutex_init(&pool->run_lock, NULL) == 0)
        run_lock_ready = true;
    if (run_lock_ready && pthread_cond_init(&pool->work_cv, NULL) == 0)
        work_cv_ready = true;
    if (work_cv_ready && pthread_cond_init(&pool->done_cv, NULL) == 0)
        done_cv_ready = true;
    if (!done_cv_ready) {
        if (work_cv_ready)
            (void)pthread_cond_destroy(&pool->work_cv);
        if (run_lock_ready)
            (void)pthread_mutex_destroy(&pool->run_lock);
        if (sleep_lock_ready)
            (void)pthread_mutex_destroy(&pool->sleep_lock);
        free(pool->threads);
        free(pool);
        return THREADPOOL_SYSTEM_ERROR;
    }

    uint32_t created = 0;
    for (; created < thread_count; ++created) {
        if (pthread_create(&pool->threads[created], NULL,
                           gui_forms_atomic_pool_worker, pool) != 0)
            break;
    }
    if (created != thread_count) {
        (void)pthread_mutex_lock(&pool->sleep_lock);
        pool->terminate = true;
        (void)pthread_cond_broadcast(&pool->work_cv);
        (void)pthread_mutex_unlock(&pool->sleep_lock);
        for (uint32_t index = 0; index < created; ++index)
            (void)pthread_join(pool->threads[index], NULL);
        (void)pthread_cond_destroy(&pool->done_cv);
        (void)pthread_cond_destroy(&pool->work_cv);
        (void)pthread_mutex_destroy(&pool->run_lock);
        (void)pthread_mutex_destroy(&pool->sleep_lock);
        free(pool->threads);
        free(pool);
        return THREADPOOL_SYSTEM_ERROR;
    }
    *output = pool;
    return THREADPOOL_OK;
}

gui_forms_atomic_pool_result_t gui_forms_atomic_pool_run_checked(gui_forms_atomic_pool_t *pool,
                                           const gui_forms_atomic_pool_task_t *tasks,
                                           uint32_t count)
{
    if (!pool)
        return THREADPOOL_INVALID_ARGUMENT;
    if (count == 0)
        return THREADPOOL_OK;
    if (!tasks)
        return THREADPOOL_INVALID_ARGUMENT;
    for (uint32_t index = 0; index < count; ++index) {
        if (!tasks[index].function)
            return THREADPOOL_INVALID_TASK;
    }

    if (pthread_mutex_lock(&pool->run_lock) != 0)
        return THREADPOOL_SYSTEM_ERROR;
    if (pool->thread_count == 1 || (uint64_t)count <=
        (uint64_t)pool->thread_count * MINIMUM_TASKS_PER_WORKER) {
        for (uint32_t index = 0; index < count; ++index)
            tasks[index].function(tasks[index].argument);
        (void)pthread_mutex_unlock(&pool->run_lock);
        return THREADPOOL_OK;
    }
    if (pthread_mutex_lock(&pool->sleep_lock) != 0) {
        (void)pthread_mutex_unlock(&pool->run_lock);
        return THREADPOOL_SYSTEM_ERROR;
    }
    pool->tasks = tasks;
    const uint64_t target_claims =
        (uint64_t)pool->thread_count * CLAIMS_PER_WORKER_TARGET;
    uint64_t claim_size = ((uint64_t)count + target_claims - 1) /
                          target_claims;
    if (claim_size == 0)
        claim_size = 1;
    if (claim_size > MAXIMUM_CLAIM_SIZE)
        claim_size = MAXIMUM_CLAIM_SIZE;
    atomic_store_explicit(&pool->pending, count, memory_order_relaxed);
    atomic_store_explicit(&pool->claim_size, (uint32_t)claim_size,
                          memory_order_relaxed);
    atomic_store_explicit(&pool->remaining, count, memory_order_release);
    (void)pthread_cond_broadcast(&pool->work_cv);
    while (atomic_load_explicit(&pool->pending, memory_order_acquire) != 0)
        (void)pthread_cond_wait(&pool->done_cv, &pool->sleep_lock);
    pool->tasks = NULL;
    (void)pthread_mutex_unlock(&pool->sleep_lock);
    (void)pthread_mutex_unlock(&pool->run_lock);
    return THREADPOOL_OK;
}

uint32_t gui_forms_atomic_pool_thread_count(const gui_forms_atomic_pool_t *pool)
{
    return pool ? pool->thread_count : 0;
}

void gui_forms_atomic_pool_destroy(gui_forms_atomic_pool_t *pool)
{
    if (!pool)
        return;
    (void)pthread_mutex_lock(&pool->run_lock);
    (void)pthread_mutex_lock(&pool->sleep_lock);
    pool->terminate = true;
    (void)pthread_cond_broadcast(&pool->work_cv);
    (void)pthread_mutex_unlock(&pool->sleep_lock);
    for (uint32_t index = 0; index < pool->thread_count; ++index)
        (void)pthread_join(pool->threads[index], NULL);
    (void)pthread_mutex_unlock(&pool->run_lock);
    (void)pthread_cond_destroy(&pool->done_cv);
    (void)pthread_cond_destroy(&pool->work_cv);
    (void)pthread_mutex_destroy(&pool->run_lock);
    (void)pthread_mutex_destroy(&pool->sleep_lock);
    free(pool->threads);
    free(pool);
}

gui_forms_atomic_pool_t *gui_forms_atomic_pool_create(uint32_t thread_count)
{
    gui_forms_atomic_pool_t *pool = NULL;
    return gui_forms_atomic_pool_create_checked(thread_count, &pool) == THREADPOOL_OK
               ? pool
               : NULL;
}

void gui_forms_atomic_pool_run(gui_forms_atomic_pool_t *pool, gui_forms_atomic_pool_task_t *tasks, uint32_t count)
{
    (void)gui_forms_atomic_pool_run_checked(pool, tasks, count);
}

/* GUI.Forms adapter extension: dispatch and wait are separate so a producer
   can request cooperative cancellation before the owner joins it. */
gui_forms_atomic_pool_result_t gui_forms_atomic_pool_begin_checked(
    gui_forms_atomic_pool_t *pool, const gui_forms_atomic_pool_task_t *tasks, uint32_t count)
{
    if (!pool || !tasks || count == 0) return THREADPOOL_INVALID_ARGUMENT;
    for (uint32_t index = 0; index < count; ++index) {
        if (!tasks[index].function) return THREADPOOL_INVALID_TASK;
    }
    if (pthread_mutex_lock(&pool->run_lock) != 0) return THREADPOOL_SYSTEM_ERROR;
    if (pthread_mutex_lock(&pool->sleep_lock) != 0) {
        (void)pthread_mutex_unlock(&pool->run_lock);
        return THREADPOOL_SYSTEM_ERROR;
    }
    pool->tasks = tasks;
    const uint64_t target_claims =
        (uint64_t)pool->thread_count * CLAIMS_PER_WORKER_TARGET;
    uint64_t claim_size = ((uint64_t)count + target_claims - 1) /
                          target_claims;
    if (claim_size == 0)
        claim_size = 1;
    if (claim_size > MAXIMUM_CLAIM_SIZE)
        claim_size = MAXIMUM_CLAIM_SIZE;
    atomic_store_explicit(&pool->pending, count, memory_order_relaxed);
    atomic_store_explicit(&pool->claim_size, (uint32_t)claim_size,
                          memory_order_relaxed);
    atomic_store_explicit(&pool->remaining, count, memory_order_release);
    (void)pthread_cond_broadcast(&pool->work_cv);
    (void)pthread_mutex_unlock(&pool->sleep_lock);
    return THREADPOOL_OK;
}

gui_forms_atomic_pool_result_t gui_forms_atomic_pool_wait_checked(gui_forms_atomic_pool_t *pool)
{
    if (!pool) return THREADPOOL_INVALID_ARGUMENT;
    if (pthread_mutex_lock(&pool->sleep_lock) != 0) return THREADPOOL_SYSTEM_ERROR;
    while (atomic_load_explicit(&pool->pending, memory_order_acquire) != 0)
        (void)pthread_cond_wait(&pool->done_cv, &pool->sleep_lock);
    pool->tasks = NULL;
    (void)pthread_mutex_unlock(&pool->sleep_lock);
    (void)pthread_mutex_unlock(&pool->run_lock);
    return THREADPOOL_OK;
}

int gui_forms_atomic_pool_is_worker(const gui_forms_atomic_pool_t *pool)
{
    const pthread_t caller = pthread_self();
    for (uint32_t index = 0; index < pool->thread_count; ++index) {
        if (pthread_equal(caller, pool->threads[index])) return 1;
    }
    return 0;
}
