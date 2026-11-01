// Copyright (c) 2026 Joshuah Rainstar
// SPDX-License-Identifier: MIT

#ifndef GUI_FORMS_PRIVATE_ATOMIC_POOL_H
#define GUI_FORMS_PRIVATE_ATOMIC_POOL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gui_forms_atomic_pool_t gui_forms_atomic_pool_t;

typedef struct {
    void (*function)(void *);
    void *argument;
} gui_forms_atomic_pool_task_t;

typedef enum {
    THREADPOOL_OK = 0,
    THREADPOOL_INVALID_ARGUMENT = 1,
    THREADPOOL_ALLOCATION_FAILED = 2,
    THREADPOOL_SYSTEM_ERROR = 3,
    THREADPOOL_INVALID_TASK = 4
} gui_forms_atomic_pool_result_t;

/* Checked construction. `thread_count` must be non-zero. */
gui_forms_atomic_pool_result_t gui_forms_atomic_pool_create_checked(uint32_t thread_count,
                                              gui_forms_atomic_pool_t **output);

/* Synchronous batch. Concurrent calls are serialized. Callbacks must return. */
gui_forms_atomic_pool_result_t gui_forms_atomic_pool_run_checked(gui_forms_atomic_pool_t *pool,
                                           const gui_forms_atomic_pool_task_t *tasks,
                                           uint32_t count);

uint32_t gui_forms_atomic_pool_thread_count(const gui_forms_atomic_pool_t *pool);

/* Async dispatch always uses workers, including one-task batches. The owner
   must pair begin/wait on the same thread; tasks remain borrowed until wait.
   Existing synchronous run keeps its measured small-batch caller path. */
gui_forms_atomic_pool_result_t gui_forms_atomic_pool_begin_checked(
    gui_forms_atomic_pool_t *pool, const gui_forms_atomic_pool_task_t *tasks, uint32_t count);
gui_forms_atomic_pool_result_t gui_forms_atomic_pool_wait_checked(gui_forms_atomic_pool_t *pool);
int gui_forms_atomic_pool_is_worker(const gui_forms_atomic_pool_t *pool);

/* Safe lifetime still requires no new API call to begin during destruction. */
void gui_forms_atomic_pool_destroy(gui_forms_atomic_pool_t *pool);

/* Source-compatible conveniences retained from the supplied control. */
gui_forms_atomic_pool_t *gui_forms_atomic_pool_create(uint32_t thread_count);
void gui_forms_atomic_pool_run(gui_forms_atomic_pool_t *pool, gui_forms_atomic_pool_task_t *tasks, uint32_t count);

#ifdef __cplusplus
}
#endif

#endif
