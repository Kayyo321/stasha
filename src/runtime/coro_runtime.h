/* ── Stasha Coroutine/Async Runtime ────────────────────────────────────────
 *
 * Dedicated executor-backed runtime for `async fn` task handles.
 * This replaces the old thread runtime as the backing implementation for
 * `async.()` / `await(...)` / `await.all(...)` / `await.any(...)`.
 *
 * v1 of the runtime still executes each spawned task to completion on a
 * worker thread. It does not yet expose compiler-lowered in-function suspend
 * points, but it provides the task-handle ABI, executor queue, cancellation
 * flag, and destruction semantics needed by the coroutine migration.
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
