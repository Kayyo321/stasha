/* ── Stasha Executor-Queue Runtime ─────────────────────────────────────────
 *
 * Thread-pool executor for `thread.(fn)(args)` dispatch.
 * `async fn` coroutines use llvm.coro.* and drive themselves synchronously —
 * they do NOT go through this queue.
 *
 * API surface:
 *   __async_dispatch  — submit a work item, get back a future handle
 *   __async_get/wait  — block until done, retrieve result pointer
 *   __async_ready     — non-blocking done check
 *   __async_cancel    — request cancellation (checked before job starts)
 *   __async_drop      — wait + free the future
 *   __async_wait_any  — wait for the first of N futures to complete
 * ── */

#ifndef STASHA_CORO_RUNTIME_H
#define STASHA_CORO_RUNTIME_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct __async_future __async_future_t;
typedef struct __async_stream __async_stream_t;

__async_future_t *__async_dispatch(
    void (*fn)(void *args, void *result),
    void *args,
    size_t result_size
);

void *__async_get(__async_future_t *f);
void __async_wait(__async_future_t *f);
int  __async_ready(__async_future_t *f);
void __async_drop(__async_future_t *f);
int  __async_wait_any(__async_future_t **fs, int n);

void __async_cancel(__async_future_t *f);
int  __async_cancelled(__async_future_t *f);

void __async_runtime_init(int num_threads);
void __async_runtime_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* STASHA_CORO_RUNTIME_H */
