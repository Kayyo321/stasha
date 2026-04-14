/* ── Stasha Thread Runtime Implementation ───────────────────────────────────
 *
 * Thread pool with a lock-based ring-buffer job queue.
 * Workers: min(nCPU, MAX_WORKERS), spawned once at startup.
 * Job queue: QUEUE_CAP slots (ring buffer + mutex + condvars).
 * Futures:   heap-allocated structs with atomic done flag + condvar.
 *
 * Overhead per dispatch: 1 mutex lock/unlock + 1 condvar signal.
 * Overhead per future_get: 0 if already done (atomic load), else condvar wait.
 * ── */

#include "thread_runtime.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* ── tunables ── */
#define MAX_WORKERS  64
#define QUEUE_CAP    8192   /* must be power-of-two for the ring mask trick */

/* ── future ── */
struct __future {
    _Atomic int     done;    /* 0 = pending, 1 = complete                 */
    void           *result;  /* heap-allocated result buffer (may be NULL) */
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
};

/* ── job ── */
typedef struct {
    void (*fn)(void *args, void *result);
    void       *args;
    __future_t *future;
} job_t;

/* ── global thread pool state ── */
static job_t           __queue[QUEUE_CAP];
static size_t          __q_head  = 0;   /* next slot to consume         */
static size_t          __q_tail  = 0;   /* next slot to produce into    */
static size_t          __q_count = 0;   /* number of queued jobs        */
static pthread_mutex_t __q_mutex     = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  __q_not_empty = PTHREAD_COND_INITIALIZER;
static pthread_cond_t  __q_not_full  = PTHREAD_COND_INITIALIZER;
static _Atomic int     __q_shutdown  = 0;

static pthread_t __workers[MAX_WORKERS];
static int       __nworkers = 0;
static pthread_mutex_t __init_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ── worker thread ── */
static void *worker_thread_fn(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&__q_mutex);
        while (__q_count == 0 && !atomic_load_explicit(&__q_shutdown, memory_order_relaxed))
            pthread_cond_wait(&__q_not_empty, &__q_mutex);

        if (__q_count == 0) {
            /* shutdown signalled and queue is empty — exit */
            pthread_mutex_unlock(&__q_mutex);
            return NULL;
        }

        job_t job = __queue[__q_head & (QUEUE_CAP - 1)];
        __q_head++;
        __q_count--;
        pthread_cond_signal(&__q_not_full);
        pthread_mutex_unlock(&__q_mutex);

        /* execute the job */
        job.fn(job.args, job.future->result);

        /* signal completion */
        pthread_mutex_lock(&job.future->mutex);
        atomic_store_explicit(&job.future->done, 1, memory_order_release);
        pthread_cond_broadcast(&job.future->cond);
        pthread_mutex_unlock(&job.future->mutex);
    }
    return NULL;
}

/* ── public API ── */

void __thread_runtime_init(int num_threads) {
    pthread_mutex_lock(&__init_mutex);
    if (__nworkers > 0) {
        pthread_mutex_unlock(&__init_mutex);
        return; /* already running */
    }
    if (num_threads < 1)  num_threads = 4;
    if (num_threads > MAX_WORKERS) num_threads = MAX_WORKERS;
    for (int i = 0; i < num_threads; i++) {
        if (pthread_create(&__workers[i], NULL, worker_thread_fn, NULL) != 0)
            break;
        __nworkers++;
    }
    pthread_mutex_unlock(&__init_mutex);
}

void __thread_runtime_shutdown(void) {
    pthread_mutex_lock(&__init_mutex);
    if (__nworkers == 0) {
        pthread_mutex_unlock(&__init_mutex);
        return;
    }
    atomic_store_explicit(&__q_shutdown, 1, memory_order_release);
    /* wake all workers so they can observe shutdown */
    pthread_mutex_lock(&__q_mutex);
    pthread_cond_broadcast(&__q_not_empty);
    pthread_mutex_unlock(&__q_mutex);
    for (int i = 0; i < __nworkers; i++)
        pthread_join(__workers[i], NULL);
    __nworkers = 0;
    atomic_store(&__q_shutdown, 0);
    pthread_mutex_unlock(&__init_mutex);
}

/* ── auto init / shutdown ── */

__attribute__((constructor))
static void __thread_runtime_auto_init(void) {
    /* detect CPU count */
#if defined(_SC_NPROCESSORS_ONLN)
    int n = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 4;
#else
    int n = 4;
#endif
    __thread_runtime_init(n);
}

__attribute__((destructor))
static void __thread_runtime_auto_shutdown(void) {
    __thread_runtime_shutdown();
}

__future_t *__thread_dispatch(
        void (*fn)(void *args, void *result),
        void *args,
        size_t result_size) {

    __future_t *f = (__future_t *)malloc(sizeof(__future_t));
    if (!f) return NULL;

    atomic_store_explicit(&f->done, 0, memory_order_relaxed);
    f->result = result_size > 0 ? malloc(result_size) : NULL;
    pthread_mutex_init(&f->mutex, NULL);
    pthread_cond_init(&f->cond, NULL);

    job_t job = { fn, args, f };

    pthread_mutex_lock(&__q_mutex);
    /* if queue is full, block until there is space */
    while (__q_count == QUEUE_CAP)
        pthread_cond_wait(&__q_not_full, &__q_mutex);

    __queue[__q_tail & (QUEUE_CAP - 1)] = job;
    __q_tail++;
    __q_count++;
    pthread_cond_signal(&__q_not_empty);
    pthread_mutex_unlock(&__q_mutex);

    return f;
}

void *__future_get(__future_t *f) {
    /* fast path: already done */
    if (atomic_load_explicit(&f->done, memory_order_acquire))
        return f->result;

    pthread_mutex_lock(&f->mutex);
    while (!atomic_load_explicit(&f->done, memory_order_acquire))
        pthread_cond_wait(&f->cond, &f->mutex);
    pthread_mutex_unlock(&f->mutex);

    return f->result;
}

void __future_wait(__future_t *f) {
    (void)__future_get(f);
}

int __future_ready(__future_t *f) {
    return atomic_load_explicit(&f->done, memory_order_acquire);
}

void __future_drop(__future_t *f) {
    __future_wait(f);
    if (f->result) {
        free(f->result);
        f->result = NULL;
    }
    pthread_mutex_destroy(&f->mutex);
    pthread_cond_destroy(&f->cond);
    free(f);
}
