/* ── Stasha Thread Runtime ──────────────────────────────────────────────────
 *
 * High-level thread pool + future system.
 * Compiled separately and linked into every Stasha executable.
 *
 * API used by compiler-generated code:
 *
 *   __future_t *__thread_dispatch(fn, args, result_size)
 *       Submit a job to the thread pool. Returns a future handle.
 *       fn:          wrapper function: void fn(void *args, void *result)
 *       args:        heap-allocated argument struct (freed by the worker)
 *       result_size: size of return value in bytes (0 for void functions)
 *
 *   void  *__future_get(__future_t *)     block, return ptr to result
 *   void   __future_wait(__future_t *)    block, discard result
 *   int    __future_ready(__future_t *)   non-blocking: 1=done, 0=pending
 *   void   __future_drop(__future_t *)    wait + free future + result
 *
 * Thread pool:
 *   Auto-initialised on first call to __thread_dispatch (or via
 *   __attribute__((constructor))).  Workers = min(nCPU, 64).
 *   Auto-shutdown via __attribute__((destructor)).
 * ── */

#ifndef STASHA_THREAD_RUNTIME_H
#define STASHA_THREAD_RUNTIME_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque future handle — do not touch fields directly. */
typedef struct __future __future_t;

/* ── Public API ── */

/* Submit a job. Returned future must be freed with __future_drop when done. */
__future_t *__thread_dispatch(
    void (*fn)(void *args, void *result),
    void *args,
    size_t result_size
);

/* Block until done; return pointer to result buffer (NULL if void return). */
void *__future_get(__future_t *f);

/* Block until done; discard return value. */
void __future_wait(__future_t *f);

/* Non-blocking: returns 1 if the job finished, 0 if still running. */
int __future_ready(__future_t *f);

/* Wait for completion, then free the future and its result buffer. */
void __future_drop(__future_t *f);

/* Manual pool management (called automatically via constructor/destructor). */
void __thread_runtime_init(int num_threads);
void __thread_runtime_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* STASHA_THREAD_RUNTIME_H */
