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

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <process.h>

/* ── pthread compat shims for Windows ──
 * SRWLOCK and CONDITION_VARIABLE both have a valid all-zero static
 * initializer, so they slot in nicely for the existing global
 * PTHREAD_*_INITIALIZER state. */
typedef SRWLOCK             pthread_mutex_t;
typedef CONDITION_VARIABLE  pthread_cond_t;
typedef HANDLE              pthread_t;
#define PTHREAD_MUTEX_INITIALIZER  SRWLOCK_INIT
#define PTHREAD_COND_INITIALIZER   CONDITION_VARIABLE_INIT

static int pthread_mutex_init(pthread_mutex_t *m, void *attr)    { (void)attr; InitializeSRWLock(m); return 0; }
static int pthread_mutex_destroy(pthread_mutex_t *m)             { (void)m; return 0; }
static int pthread_mutex_lock(pthread_mutex_t *m)                { AcquireSRWLockExclusive(m); return 0; }
static int pthread_mutex_unlock(pthread_mutex_t *m)              { ReleaseSRWLockExclusive(m); return 0; }
static int pthread_cond_init(pthread_cond_t *c, void *attr)      { (void)attr; InitializeConditionVariable(c); return 0; }
static int pthread_cond_destroy(pthread_cond_t *c)               { (void)c; return 0; }
static int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) { SleepConditionVariableSRW(c, m, INFINITE, 0); return 0; }
static int pthread_cond_signal(pthread_cond_t *c)                { WakeConditionVariable(c); return 0; }
static int pthread_cond_broadcast(pthread_cond_t *c)             { WakeAllConditionVariable(c); return 0; }

/* Thread create/join compat */
typedef struct { void *(*fn)(void *); void *arg; } win_thread_trampoline_arg_t;
static unsigned __stdcall win_thread_trampoline(void *p) {
    win_thread_trampoline_arg_t a = *(win_thread_trampoline_arg_t *)p;
    free(p);
    a.fn(a.arg);
    return 0;
}
static int pthread_create(pthread_t *t, void *attr, void *(*fn)(void *), void *arg) {
    (void)attr;
    win_thread_trampoline_arg_t *a = (win_thread_trampoline_arg_t *)malloc(sizeof(*a));
    if (!a) return -1;
    a->fn = fn; a->arg = arg;
    uintptr_t h = _beginthreadex(NULL, 0, win_thread_trampoline, a, 0, NULL);
    if (!h) { free(a); return -1; }
    *t = (HANDLE)h;
    return 0;
}
static int pthread_join(pthread_t t, void **retval) {
    (void)retval;
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    return 0;
}

#else
#include <pthread.h>
#include <unistd.h>
#endif

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

/* Global completion notifier — workers broadcast after every job so that
   __future_wait_any can block without polling. */
static pthread_mutex_t __any_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  __any_cv    = PTHREAD_COND_INITIALIZER;

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

        /* wake any thread blocked in __future_wait_any */
        pthread_mutex_lock(&__any_mutex);
        pthread_cond_broadcast(&__any_cv);
        pthread_mutex_unlock(&__any_mutex);
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

#ifdef _WIN32
static int __detect_cpu_count(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int n = (int)si.dwNumberOfProcessors;
    return n < 1 ? 4 : n;
}
#else
static int __detect_cpu_count(void) {
#if defined(_SC_NPROCESSORS_ONLN)
    int n = (int)sysconf(_SC_NPROCESSORS_ONLN);
    return n < 1 ? 4 : n;
#else
    return 4;
#endif
}
#endif

#ifdef _WIN32
/* MSVC clang supports GCC-style constructor/destructor attributes. */
#endif

__attribute__((constructor))
static void __thread_runtime_auto_init(void) {
    __thread_runtime_init(__detect_cpu_count());
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

int __future_wait_any(__future_t **fs, int n) {
    if (n <= 0 || !fs) return -1;
    for (;;) {
        /* fast scan — any already done? */
        for (int i = 0; i < n; i++) {
            if (fs[i] && atomic_load_explicit(&fs[i]->done, memory_order_acquire))
                return i;
        }
        /* block until some future signals completion */
        pthread_mutex_lock(&__any_mutex);
        /* re-check under lock to avoid missed wakeup */
        int idx = -1;
        for (int i = 0; i < n; i++) {
            if (fs[i] && atomic_load_explicit(&fs[i]->done, memory_order_acquire)) {
                idx = i; break;
            }
        }
        if (idx >= 0) {
            pthread_mutex_unlock(&__any_mutex);
            return idx;
        }
        pthread_cond_wait(&__any_cv, &__any_mutex);
        pthread_mutex_unlock(&__any_mutex);
    }
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
