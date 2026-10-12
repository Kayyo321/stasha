#if !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "observation_runtime.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

_Static_assert(sizeof(size_t) == 8u && sizeof(uintptr_t) == 8u,
               "observation ABI 2 requires a native LP64 host");

/* This runtime deliberately has no constructor, destructor, signal handler or
 * atexit finalizer. Its sole successful endpoint is the generated test main.
 * All metadata is private fixed storage, never observation-allocated memory.
 * Keys are integer snapshots: no invalidated C pointer object is inspected
 * after a native free or successful realloc. Integers do not prove freshness. */
typedef struct {
    uintptr_t key;
    uint64_t generation;
    uint64_t bytes;
    uint32_t alloc_site;
    uint32_t resize_site;
    uint32_t origin_test;
} obs_record;

typedef struct {
    obs_record records[STASHA_OBSERVATION_MAX_RECORDS];
    unsigned char tests[STASHA_OBSERVATION_MAX_TESTS];
    unsigned char hits[(STASHA_OBSERVATION_MAX_LINES + 7u) / 8u];
    char identities[5][STASHA_OBSERVATION_MAX_ID_BYTES + 1u];
    char path[STASHA_OBSERVATION_MAX_PATH_BYTES + 1u];
    uint64_t total_bytes, generation, assertions_passed, assertions_failed;
    uint32_t enabled, test_count, line_count, site_count;
    uint32_t started, completed, current_test, record_count;
    enum stasha_observation_reason reason;
    unsigned initialized, finalized, endpoint;
} obs_state;

typedef struct { uintptr_t key; int error_number; } obs_alloc_result;
typedef struct {
    void *context;
    obs_alloc_result (*allocate)(void *, size_t);
    obs_alloc_result (*resize)(void *, uintptr_t, size_t);
    int (*release)(void *, uintptr_t);
} obs_allocator;

static obs_state obs_global;

static void obs_fail(obs_state *s, enum stasha_observation_reason reason) {
    if (s->reason == STASHA_OBSERVATION_OK) s->reason = reason;
}

static int obs_copy_id(char *out, const char *in) {
    if (!in) return 0;
    size_t n = strnlen(in, STASHA_OBSERVATION_MAX_ID_BYTES + 1u);
    if (!n || n > STASHA_OBSERVATION_MAX_ID_BYTES) return 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)in[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '.' ||
              c == ':' || c == '-')) return 0;
    }
    memcpy(out, in, n + 1u);
    return 1;
}

static int obs_core_init(obs_state *s, const char *const ids[5],
                         uint32_t enabled, uint32_t tests,
                         uint32_t lines, uint32_t sites) {
    if (s->initialized || s->finalized || s->reason) {
        obs_fail(s, STASHA_OBSERVATION_BAD_LIFECYCLE);
        return 0;
    }
    s->current_test = STASHA_OBSERVATION_NO_TEST;
    if (!enabled || enabled > 3u || tests > STASHA_OBSERVATION_MAX_TESTS ||
        lines > STASHA_OBSERVATION_MAX_LINES || sites > STASHA_OBSERVATION_MAX_SITES) {
        obs_fail(s, STASHA_OBSERVATION_BAD_CONFIGURATION);
        return 0;
    }
    for (unsigned i = 0; i < 5u; ++i) {
        if (!obs_copy_id(s->identities[i], ids[i])) {
            obs_fail(s, STASHA_OBSERVATION_BAD_CONFIGURATION);
            return 0;
        }
    }
    s->enabled = enabled;
    s->test_count = tests;
    s->line_count = lines;
    s->site_count = sites;
    s->initialized = 1u;
    return 1;
}

static int obs_core_begin(obs_state *s, uint32_t test) {
    if (s->reason) return 0;
    if (!s->initialized || s->finalized || test >= s->test_count ||
        s->current_test != STASHA_OBSERVATION_NO_TEST || test != s->completed ||
        s->started != s->completed || s->tests[test] != 0u) {
        obs_fail(s, STASHA_OBSERVATION_BAD_LIFECYCLE);
        return 0;
    }
    s->tests[test] = 1u;
    s->current_test = test;
    ++s->started;
    return 1;
}

static int obs_core_end(obs_state *s, uint32_t test) {
    if (s->reason) return 0;
    if (!s->initialized || s->finalized || test >= s->test_count ||
        s->current_test != test || s->tests[test] != 1u || test != s->completed) {
        obs_fail(s, STASHA_OBSERVATION_BAD_LIFECYCLE);
        return 0;
    }
    s->tests[test] = 2u;
    s->current_test = STASHA_OBSERVATION_NO_TEST;
    ++s->completed;
    return 1;
}

static int obs_current_test(obs_state *s) {
    if (s->reason) return 0;
    if (!s->initialized || s->finalized || s->current_test >= s->test_count ||
        s->tests[s->current_test] != 1u) {
        obs_fail(s, STASHA_OBSERVATION_BAD_LIFECYCLE);
        return 0;
    }
    return 1;
}

static int obs_core_hit(obs_state *s, uint32_t line) {
    if (!obs_current_test(s)) return 0;
    if (!(s->enabled & STASHA_OBSERVATION_COVERAGE) || line >= s->line_count) {
        obs_fail(s, STASHA_OBSERVATION_BAD_LINE);
        return 0;
    }
    s->hits[line / 8u] |= (unsigned char)(1u << (line % 8u));
    return 1;
}

static int obs_operation(obs_state *s, uint32_t site) {
    if (!obs_current_test(s)) return 0;
    if (site >= s->site_count) {
        obs_fail(s, STASHA_OBSERVATION_BAD_SITE);
        return 0;
    }
    if (s->record_count > STASHA_OBSERVATION_MAX_RECORDS) {
        obs_fail(s, STASHA_OBSERVATION_CORRUPTED_LEDGER);
        return 0;
    }
    return 1;
}

static uint32_t obs_find(const obs_state *s, uintptr_t key) {
    for (uint32_t i = 0; i < s->record_count; ++i)
        if (s->records[i].key == key) return i;
    return STASHA_OBSERVATION_MAX_RECORDS;
}

static int obs_new_capacity(obs_state *s, uint64_t bytes) {
    if (s->record_count == STASHA_OBSERVATION_MAX_RECORDS)
        obs_fail(s, STASHA_OBSERVATION_RECORD_CAPACITY);
    else if (s->generation == UINT64_MAX)
        obs_fail(s, STASHA_OBSERVATION_GENERATION_OVERFLOW);
    else if (bytes > UINT64_MAX - s->total_bytes)
        obs_fail(s, STASHA_OBSERVATION_TOTAL_OVERFLOW);
    return s->reason == STASHA_OBSERVATION_OK;
}

static void obs_add(obs_state *s, uintptr_t key, uint64_t bytes, uint32_t site) {
    if (obs_find(s, key) != STASHA_OBSERVATION_MAX_RECORDS) {
        obs_fail(s, STASHA_OBSERVATION_DUPLICATE_BASE);
        return;
    }
    obs_record *r = &s->records[s->record_count++];
    r->key = key;
    r->generation = ++s->generation;
    r->bytes = bytes;
    r->alloc_site = site;
    r->resize_site = STASHA_OBSERVATION_NO_SITE;
    r->origin_test = s->current_test;
    s->total_bytes += bytes;
}

static obs_alloc_result obs_core_new(obs_state *s, const obs_allocator *a,
                                     size_t bytes, uint32_t site) {
    obs_alloc_result result = { 0, errno };
    if (!obs_operation(s, site) || !obs_new_capacity(s, (uint64_t)bytes)) return result;
    result = a->allocate(a->context, bytes);
    if (result.key) obs_add(s, result.key, (uint64_t)bytes, site);
    return result;
}

static obs_alloc_result obs_core_mov(obs_state *s, const obs_allocator *a,
                                     uintptr_t key, size_t bytes, uint32_t site) {
    obs_alloc_result result = { 0, errno };
    if (!obs_operation(s, site)) return result;
    if (!bytes) {
        obs_fail(s, STASHA_OBSERVATION_ZERO_RESIZE);
        return result;
    }
    if (!key) {
        if (!obs_new_capacity(s, (uint64_t)bytes)) return result;
        /* Keep realloc(NULL,n) as realloc(NULL,n), not malloc(n). */
        result = a->resize(a->context, 0, bytes);
        if (result.key) obs_add(s, result.key, (uint64_t)bytes, site);
        return result;
    }
    uint32_t index = obs_find(s, key);
    if (index == STASHA_OBSERVATION_MAX_RECORDS) {
        obs_fail(s, STASHA_OBSERVATION_UNKNOWN_BASE);
        return result;
    }
    uint64_t old_bytes = s->records[index].bytes;
    if (old_bytes > s->total_bytes) {
        obs_fail(s, STASHA_OBSERVATION_CORRUPTED_LEDGER);
        return result;
    }
    uint64_t other_bytes = s->total_bytes - old_bytes;
    if ((uint64_t)bytes > UINT64_MAX - other_bytes) {
        obs_fail(s, STASHA_OBSERVATION_TOTAL_OVERFLOW);
        return result;
    }
    result = a->resize(a->context, key, bytes);
    /* NULL nonzero resize preserves the entire old record, including site. */
    if (!result.key) return result;
    uint32_t duplicate = obs_find(s, result.key);
    if (duplicate != STASHA_OBSERVATION_MAX_RECORDS && duplicate != index) {
        obs_fail(s, STASHA_OBSERVATION_DUPLICATE_BASE);
        return result;
    }
    s->records[index].key = result.key;
    s->records[index].bytes = (uint64_t)bytes;
    s->records[index].resize_site = site;
    s->total_bytes = other_bytes + (uint64_t)bytes;
    return result;
}

static int obs_core_rem(obs_state *s, const obs_allocator *a,
                         uintptr_t key, uint32_t site) {
    int error_number = errno;
    if (!obs_operation(s, site)) return error_number;
    if (!key) return error_number;
    uint32_t index = obs_find(s, key);
    if (index == STASHA_OBSERVATION_MAX_RECORDS) {
        obs_fail(s, STASHA_OBSERVATION_UNKNOWN_BASE);
        return error_number;
    }
    uint64_t bytes = s->records[index].bytes;
    if (bytes > s->total_bytes) {
        obs_fail(s, STASHA_OBSERVATION_CORRUPTED_LEDGER);
        return error_number;
    }
    error_number = a->release(a->context, key);
    s->total_bytes -= bytes;
    --s->record_count;
    /* Compacting retains increasing generation order without an allocator. */
    memmove(&s->records[index], &s->records[index + 1u],
            (s->record_count - index) * sizeof(s->records[0]));
    memset(&s->records[s->record_count], 0, sizeof(s->records[0]));
    return error_number;
}

static int obs_audit(obs_state *s) {
    if (s->record_count > STASHA_OBSERVATION_MAX_RECORDS ||
        s->test_count > STASHA_OBSERVATION_MAX_TESTS ||
        s->line_count > STASHA_OBSERVATION_MAX_LINES ||
        s->site_count > STASHA_OBSERVATION_MAX_SITES) {
        obs_fail(s, STASHA_OBSERVATION_CORRUPTED_LEDGER);
        return 0;
    }
    uint32_t started = 0, completed = 0;
    unsigned previous = 2u;
    for (uint32_t i = 0; i < s->test_count; ++i) {
        unsigned value = s->tests[i];
        if (value > 2u || value > previous ||
            (value == 1u && (s->current_test != i || started != completed))) {
            obs_fail(s, STASHA_OBSERVATION_CORRUPTED_LEDGER);
            return 0;
        }
        started += value != 0u;
        completed += value == 2u;
        previous = value;
    }
    if (started != s->started || completed != s->completed ||
        (started == completed && s->current_test != STASHA_OBSERVATION_NO_TEST) ||
        (started != completed && (started != completed + 1u || s->current_test != completed))) {
        obs_fail(s, STASHA_OBSERVATION_CORRUPTED_LEDGER);
        return 0;
    }
    if (s->line_count % 8u &&
        (s->hits[s->line_count / 8u] & (unsigned char)(0xffu << (s->line_count % 8u)))) {
        obs_fail(s, STASHA_OBSERVATION_CORRUPTED_LEDGER);
        return 0;
    }
    uint64_t total = 0, generation = 0;
    for (uint32_t i = 0; i < s->record_count; ++i) {
        const obs_record *r = &s->records[i];
        if (!r->key || r->generation <= generation || r->generation > s->generation ||
            r->alloc_site >= s->site_count || r->origin_test >= s->test_count ||
            !s->tests[r->origin_test] ||
            (r->resize_site != STASHA_OBSERVATION_NO_SITE && r->resize_site >= s->site_count) ||
            r->bytes > UINT64_MAX - total) {
            obs_fail(s, STASHA_OBSERVATION_CORRUPTED_LEDGER);
            return 0;
        }
        for (uint32_t j = 0; j < i; ++j) {
            if (s->records[j].key == r->key) {
                obs_fail(s, STASHA_OBSERVATION_CORRUPTED_LEDGER);
                return 0;
            }
        }
        total += r->bytes;
        generation = r->generation;
    }
    if (total != s->total_bytes) {
        obs_fail(s, STASHA_OBSERVATION_CORRUPTED_LEDGER);
        return 0;
    }
    return 1;
}

static int obs_core_finish(obs_state *s, uint64_t passed, uint64_t failed) {
    if (!s->initialized || s->finalized) {
        obs_fail(s, STASHA_OBSERVATION_BAD_LIFECYCLE);
        return 0;
    }
    s->finalized = 1u;
    s->assertions_passed = passed;
    s->assertions_failed = failed;
    if (!obs_audit(s)) return 0;
    if (!s->test_count || s->started != s->test_count || s->completed != s->test_count ||
        s->current_test != STASHA_OBSERVATION_NO_TEST || passed > UINT64_MAX - failed)
        obs_fail(s, STASHA_OBSERVATION_BAD_LIFECYCLE);
    else {
        s->endpoint = 1u;
        for (uint32_t i = 0; i < s->test_count; ++i)
            if (s->tests[i] != 2u) obs_fail(s, STASHA_OBSERVATION_BAD_LIFECYCLE);
    }
    (void)obs_audit(s);
    return s->reason == STASHA_OBSERVATION_OK;
}

/* Capture errno immediately after the native call, before any bookkeeping.
 * Only these adapters convert integer keys back to live native pointers. */
static obs_alloc_result obs_native_new(void *context, size_t bytes) {
    (void)context;
    void *p = malloc(bytes);
    int error_number = errno;
    obs_alloc_result result = { (uintptr_t)p, error_number };
    return result;
}

static obs_alloc_result obs_native_mov(void *context, uintptr_t key, size_t bytes) {
    (void)context;
    void *p = realloc((void *)key, bytes);
    int error_number = errno;
    obs_alloc_result result = { (uintptr_t)p, error_number };
    return result;
}

static int obs_native_rem(void *context, uintptr_t key) {
    (void)context;
    free((void *)key);
    return errno;
}

static const obs_allocator obs_native_allocator = {
    NULL, obs_native_new, obs_native_mov, obs_native_rem
};

typedef struct {
    void *context;
    int (*write_bytes)(void *, const char *, size_t);
    size_t count;
    int failed;
} obs_writer;

/* The production adapters below are ordinary path-based syscalls. Keeping the
 * boundary explicit also permits pure publication-failure controls without
 * touching a real file, descriptor, or allocator in the qualification tests. */
typedef struct {
    void *context;
    int (*open_report)(void *, const char *, int, unsigned);
    ssize_t (*write_report)(void *, int, const char *, size_t);
    int (*close_report)(void *, int);
} obs_io;

typedef struct { const obs_io *io; int fd; } obs_fd_context;

static void obs_write(obs_writer *w, const char *bytes, size_t count) {
    if (w->failed) return;
    if (count > STASHA_OBSERVATION_MAX_REPORT_BYTES - w->count ||
        !w->write_bytes(w->context, bytes, count)) {
        w->failed = 1;
        return;
    }
    w->count += count;
}

static void obs_printf(obs_writer *w, const char *format, ...) {
    char buffer[512];
    va_list args;
    va_start(args, format);
    int n = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (n < 0 || (size_t)n >= sizeof(buffer)) { w->failed = 1; return; }
    obs_write(w, buffer, (size_t)n);
}

static int obs_emit_report(obs_state *s, obs_writer *w) {
    if (!s->initialized || !obs_audit(s)) return 0;
    obs_printf(w, "STASHA_OBSERVATION %u\nprofile %s\nruntime_abi %u\nmetric_versions %u %u\n",
        STASHA_OBSERVATION_SCHEMA_VERSION, STASHA_OBSERVATION_PROFILE,
        STASHA_OBSERVATION_ABI_VERSION, STASHA_OBSERVATION_ALLOCATION_VERSION,
        STASHA_OBSERVATION_LINE_VERSION);
    static const char *const names[5] = { "run", "input_graph", "map", "config", "compiler" };
    for (unsigned i = 0; i < 5u; ++i) obs_printf(w, "%s %s\n", names[i], s->identities[i]);
    obs_printf(w, "enabled %" PRIu32 "\ninventory %" PRIu32 " %" PRIu32 " %" PRIu32 "\n",
        s->enabled, s->test_count, s->line_count, s->site_count);
    unsigned complete = s->finalized && s->endpoint && !s->reason;
    obs_printf(w, "state %u %u %u\nassertions %" PRIu64 " %" PRIu64
        "\ntests %" PRIu32 " %" PRIu32 "\n", complete, (unsigned)s->reason,
        s->endpoint, s->assertions_passed, s->assertions_failed, s->started, s->completed);
    for (uint32_t i = 0; i < s->test_count; ++i)
        obs_printf(w, "test %" PRIu32 " %u\n", i, (unsigned)s->tests[i]);
    obs_write(w, "hits ", 5u);
    if (!s->line_count) obs_write(w, "-", 1u);
    else {
        static const char hex[] = "0123456789abcdef";
        char chunk[256];
        uint32_t bytes = (s->line_count + 7u) / 8u;
        for (uint32_t i = 0; i < bytes;) {
            size_t used = 0;
            while (i < bytes && used < sizeof(chunk)) {
                unsigned char b = s->hits[i++];
                chunk[used++] = hex[b >> 4u];
                chunk[used++] = hex[b & 15u];
            }
            obs_write(w, chunk, used);
        }
    }
    obs_printf(w, "\nallocations %" PRIu32 " %" PRIu64 "\n", s->record_count, s->total_bytes);
    for (uint32_t i = 0; i < s->record_count; ++i) {
        const obs_record *r = &s->records[i];
        obs_printf(w, "allocation %" PRIu64 " %" PRIu64 " %" PRIu32 " %" PRIu32 " %" PRIu32 "\n",
            r->generation, r->bytes, r->alloc_site, r->resize_site, r->origin_test);
    }
    obs_write(w, "END\n", 4u);
    return !w->failed;
}

static int obs_fd_write(void *context, const char *bytes, size_t count) {
    const obs_fd_context *out = context;
    while (count) {
        ssize_t n = out->io->write_report(out->io->context, out->fd, bytes, count);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0 || (size_t)n > count) return 0;
        bytes += (size_t)n;
        count -= (size_t)n;
    }
    return 1;
}

static int obs_publish_with(obs_state *s, const obs_io *io) {
    if (!s->initialized || !s->path[0]) {
        obs_fail(s, STASHA_OBSERVATION_REPORT_IO);
        return 0;
    }
    int fd = io->open_report(io->context, s->path,
        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) { obs_fail(s, STASHA_OBSERVATION_REPORT_IO); return 0; }
    obs_fd_context context = { io, fd };
    obs_writer writer = { &context, obs_fd_write, 0, 0 };
    int ok = obs_emit_report(s, &writer);
    /* On Linux a failed close, including EINTR, is not retried on a possibly
     * recycled descriptor. Any close failure makes the child fail. */
    if (io->close_report(io->context, fd) != 0) ok = 0;
    if (!ok) obs_fail(s, STASHA_OBSERVATION_REPORT_IO);
    return ok;
}

static int obs_native_open(void *context, const char *path, int flags, unsigned mode) {
    (void)context;
    return open(path, flags, mode);
}
static ssize_t obs_native_write(void *context, int fd, const char *bytes, size_t count) {
    (void)context;
    return write(fd, bytes, count);
}
static int obs_native_close(void *context, int fd) {
    (void)context;
    return close(fd);
}
static const obs_io obs_native_io = {
    NULL, obs_native_open, obs_native_write, obs_native_close
};

static int obs_finish_publish(obs_state *s, uint64_t passed, uint64_t failed,
                               const obs_io *io) {
    if (s->finalized) {
        obs_fail(s, STASHA_OBSERVATION_BAD_LIFECYCLE);
        return 1;
    }
    int complete = obs_core_finish(s, passed, failed);
    int published = obs_publish_with(s, io);
    return !complete || !published;
}

static void obs_diagnostic(const obs_state *s) {
    static const char *const descriptions[] = {
        "unspecified runtime failure", "invalid runtime configuration",
        "test lifecycle did not complete exactly once", "unknown allocation operation site",
        "unknown coverage line", "pointer is not a live tracked allocation base",
        "zero-size resize is unsupported by observation profile", "allocation record capacity exhausted",
        "outstanding requested-byte total overflow", "duplicate live allocation base",
        "allocation generation overflow", "inconsistent observation bookkeeping",
        "report creation, write, or close failed"
    };
    unsigned reason = (unsigned)s->reason;
    const char *description = reason < sizeof(descriptions) / sizeof(descriptions[0])
        ? descriptions[reason] : "unknown runtime failure";
    fprintf(stderr, "observation incomplete: %s (reason %u)\n", description, reason);
}

static _Noreturn void obs_abort(obs_state *s) {
    (void)obs_publish_with(s, &obs_native_io);
    obs_diagnostic(s);
    /* Unsupported free/resize must stop before allocator invocation, without
     * executing source atexit/cleanup paths outside the admitted endpoint. */
    _Exit(1);
}

void __stasha_obs_init_v2(const char *run, const char *input_graph, const char *map,
                       const char *config, const char *compiler, uint32_t enabled,
                       uint32_t tests, uint32_t lines, uint32_t sites) {
    int error_number = errno;
    const char *const ids[5] = { run, input_graph, map, config, compiler };
    if (!obs_core_init(&obs_global, ids, enabled, tests, lines, sites)) obs_abort(&obs_global);
    const char *path = getenv("STASHA_OBSERVATION_REPORT");
    size_t n = path ? strnlen(path, STASHA_OBSERVATION_MAX_PATH_BYTES + 1u) : 0;
    if (!n || n > STASHA_OBSERVATION_MAX_PATH_BYTES) {
        obs_fail(&obs_global, STASHA_OBSERVATION_BAD_CONFIGURATION);
        obs_abort(&obs_global);
    }
    memcpy(obs_global.path, path, n + 1u);
    errno = error_number;
}

void __stasha_obs_test_begin(uint32_t test) {
    int error_number = errno;
    if (!obs_core_begin(&obs_global, test)) obs_abort(&obs_global);
    errno = error_number;
}

void __stasha_obs_test_end(uint32_t test) {
    int error_number = errno;
    if (!obs_core_end(&obs_global, test)) obs_abort(&obs_global);
    errno = error_number;
}

void __stasha_obs_hit(uint32_t line) {
    int error_number = errno;
    if (!obs_core_hit(&obs_global, line)) obs_abort(&obs_global);
    errno = error_number;
}

void *__stasha_obs_new(size_t bytes, uint32_t site) {
    obs_alloc_result result = obs_core_new(&obs_global, &obs_native_allocator, bytes, site);
    if (obs_global.reason) obs_abort(&obs_global);
    errno = result.error_number;
    return (void *)result.key;
}

void *__stasha_obs_mov(void *base, size_t bytes, uint32_t site) {
    uintptr_t key = (uintptr_t)base; /* Snapshot while base is still valid. */
    obs_alloc_result result = obs_core_mov(&obs_global, &obs_native_allocator, key, bytes, site);
    if (obs_global.reason) obs_abort(&obs_global);
    errno = result.error_number;
    return (void *)result.key;
}

void __stasha_obs_rem(void *base, uint32_t site) {
    uintptr_t key = (uintptr_t)base; /* No later evaluation of base. */
    int error_number = obs_core_rem(&obs_global, &obs_native_allocator, key, site);
    if (obs_global.reason) obs_abort(&obs_global);
    errno = error_number;
}

int __stasha_obs_finish(uint64_t passed, uint64_t failed) {
    int status = obs_finish_publish(&obs_global, passed, failed, &obs_native_io);
    if (status) obs_diagnostic(&obs_global);
    return status;
}
