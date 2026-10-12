#ifndef STASHA_OBSERVATION_DRIVER_H
#define STASHA_OBSERVATION_DRIVER_H

/* Driver-only helpers, included before common.h's compiler-allocation macros.
 * Reports are data, never program output or shell commands. */
#include "observation_options.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#if defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>
#endif

#define STS_OBS_SOURCE_LIMIT (16u * 1024u * 1024u)
#define STS_OBS_SOURCE_LINES 1000000u
#define STS_OBS_REPORT_LIMIT STASHA_OBSERVATION_MAX_REPORT_BYTES
#define STS_OBS_TEST_LIMIT STASHA_OBSERVATION_MAX_TESTS
#define STS_OBS_LINE_LIMIT STASHA_OBSERVATION_MAX_LINES
#define STS_OBS_SITE_LIMIT STASHA_OBSERVATION_MAX_SITES
#define STS_OBS_RECORD_LIMIT STASHA_OBSERVATION_MAX_RECORDS

typedef struct {
    uint64_t generation, bytes;
    uint32_t allocation_site, resize_site, test;
} sts_observation_live;
typedef struct {
    uint64_t assertions_passed, assertions_failed, live_bytes, covered;
    uint32_t complete, reason, endpoint, started, completed, live_count;
    uint8_t hits[STS_OBS_LINE_LIMIT / 8];
    sts_observation_live live[STS_OBS_RECORD_LIMIT];
} sts_observation_report;
typedef struct {
    int launched, waited, launch_errno, wait_errno, exited, exit_code, signal;
} sts_observation_child;
typedef struct {
    char directory[512], report_path[576], metadata_path[576];
    char binary_id[129], runtime_id[129], target[256];
    char *source, *canonical_source;
    size_t source_size;
    int compile_link_success;
    int image_fd;
} sts_observation_run;

static int sts_observation_error(char *error, size_t size, const char *message) {
    if (size) snprintf(error, size, "%s", message);
    return -1;
}
static void sts_observation_manifest_free(sts_observation_manifest *m) {
    if (m->sites) for (uint32_t i = 0; i < m->site_count; ++i) free(m->sites[i].owner);
    if (m->tests) for (uint32_t i = 0; i < m->test_count; ++i) free(m->tests[i].name);
    free(m->lines); free(m->sites); free(m->tests); free(m->exclusions);
    memset(m, 0, sizeof(*m));
}
static void sts_observation_run_free(sts_observation_run *r) {
#if defined(__linux__)
    if (r->image_fd >= 0) {
        if (close(r->image_fd)) fprintf(stderr, "observation: cannot close retained image descriptor: %s\n", strerror(errno));
        r->image_fd = -1;
    }
#endif
    free(r->source); free(r->canonical_source);
    r->source = r->canonical_source = NULL;
}
static int sts_observation_id_valid(const char *s) {
    size_t n = 0;
    if (!s || !*s) return 0;
    for (; n <= 128 && s[n]; ++n) {
        unsigned char c = (unsigned char)s[n];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '.' || c == ':' || c == '-')) return 0;
    }
    return n <= 128;
}
static int sts_observation_manifest_valid(const sts_observation_config *c,
                                           const sts_observation_manifest *m) {
    if (!c || !m || !c->source_line_count || c->source_line_count > STS_OBS_SOURCE_LINES ||
        !sts_observation_id_valid(c->run_id) || !sts_observation_id_valid(c->source_id) ||
        !sts_observation_id_valid(c->config_id) || !sts_observation_id_valid(c->compiler_id) ||
        !sts_observation_id_valid(m->map_id) || c->enabled_mask < 1 || c->enabled_mask > 3 ||
        c->enabled_mask != (uint32_t)(((c->warn_memleak || c->has_max_bytes) ? 1 : 0) | (c->has_min_coverage ? 2 : 0)) ||
        c->min_coverage > 100 || !m->test_count || m->test_count > STS_OBS_TEST_LIMIT ||
        m->line_count > STS_OBS_LINE_LIMIT || m->site_count > STS_OBS_SITE_LIMIT ||
        !m->tests || !m->exclusions || (m->line_count && !m->lines) || (m->site_count && !m->sites)) return 0;
    uint32_t line_id = 0;
    for (uint32_t line = 1; line <= c->source_line_count; ++line) {
        if (m->exclusions[line] > STS_OBS_UNREACHABLE) return 0;
        if (m->exclusions[line] == STS_OBS_ELIGIBLE) {
            if (line_id >= m->line_count || m->lines[line_id++] != line) return 0;
        }
    }
    if (line_id != m->line_count) return 0;
    for (uint32_t i = 0; i < m->test_count; ++i)
        if (!m->tests[i].line || m->tests[i].line > c->source_line_count || !m->tests[i].name) return 0;
    for (uint32_t i = 0; i < m->site_count; ++i) {
        const sts_observation_site *s = &m->sites[i];
        if (!s->line || s->line > c->source_line_count || !s->owner ||
            s->kind < STS_OBS_NEW || s->kind > STS_OBS_REM ||
            (s->owner_test != UINT32_MAX && s->owner_test >= m->test_count)) return 0;
    }
    return 1;
}

/* Fixed-order grammar keeps duplicate fields/IDs, extra END records and
 * unrecognized data impossible to accept accidentally. */
typedef struct { const char *p, *end; } sts_observation_cursor;
static int sts_observation_literal(sts_observation_cursor *r, const char *s) {
    size_t n = strlen(s);
    if ((size_t)(r->end - r->p) < n || memcmp(r->p, s, n)) return 0;
    r->p += n; return 1;
}
static int sts_observation_number(sts_observation_cursor *r, uint64_t maximum,
                                   char delimiter, uint64_t *out) {
    const char *start = r->p;
    uint64_t value = 0;
    if (r->p == r->end || *r->p < '0' || *r->p > '9') return 0;
    while (r->p < r->end && *r->p >= '0' && *r->p <= '9') {
        uint64_t digit = (uint64_t)(*r->p++ - '0');
        if (digit > maximum || value > (maximum - digit) / 10) return 0;
        value = value * 10 + digit;
    }
    if ((r->p - start > 1 && *start == '0') || r->p == r->end || *r->p++ != delimiter) return 0;
    *out = value; return 1;
}
static int sts_observation_identity(sts_observation_cursor *r, const char *key, const char *value) {
    return sts_observation_literal(r, key) && sts_observation_literal(r, value) && sts_observation_literal(r, "\n");
}
static int sts_observation_hex(unsigned char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}
static int sts_observation_parse_report(const void *bytes, size_t size,
                                         const sts_observation_config *c,
                                         const sts_observation_manifest *m,
                                         sts_observation_report *out,
                                         char *error, size_t error_size) {
    if (!bytes || !size || size > STS_OBS_REPORT_LIMIT || !sts_observation_manifest_valid(c, m))
        return sts_observation_error(error, error_size, "invalid report size or expected manifest");
    memset(out, 0, sizeof(*out));
    sts_observation_cursor r = {(const char *)bytes, (const char *)bytes + size};
    uint64_t a, b, d;
#define OBS_LITERAL(s) do { if (!sts_observation_literal(&r, (s))) goto malformed; } while (0)
#define OBS_NUMBER(max, sep, dst) do { if (!sts_observation_number(&r, (max), (sep), &(dst))) goto malformed; } while (0)
    OBS_LITERAL("STASHA_OBSERVATION 1\nprofile native-sequential-leaf-v1\nruntime_abi 1\nmetric_versions 1 1\n");
    if (!sts_observation_identity(&r, "run ", c->run_id) ||
        !sts_observation_identity(&r, "source ", c->source_id) ||
        !sts_observation_identity(&r, "map ", m->map_id) ||
        !sts_observation_identity(&r, "config ", c->config_id) ||
        !sts_observation_identity(&r, "compiler ", c->compiler_id)) goto identity;
    OBS_LITERAL("enabled "); OBS_NUMBER(3, '\n', a); if (a != c->enabled_mask) goto identity;
    OBS_LITERAL("inventory "); OBS_NUMBER(STS_OBS_TEST_LIMIT, ' ', a);
    OBS_NUMBER(STS_OBS_LINE_LIMIT, ' ', b); OBS_NUMBER(STS_OBS_SITE_LIMIT, '\n', d);
    if (a != m->test_count || b != m->line_count || d != m->site_count) goto identity;
    OBS_LITERAL("state "); OBS_NUMBER(1, ' ', a); out->complete = (uint32_t)a;
    OBS_NUMBER(12, ' ', a); out->reason = (uint32_t)a;
    OBS_NUMBER(1, '\n', a); out->endpoint = (uint32_t)a;
    OBS_LITERAL("assertions "); OBS_NUMBER(UINT64_MAX, ' ', out->assertions_passed);
    OBS_NUMBER(UINT64_MAX, '\n', out->assertions_failed);
    if (out->assertions_passed > UINT64_MAX - out->assertions_failed) goto impossible;
    OBS_LITERAL("tests "); OBS_NUMBER(m->test_count, ' ', a); out->started = (uint32_t)a;
    OBS_NUMBER(m->test_count, '\n', b); out->completed = (uint32_t)b;
    if (b > a || a - b > 1) goto impossible;
    for (uint32_t i = 0; i < m->test_count; ++i) {
        OBS_LITERAL("test "); OBS_NUMBER(m->test_count - 1, ' ', a); if (a != i) goto impossible;
        OBS_NUMBER(2, '\n', b);
        uint64_t expected = i < out->completed ? 2 : i < out->started ? 1 : 0;
        if (b != expected) goto impossible;
    }
    if (out->endpoint && (out->started != m->test_count || out->completed != m->test_count)) goto impossible;
    if (out->complete && (!out->endpoint || out->reason != 0)) goto impossible;
    if (!out->complete && !out->reason) goto impossible;
    OBS_LITERAL("hits ");
    if (!m->line_count) OBS_LITERAL("-\n");
    else {
        size_t count = (m->line_count + 7u) / 8u;
        if ((size_t)(r.end - r.p) < count * 2 + 1) goto malformed;
        for (size_t i = 0; i < count; ++i) {
            int hi = sts_observation_hex((unsigned char)*r.p++), lo = sts_observation_hex((unsigned char)*r.p++);
            if (hi < 0 || lo < 0) goto malformed;
            out->hits[i] = (uint8_t)((hi << 4) | lo);
        }
        if (m->line_count % 8 && (out->hits[count - 1] >> (m->line_count % 8))) goto impossible;
        OBS_LITERAL("\n");
        for (uint32_t i = 0; i < m->line_count; ++i) out->covered += !!(out->hits[i / 8] & (1u << (i % 8)));
    }
    if (out->covered && !out->started) goto impossible;
    OBS_LITERAL("allocations "); OBS_NUMBER(STS_OBS_RECORD_LIMIT, ' ', a); out->live_count = (uint32_t)a;
    OBS_NUMBER(UINT64_MAX, '\n', b);
    uint64_t previous = 0;
    for (uint32_t i = 0; i < out->live_count; ++i) {
        sts_observation_live *live = &out->live[i];
        OBS_LITERAL("allocation "); OBS_NUMBER(UINT64_MAX, ' ', live->generation);
        if (!live->generation || live->generation <= previous) goto impossible;
        previous = live->generation;
        OBS_NUMBER(UINT64_MAX, ' ', live->bytes);
        OBS_NUMBER(UINT32_MAX, ' ', a); live->allocation_site = (uint32_t)a;
        OBS_NUMBER(UINT32_MAX, ' ', a); live->resize_site = (uint32_t)a;
        OBS_NUMBER(UINT32_MAX, '\n', a); live->test = (uint32_t)a;
        if (live->allocation_site >= m->site_count || live->test >= out->started ||
            (m->sites[live->allocation_site].kind != STS_OBS_NEW && m->sites[live->allocation_site].kind != STS_OBS_MOV) ||
            (live->resize_site != UINT32_MAX && (live->resize_site >= m->site_count || m->sites[live->resize_site].kind != STS_OBS_MOV)) ||
            (m->sites[live->allocation_site].owner_test != UINT32_MAX && m->sites[live->allocation_site].owner_test != live->test) ||
            out->live_bytes > UINT64_MAX - live->bytes) goto impossible;
        out->live_bytes += live->bytes;
    }
    if (b != out->live_bytes) goto impossible;
    OBS_LITERAL("END\n");
    if (r.p != r.end) goto malformed;
#undef OBS_LITERAL
#undef OBS_NUMBER
    return 0;
identity:
    return sts_observation_error(error, error_size, "report identity/configuration/inventory mismatch");
impossible:
    return sts_observation_error(error, error_size, "impossible report counts, state, bitmap or allocation records");
malformed:
    return sts_observation_error(error, error_size, "malformed, truncated, duplicate or unsupported observation report");
}

static int sts_observation_status(const sts_observation_config *c,
                                   const sts_observation_manifest *m,
                                   const sts_observation_report *r,
                                   const sts_observation_child *child) {
    if (!child->launched || !child->waited || child->launch_errno || child->wait_errno ||
        !child->exited || child->exit_code || child->signal || !r->complete || r->reason ||
        !r->endpoint || !m->test_count || r->started != m->test_count ||
        r->completed != m->test_count || r->assertions_failed) return 1;
    if (c->has_max_bytes && r->live_bytes > c->max_bytes) return 1;
    if (c->has_min_coverage && !sts_observation_coverage_pass(r->covered, m->line_count, c->min_coverage)) return 1;
    return 0;
}

#if defined(__linux__)
static int sts_observation_hash_fd(int fd, char out[129]) {
    unsigned char buffer[32768];
    sts_sha256_ctx hash; sts_sha256_init(&hash);
    if (lseek(fd, 0, SEEK_SET) < 0) return -1;
    for (;;) {
        ssize_t n = read(fd, buffer, sizeof(buffer));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return -1;
        if (!n) break;
        sts_sha256_update(&hash, buffer, (uint64_t)n);
    }
    sts_observation_digest_finish(&hash, out);
    return lseek(fd, 0, SEEK_SET) < 0 ? -1 : 0;
}
static int sts_observation_hash_file(const char *path, int nofollow, char out[129]) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | (nofollow ? O_NOFOLLOW : 0));
    if (fd < 0) return -1;
    struct stat st;
    int status = fstat(fd, &st) || !S_ISREG(st.st_mode) ? -1 : sts_observation_hash_fd(fd, out);
    int saved = errno;
    if (close(fd) && !status) return -1;
    errno = saved; return status;
}
static int sts_observation_prepare(sts_observation_config *c, sts_observation_run *run,
                                    const char *source, const char *runtime, const char *target,
                                    int optimization, int debug, char *error, size_t error_size) {
    memset(run, 0, sizeof(*run));
    run->image_fd = -1;
    run->canonical_source = realpath(source, NULL);
    if (!run->canonical_source) return sts_observation_error(error, error_size, "cannot resolve observation source");
    int fd = open(run->canonical_source, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;
    if (fd < 0) return sts_observation_error(error, error_size, "cannot open observation source");
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 || (uint64_t)st.st_size > STS_OBS_SOURCE_LIMIT) {
        close(fd); return sts_observation_error(error, error_size, "observation source must be a bounded ordinary file (at most 16 MiB)");
    }
    run->source_size = (size_t)st.st_size;
    run->source = malloc(run->source_size + 1);
    if (!run->source) { close(fd); return sts_observation_error(error, error_size, "cannot allocate observation source buffer"); }
    size_t offset = 0;
    while (offset < run->source_size) {
        ssize_t n = read(fd, run->source + offset, run->source_size - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { close(fd); return sts_observation_error(error, error_size, "observation source changed or could not be read"); }
        offset += (size_t)n;
    }
    char extra;
    ssize_t n;
    do { n = read(fd, &extra, 1); } while (n < 0 && errno == EINTR);
    int closed = close(fd);
    if (n != 0 || closed || memchr(run->source, 0, run->source_size))
        return sts_observation_error(error, error_size, "observation source changed, contains NUL, or failed to close");
    run->source[run->source_size] = 0;
    c->source_line_count = 1;
    for (size_t i = 0; i < run->source_size; ++i) if (run->source[i] == '\n') ++c->source_line_count;
    if (c->source_line_count > STS_OBS_SOURCE_LINES)
        return sts_observation_error(error, error_size, "observation source exceeds line capacity");
    c->source_path = run->canonical_source;
    sts_observation_digest(run->source, run->source_size, c->source_id);
    if (sts_observation_hash_file("/proc/self/exe", 0, c->compiler_id) ||
        sts_observation_hash_file(runtime, 1, run->runtime_id))
        return sts_observation_error(error, error_size, "cannot hash compiler or matching observation_runtime.a");
    snprintf(run->target, sizeof(run->target), "%s", target);
    char config[1024];
    int len = snprintf(config, sizeof(config), "observation-1\nprofile native-sequential-leaf-v1\ntarget %s\ncompiler %s\nruntime %s\nmask %u\nwarn %d\nmax %d %" PRIu64 "\nmin %d %u\noptimization %d\ndebug %d\n",
        target, c->compiler_id, run->runtime_id, c->enabled_mask, c->warn_memleak,
        c->has_max_bytes, c->max_bytes, c->has_min_coverage, c->min_coverage, optimization, debug);
    if (len < 0 || (size_t)len >= sizeof(config)) return sts_observation_error(error, error_size, "observation configuration exceeds capacity");
    sts_observation_digest(config, (size_t)len, c->config_id);
    snprintf(run->directory, sizeof(run->directory), "/tmp/stasha-observation-XXXXXX");
    if (!mkdtemp(run->directory)) { run->directory[0] = 0; return sts_observation_error(error, error_size, "cannot create private observation directory"); }
    snprintf(run->report_path, sizeof(run->report_path), "%s/report", run->directory);
    snprintf(run->metadata_path, sizeof(run->metadata_path), "%s/driver.json", run->directory);
    /* Directory uniqueness, process identity and compiler/config identity bind
     * this run. IDs are freshness labels, not cryptographic attestation. */
    char fresh[1024];
    len = snprintf(fresh, sizeof(fresh), "%s:%ld:%s:%s", run->directory, (long)getpid(), c->source_id, c->config_id);
    sts_observation_digest(fresh, (size_t)len, c->run_id);
    return 0;
}

static int sts_observation_wait_child(pid_t pid, sts_observation_child *out,
                                       pid_t (*wait_call)(pid_t, int *, int)) {
    int status = 0;
    pid_t got;
    do { got = wait_call(pid, &status, 0); } while (got < 0 && errno == EINTR);
    if (got != pid) { out->wait_errno = got < 0 ? errno : ECHILD; return -1; }
    out->waited = 1;
    if (WIFEXITED(status)) { out->exited = 1; out->exit_code = WEXITSTATUS(status); }
    else if (WIFSIGNALED(status)) out->signal = WTERMSIG(status);
    else { out->wait_errno = ECHILD; return -1; }
    return 0;
}
static int sts_observation_exec_once(int fd, char *const argv[], char *const env[],
                                     int (*exec_call)(int, char *const[], char *const[])) {
    exec_call(fd, argv, env);
    return errno ? errno : EIO; /* A returning exec always failed. */
}
static int sts_observation_launch(sts_observation_run *run, const char *image,
                                   sts_observation_child *out, char *error, size_t error_size) {
    memset(out, 0, sizeof(*out));
    /* The output transaction opened this descriptor before publication.
     * A concurrent replacement of the public output name cannot select another image. */
    int fd = run->image_fd;
    struct stat st, collision;
    if (fd < 0) return sts_observation_error(error, error_size, "cannot open freshly compiled observation image");
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || sts_observation_hash_fd(fd, run->binary_id)) {
        close(fd); run->image_fd = -1; return sts_observation_error(error, error_size, "cannot identify freshly compiled observation image");
    }
    if (lstat(run->report_path, &collision) == 0 || errno != ENOENT) {
        close(fd); run->image_fd = -1; return sts_observation_error(error, error_size, "observation report path collision");
    }
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC)) { close(fd); run->image_fd = -1; return sts_observation_error(error, error_size, "cannot create observation launch status pipe"); }
    pid_t pid = fork();
    if (pid == 0) {
        close(pipefd[0]);
        int failure;
        if (setenv("STASHA_OBSERVATION_REPORT", run->report_path, 1)) failure = errno;
        else {
            extern char **environ;
            char *args[] = {(char *)image, NULL};
            failure = sts_observation_exec_once(fd, args, environ, fexecve);
        }
        const char *p = (const char *)&failure;
        size_t left = sizeof(failure);
        while (left) {
            ssize_t count = write(pipefd[1], p, left);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) break;
            p += count; left -= (size_t)count;
        }
        _exit(127);
    }
    int fork_error = errno;
    int close_error = close(fd) ? errno : 0; run->image_fd = -1;
    if (close(pipefd[1]) && !close_error) close_error = errno;
    if (pid < 0) { close(pipefd[0]); out->launch_errno = fork_error; return sts_observation_error(error, error_size, "cannot fork observation child"); }
    out->launched = 1;
    out->launch_errno = close_error;
    int failure = 0;
    char *p = (char *)&failure;
    size_t used = 0;
    for (;;) {
        char more[sizeof(failure) + 1];
        ssize_t count = read(pipefd[0], more, sizeof(more));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { out->launch_errno = errno; break; }
        if (!count) break;
        if ((size_t)count > sizeof(failure) - used) { out->launch_errno = EIO; break; }
        memcpy(p + used, more, (size_t)count); used += (size_t)count;
    }
    if (close(pipefd[0]) && !out->launch_errno) out->launch_errno = errno;
    if (used) out->launch_errno = used == sizeof(failure) && failure ? failure : EIO;
    int waited = sts_observation_wait_child(pid, out, waitpid);
    if (out->launch_errno) return sts_observation_error(error, error_size, "observation image exec failed");
    if (waited) return sts_observation_error(error, error_size, "observation child wait failed");
    return 0;
}
static int sts_observation_read_report(const sts_observation_run *run,
                                        const sts_observation_config *c, const sts_observation_manifest *m,
                                        sts_observation_report *out, char *error, size_t error_size) {
    int fd = open(run->report_path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return sts_observation_error(error, error_size, "missing or inaccessible observation report");
    struct stat before, after;
    if (fstat(fd, &before) || !S_ISREG(before.st_mode) || before.st_nlink != 1 ||
        before.st_uid != geteuid() || (before.st_mode & 0777) != 0600 ||
        before.st_size <= 0 || (uint64_t)before.st_size > STS_OBS_REPORT_LIMIT) {
        close(fd); return sts_observation_error(error, error_size, "observation report is not a private bounded regular file");
    }
    size_t size = (size_t)before.st_size, used = 0;
    char *bytes = malloc(size);
    if (!bytes) { close(fd); return sts_observation_error(error, error_size, "cannot allocate observation report buffer"); }
    int bad = 0;
    while (used < size) {
        ssize_t n = read(fd, bytes + used, size - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { bad = 1; break; }
        used += (size_t)n;
    }
    char extra;
    ssize_t n;
    do { n = read(fd, &extra, 1); } while (n < 0 && errno == EINTR);
    if (n != 0 || fstat(fd, &after) || before.st_size != after.st_size ||
        before.st_dev != after.st_dev || before.st_ino != after.st_ino ||
        before.st_mtim.tv_sec != after.st_mtim.tv_sec || before.st_mtim.tv_nsec != after.st_mtim.tv_nsec) bad = 1;
    if (close(fd)) bad = 1;
    struct stat named;
    if (lstat(run->report_path, &named) || !S_ISREG(named.st_mode) ||
        named.st_dev != before.st_dev || named.st_ino != before.st_ino) bad = 1;
    int status = bad ? sts_observation_error(error, error_size, "truncated, replaced or unreadable observation report") :
        sts_observation_parse_report(bytes, size, c, m, out, error, error_size);
    free(bytes); return status;
}
#endif /* Linux process/file primitives */

/* Preserve Unicode text in JSON while keeping arbitrary filesystem/source
 * bytes reconstructible through the accompanying hexadecimal byte fields. */
static unsigned sts_observation_utf8_width(const unsigned char *p) {
    unsigned width = *p >= 0xc2 && *p <= 0xdf ? 2 :
                     *p >= 0xe0 && *p <= 0xef ? 3 :
                     *p >= 0xf0 && *p <= 0xf4 ? 4 : 0;
    if (!width) return 0;
    for (unsigned i = 1; i < width; ++i) if (p[i] < 0x80 || p[i] > 0xbf) return 0;
    if ((p[0] == 0xe0 && p[1] < 0xa0) || (p[0] == 0xed && p[1] > 0x9f) ||
        (p[0] == 0xf0 && p[1] < 0x90) || (p[0] == 0xf4 && p[1] > 0x8f)) return 0;
    return width;
}
static int sts_observation_json_string(FILE *file, const char *s) {
    if (fputc('"', file) == EOF) return -1;
    const unsigned char *p = (const unsigned char *)(s ? s : "");
    while (*p) {
        if (*p == '"' || *p == '\\') {
            if (fputc('\\', file) == EOF || fputc(*p++, file) == EOF) return -1;
        } else if (*p < 32 || *p == 127) {
            if (fprintf(file, "\\u%04x", *p++) < 0) return -1;
        } else if (*p >= 128) {
            unsigned width = sts_observation_utf8_width(p);
            if (width) { if (fwrite(p, 1, width, file) != width) return -1; p += width; }
            else if (fprintf(file, "\\u%04x", *p++) < 0) return -1;
        } else if (fputc(*p++, file) == EOF) return -1;
    }
    return fputc('"', file) == EOF ? -1 : 0;
}
static int sts_observation_json_bytes(FILE *file, const char *s) {
    if (fputc('"', file) == EOF) return -1;
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; ++p)
        if (fprintf(file, "%02x", *p) < 0) return -1;
    return fputc('"', file) == EOF ? -1 : 0;
}
static int sts_observation_write_metadata(const sts_observation_run *run,
                                           const sts_observation_config *c, const sts_observation_manifest *m,
                                           const sts_observation_child *child,
                                           const sts_observation_report *report, int parsed, int overall) {
#if defined(__linux__)
    int fd = open(run->metadata_path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); return -1; }
    int manifest_valid = sts_observation_manifest_valid(c, m);
    fprintf(f, "{\n\"schema\":1,\"profile\":\"native-sequential-leaf-v1\",\"target\":");
    sts_observation_json_string(f, run->target);
    fprintf(f, ",\n\"text_encoding\":\"UTF-8; raw dynamic text bytes in *_bytes_hex\",\n\"source_path\":"); sts_observation_json_string(f, c->source_path);
    fputs(",\"source_path_bytes_hex\":", f); sts_observation_json_bytes(f, c->source_path);
    fprintf(f, ",\n\"run\":\"%s\",\"source_sha256\":\"%s\",\"map_sha256\":\"%s\",\"config_sha256\":\"%s\",\n\"compiler_sha256\":\"%s\",\"runtime_sha256\":\"%s\",\"binary_sha256\":\"%s\",\n",
        c->run_id, c->source_id, m->map_id, c->config_id, c->compiler_id, run->runtime_id, run->binary_id);
    fprintf(f, "\"enabled\":%u,\"warn_memleak\":%s,\"max_leaked_bytes\":", c->enabled_mask, c->warn_memleak ? "true" : "false");
    if (c->has_max_bytes) fprintf(f, "%" PRIu64, c->max_bytes); else fputs("null", f);
    fputs(",\"min_test_coverage\":", f); if (c->has_min_coverage) fprintf(f, "%u", c->min_coverage); else fputs("null", f);
    fprintf(f, ",\n\"child\":{\"launched\":%d,\"waited\":%d,\"launch_errno\":%d,\"wait_errno\":%d,\"exited\":%d,\"exit_code\":%d,\"signal\":%d},\n\"report_valid\":%s,\"overall_status\":%d,\n",
        child->launched, child->waited, child->launch_errno, child->wait_errno, child->exited, child->exit_code, child->signal, parsed ? "true" : "false", overall);
    fprintf(f, "\"compile_link_success\":%s,\"manifest_valid\":%s,\"allocation_metric\":\"%s\",\"coverage_metric\":\"%s\",\n",
        run->compile_link_success ? "true" : "false", manifest_valid ? "true" : "false",
        !(c->enabled_mask & 1) ? "disabled" : parsed && report->complete && (!c->has_max_bytes || report->live_bytes <= c->max_bytes) ? "pass" : "fail",
        !(c->enabled_mask & 2) ? "disabled" : parsed && report->complete && sts_observation_coverage_pass(report->covered, m->line_count, c->min_coverage) ? "pass" : "fail");
    if (parsed) fprintf(f, "\"complete\":%u,\"reason\":%u,\"suite_endpoint\":%u,\"assertions_passed\":%" PRIu64 ",\"assertions_failed\":%" PRIu64 ",\"covered_lines\":%" PRIu64 ",\"live_allocations\":%u,\"tracked_outstanding_bytes\":%" PRIu64 ",\n",
        report->complete, report->reason, report->endpoint, report->assertions_passed, report->assertions_failed, report->covered, report->live_count, report->live_bytes);
    fputs("\"lines\":[", f);
    for (uint32_t i = 0; manifest_valid && i < m->line_count; ++i) fprintf(f, "%s%u", i ? "," : "", m->lines[i]);
    fputs("],\n\"tests\":[", f);
    for (uint32_t i = 0; manifest_valid && i < m->test_count; ++i) { fprintf(f, "%s{\"id\":%u,\"line\":%u,\"name\":", i ? "," : "", i, m->tests[i].line); sts_observation_json_string(f, m->tests[i].name); fputs(",\"name_bytes_hex\":", f); sts_observation_json_bytes(f, m->tests[i].name); fputc('}', f); }
    fputs("],\n\"sites\":[", f);
    for (uint32_t i = 0; manifest_valid && i < m->site_count; ++i) { const sts_observation_site *s = &m->sites[i]; fprintf(f, "%s{\"id\":%u,\"line\":%u,\"kind\":%u,\"owner_test\":%u,\"owner\":", i ? "," : "", i, s->line, s->kind, s->owner_test); sts_observation_json_string(f, s->owner); fputs(",\"owner_bytes_hex\":", f); sts_observation_json_bytes(f, s->owner); fputc('}', f); }
    fputs("],\n\"exclusions\":[", f);
    for (uint32_t i = 1; manifest_valid && i <= c->source_line_count; ++i) fprintf(f, "%s%u", i > 1 ? "," : "", m->exclusions[i]);
    fputs("]\n}\n", f);
    int bad = ferror(f); if (fclose(f)) bad = 1;
    return bad ? -1 : 0;
#else
    (void)run; (void)c; (void)m; (void)child; (void)report; (void)parsed; (void)overall;
    return -1;
#endif
}

static void sts_observation_print_summary(const sts_observation_config *c,
                                           const sts_observation_manifest *m,
                                           const sts_observation_report *r,
                                           const sts_observation_child *child, int parsed) {
    if (child->exited) fprintf(stderr, "observation: child exit %d\n", child->exit_code);
    else if (child->signal) fprintf(stderr, "observation: child signal %d\n", child->signal);
    else fprintf(stderr, "observation: child did not complete (launch errno %d, wait errno %d)\n", child->launch_errno, child->wait_errno);
    if (!parsed) return;
    fprintf(stderr, "observation: tests %u/%u returned; assertions %" PRIu64 " passed, %" PRIu64 " failed; %s (reason %u)\n",
        r->completed, m->test_count, r->assertions_passed, r->assertions_failed, r->complete ? "complete" : "incomplete", r->reason);
    if (!(c->enabled_mask & 1)) fputs("observation: allocation metric disabled\n", stderr);
    else {
        fprintf(stderr, "observation: tracked outstanding requested bytes %" PRIu64 " in %u allocations", r->live_bytes, r->live_count);
        if (c->has_max_bytes) fprintf(stderr, "; maximum %" PRIu64 " (%s)", c->max_bytes, r->complete && r->live_bytes <= c->max_bytes ? "pass" : "fail");
        fputc('\n', stderr);
        if (c->warn_memleak && r->live_count) fputs("warning: outstanding tracked allocations at suite endpoint\n", stderr);
        for (uint32_t i = 0; i < r->live_count; ++i) {
            const sts_observation_live *live = &r->live[i];
            fprintf(stderr, "  allocation #%" PRIu64 ": %" PRIu64 " requested bytes, source line %u (%s), test %u\n", live->generation, live->bytes,
                m->sites[live->allocation_site].line, m->sites[live->allocation_site].owner, live->test);
        }
    }
    if (!(c->enabled_mask & 2)) fputs("observation: executable statement/control-line coverage disabled\n", stderr);
    else {
        if (!m->line_count) fputs("observation: executable statement/control-line coverage N/A (0/0); minimum fails\n", stderr);
        else fprintf(stderr, "observation: executable statement/control-line coverage %" PRIu64 "/%u; minimum %u%% (%s)\n",
            r->covered, m->line_count, c->min_coverage, r->complete && sts_observation_coverage_pass(r->covered, m->line_count, c->min_coverage) ? "pass" : "fail");
        fputs("observation: uncovered source lines", stderr);
        for (uint32_t i = 0; i < m->line_count; ++i) if (!(r->hits[i / 8] & (1u << (i % 8)))) fprintf(stderr, " %u", m->lines[i]);
        fputc('\n', stderr);
    }
}
#endif
