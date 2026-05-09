/* ── Stasha Coroutine/Async Runtime Implementation ──────────────────────── */

#include "coro_runtime.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#include <process.h>

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

typedef struct { void *(*fn)(void *); void *arg; } win_thr_arg_t;
static unsigned __stdcall win_thr_trampoline(void *p) {
    win_thr_arg_t a = *(win_thr_arg_t *)p;
    free(p);
    a.fn(a.arg);
    return 0;
}
static int pthread_create(pthread_t *t, void *attr, void *(*fn)(void *), void *arg) {
    (void)attr;
    win_thr_arg_t *a = (win_thr_arg_t *)malloc(sizeof(*a));
    if (!a) return -1;
    a->fn = fn; a->arg = arg;
    uintptr_t h = _beginthreadex(NULL, 0, win_thr_trampoline, a, 0, NULL);
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

#define STS_ASYNC_MAX_WORKERS 64
#define STS_ASYNC_QUEUE_CAP   8192

struct __async_future {
    _Atomic int done;
    _Atomic int cancelled;
    void       *result;
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
};

typedef struct {
    void (*fn)(void *args, void *result);
    void *args;
    __async_future_t *future;
} async_job_t;

static async_job_t      __async_queue[STS_ASYNC_QUEUE_CAP];
static size_t           __async_q_head = 0;
static size_t           __async_q_tail = 0;
static size_t           __async_q_count = 0;
static pthread_mutex_t  __async_q_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   __async_q_not_empty = PTHREAD_COND_INITIALIZER;
static pthread_cond_t   __async_q_not_full = PTHREAD_COND_INITIALIZER;
static _Atomic int      __async_q_shutdown = 0;

static pthread_t        __async_workers[STS_ASYNC_MAX_WORKERS];
static int              __async_nworkers = 0;
static pthread_mutex_t  __async_init_mutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t  __async_any_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   __async_any_cv = PTHREAD_COND_INITIALIZER;

static void *__async_worker_thread_fn(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&__async_q_mutex);
        while (__async_q_count == 0 && !atomic_load_explicit(&__async_q_shutdown, memory_order_relaxed))
            pthread_cond_wait(&__async_q_not_empty, &__async_q_mutex);

        if (__async_q_count == 0) {
            pthread_mutex_unlock(&__async_q_mutex);
            return NULL;
        }

        async_job_t job = __async_queue[__async_q_head & (STS_ASYNC_QUEUE_CAP - 1)];
        __async_q_head++;
        __async_q_count--;
        pthread_cond_signal(&__async_q_not_full);
        pthread_mutex_unlock(&__async_q_mutex);

        if (!atomic_load_explicit(&job.future->cancelled, memory_order_acquire))
            job.fn(job.args, job.future->result);

        pthread_mutex_lock(&job.future->mutex);
        atomic_store_explicit(&job.future->done, 1, memory_order_release);
        pthread_cond_broadcast(&job.future->cond);
        pthread_mutex_unlock(&job.future->mutex);

        pthread_mutex_lock(&__async_any_mutex);
        pthread_cond_broadcast(&__async_any_cv);
        pthread_mutex_unlock(&__async_any_mutex);
    }
}

void __async_runtime_init(int num_threads) {
    pthread_mutex_lock(&__async_init_mutex);
    if (__async_nworkers > 0) {
        pthread_mutex_unlock(&__async_init_mutex);
        return;
    }
    if (num_threads < 1) num_threads = 4;
    if (num_threads > STS_ASYNC_MAX_WORKERS) num_threads = STS_ASYNC_MAX_WORKERS;
    for (int i = 0; i < num_threads; i++) {
        if (pthread_create(&__async_workers[i], NULL, __async_worker_thread_fn, NULL) != 0)
            break;
        __async_nworkers++;
    }
    pthread_mutex_unlock(&__async_init_mutex);
}

void __async_runtime_shutdown(void) {
    pthread_mutex_lock(&__async_init_mutex);
    if (__async_nworkers == 0) {
        pthread_mutex_unlock(&__async_init_mutex);
        return;
    }
    atomic_store_explicit(&__async_q_shutdown, 1, memory_order_release);
    pthread_mutex_lock(&__async_q_mutex);
    pthread_cond_broadcast(&__async_q_not_empty);
    pthread_mutex_unlock(&__async_q_mutex);
    for (int i = 0; i < __async_nworkers; i++)
        pthread_join(__async_workers[i], NULL);
    __async_nworkers = 0;
    atomic_store(&__async_q_shutdown, 0);
    pthread_mutex_unlock(&__async_init_mutex);
}

static int __async_detect_cpu_count(void) {
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int n = (int)si.dwNumberOfProcessors;
    return n < 1 ? 4 : n;
#elif defined(_SC_NPROCESSORS_ONLN)
    int n = (int)sysconf(_SC_NPROCESSORS_ONLN);
    return n < 1 ? 4 : n;
#else
    return 4;
#endif
}

__attribute__((constructor))
static void __async_runtime_auto_init(void) {
    __async_runtime_init(__async_detect_cpu_count());
}

__attribute__((destructor))
static void __async_runtime_auto_shutdown(void) {
    __async_runtime_shutdown();
}

__async_future_t *__async_dispatch(
        void (*fn)(void *args, void *result),
        void *args,
        size_t result_size) {
    __async_future_t *f = (__async_future_t *)malloc(sizeof(__async_future_t));
    if (!f) return NULL;

    atomic_store_explicit(&f->done, 0, memory_order_relaxed);
    atomic_store_explicit(&f->cancelled, 0, memory_order_relaxed);
    f->result = result_size > 0 ? malloc(result_size) : NULL;
    pthread_mutex_init(&f->mutex, NULL);
    pthread_cond_init(&f->cond, NULL);

    async_job_t job = { fn, args, f };
    pthread_mutex_lock(&__async_q_mutex);
    while (__async_q_count == STS_ASYNC_QUEUE_CAP)
        pthread_cond_wait(&__async_q_not_full, &__async_q_mutex);
    __async_queue[__async_q_tail & (STS_ASYNC_QUEUE_CAP - 1)] = job;
    __async_q_tail++;
    __async_q_count++;
    pthread_cond_signal(&__async_q_not_empty);
    pthread_mutex_unlock(&__async_q_mutex);
    return f;
}

void *__async_get(__async_future_t *f) {
    if (!f) return NULL;
    if (atomic_load_explicit(&f->done, memory_order_acquire))
        return f->result;
    pthread_mutex_lock(&f->mutex);
    while (!atomic_load_explicit(&f->done, memory_order_acquire))
        pthread_cond_wait(&f->cond, &f->mutex);
    pthread_mutex_unlock(&f->mutex);
    return f->result;
}

void __async_wait(__async_future_t *f) {
    (void)__async_get(f);
}

int __async_ready(__async_future_t *f) {
    if (!f) return 1;
    return atomic_load_explicit(&f->done, memory_order_acquire);
}

int __async_wait_any(__async_future_t **fs, int n) {
    if (n <= 0 || !fs) return -1;
    for (;;) {
        for (int i = 0; i < n; i++) {
            if (fs[i] && atomic_load_explicit(&fs[i]->done, memory_order_acquire))
                return i;
        }
        pthread_mutex_lock(&__async_any_mutex);
        int idx = -1;
        for (int i = 0; i < n; i++) {
            if (fs[i] && atomic_load_explicit(&fs[i]->done, memory_order_acquire)) {
                idx = i;
                break;
            }
        }
        if (idx >= 0) {
            pthread_mutex_unlock(&__async_any_mutex);
            return idx;
        }
        pthread_cond_wait(&__async_any_cv, &__async_any_mutex);
        pthread_mutex_unlock(&__async_any_mutex);
    }
}

void __async_cancel(__async_future_t *f) {
    if (!f) return;
    atomic_store_explicit(&f->cancelled, 1, memory_order_release);
}

int __async_cancelled(__async_future_t *f) {
    if (!f) return 1;
    return atomic_load_explicit(&f->cancelled, memory_order_acquire);
}

void __async_drop(__async_future_t *f) {
    if (!f) return;
    __async_wait(f);
    if (f->result) {
        free(f->result);
        f->result = NULL;
    }
    pthread_mutex_destroy(&f->mutex);
    pthread_cond_destroy(&f->cond);
    free(f);
}
