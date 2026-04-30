#include "executor.h"

#include <assert.h>
#include <stddef.h>

#define STS_EXECUTOR_QUEUE_CAP 256

typedef void (*sts_coro_fn_t)(void *);

static _Thread_local void *__sts_exec_queue[STS_EXECUTOR_QUEUE_CAP];
static _Thread_local size_t __sts_exec_head = 0;
static _Thread_local size_t __sts_exec_tail = 0;
static _Thread_local size_t __sts_exec_count = 0;

static void __sts_resume_coro(void *coro_h) {
    ((sts_coro_fn_t *)coro_h)[0](coro_h);
}

void __sts_executor_enqueue(void *coro_h) {
    assert(__sts_exec_count < STS_EXECUTOR_QUEUE_CAP);
    __sts_exec_queue[__sts_exec_tail] = coro_h;
    __sts_exec_tail = (__sts_exec_tail + 1) % STS_EXECUTOR_QUEUE_CAP;
    __sts_exec_count++;
}

void __sts_executor_remove(void *coro_h) {
    void *kept_items[STS_EXECUTOR_QUEUE_CAP];
    size_t kept = 0;
    for (size_t i = 0; i < __sts_exec_count; i++) {
        size_t idx = (__sts_exec_head + i) % STS_EXECUTOR_QUEUE_CAP;
        void *queued = __sts_exec_queue[idx];
        if (queued == coro_h)
            continue;
        kept_items[kept] = queued;
        kept++;
    }
    for (size_t i = 0; i < kept; i++)
        __sts_exec_queue[i] = kept_items[i];
    __sts_exec_head = 0;
    __sts_exec_tail = kept % STS_EXECUTOR_QUEUE_CAP;
    __sts_exec_count = kept;
}

int __sts_executor_pending(void) {
    return __sts_exec_count != 0;
}

void __sts_executor_run_pending(void) {
    size_t budget = __sts_exec_count;
    while (budget != 0 && __sts_exec_count != 0) {
        void *coro_h = __sts_exec_queue[__sts_exec_head];
        __sts_exec_head = (__sts_exec_head + 1) % STS_EXECUTOR_QUEUE_CAP;
        __sts_exec_count--;
        budget--;
        __sts_resume_coro(coro_h);
    }
}
