/* Pure opaque-ID ledger tests. No test calls malloc/free/realloc adapters,
 * production pointer wrappers, native I/O adapters, or a generated program.
 * Compile independently: cc -std=c11 -Wall -Wextra -Werror -O2
 * tests/observation/runtime_ledger_tests.c -o /tmp/runtime_ledger_tests */
#include "../../src/runtime/observation_runtime.c"
#include <assert.h>

_Static_assert(STASHA_OBSERVATION_ABI_VERSION == 3u, "explicit v3 runtime ABI");
_Static_assert(STASHA_OBSERVATION_SCHEMA_VERSION == 3u, "explicit v3 report schema");
_Static_assert(STASHA_OBSERVATION_ALLOCATION_VERSION == 2u, "domain-qualified allocation metric");
_Static_assert(STASHA_OBSERVATION_LINE_VERSION == 2u, "file-qualified line metric");
_Static_assert(_Generic(&__stasha_obs_init_v3,
    void (*)(const char *, const char *, const char *, const char *, const char *,
             uint32_t, uint32_t, uint32_t, uint32_t): 1, default: 0),
    "v3 initializer retains its physical parameter types");

typedef struct {
    uintptr_t next_key, last_key;
    size_t last_size;
    unsigned alloc_calls, resize_calls, release_calls;
    int error_number;
} spy;

static obs_state state;
static spy allocator_spy;

static obs_alloc_result spy_new(void *context, size_t bytes) {
    spy *p = context;
    ++p->alloc_calls;
    p->last_size = bytes;
    obs_alloc_result result = { p->next_key, p->error_number };
    return result;
}
static obs_alloc_result spy_mov(void *context, uintptr_t key, size_t bytes) {
    spy *p = context;
    ++p->resize_calls;
    p->last_key = key;
    p->last_size = bytes;
    obs_alloc_result result = { p->next_key, p->error_number };
    return result;
}
static int spy_rem(void *context, uintptr_t key) {
    spy *p = context;
    ++p->release_calls;
    p->last_key = key;
    return p->error_number;
}
static const obs_allocator allocator = { &allocator_spy, spy_new, spy_mov, spy_rem };
static const char *const ids[5] = { "run01", "input_graph01", "map01", "config01", "compiler01" };

static void reset(uint32_t tests, uint32_t lines, uint32_t flags) {
    memset(&state, 0, sizeof(state));
    memset(&allocator_spy, 0, sizeof(allocator_spy));
    allocator_spy.error_number = 37;
    assert(obs_core_init(&state, ids, flags, tests, lines, 8));
}
static void start(void) { reset(1, 10, 3); assert(obs_core_begin(&state, 0)); }
static uintptr_t add(uintptr_t key, size_t bytes, uint32_t site) {
    allocator_spy.next_key = key;
    obs_alloc_result result = obs_core_new(&state, &allocator, bytes, site);
    assert(result.key == key);
    assert(result.error_number == 37);
    assert(state.reason == STASHA_OBSERVATION_OK);
    return result.key;
}
static void test_balances(void) {
    start();
    add(100, 0, 0);
    add(200, 1, 1);
    add(300, 8, 2);
    add(400, 4097, 3);
    assert(state.record_count == 4 && state.total_bytes == 4106);
    assert(state.records[0].bytes == 0 && state.records[0].generation == 1);
    assert(obs_audit(&state));
    assert(obs_core_rem(&state, &allocator, 200, 4) == 37);
    assert(obs_core_rem(&state, &allocator, 400, 4) == 37);
    assert(state.record_count == 2 && state.total_bytes == 8);
    assert(state.records[0].generation == 1 && state.records[1].generation == 3);
    assert(obs_core_rem(&state, &allocator, 300, 4) == 37);
    assert(state.record_count == 1 && state.total_bytes == 0);
    assert(obs_core_end(&state, 0));
    assert(obs_core_finish(&state, 5, 0));
    assert(state.endpoint && state.record_count == 1 && !state.reason);
}
static void test_null_and_failure(void) {
    start();
    add(0, 8, 0);
    assert(!state.record_count && !state.total_bytes && !state.generation);
    errno = 41;
    assert(obs_core_rem(&state, &allocator, 0, 1) == 41);
    assert(!allocator_spy.release_calls);
    allocator_spy.next_key = 111;
    obs_alloc_result r = obs_core_mov(&state, &allocator, 0, 8, 2);
    assert(r.key == 111 && r.error_number == 37);
    assert(allocator_spy.resize_calls == 1 && allocator_spy.alloc_calls == 1);
    assert(state.records[0].alloc_site == 2);
    assert(state.records[0].resize_site == STASHA_OBSERVATION_NO_SITE);
    obs_record before = state.records[0];
    allocator_spy.next_key = 0;
    allocator_spy.error_number = ENOMEM;
    r = obs_core_mov(&state, &allocator, 111, 100, 3);
    assert(!r.key && r.error_number == ENOMEM);
    assert(memcmp(&before, &state.records[0], sizeof(before)) == 0);
    assert(state.total_bytes == 8 && state.record_count == 1);
}
static void test_resize_alias_reuse(void) {
    start();
    uintptr_t alias = add(1000, 8, 1);
    allocator_spy.next_key = alias;
    assert(obs_core_mov(&state, &allocator, alias, 16, 2).key == alias);
    assert(state.total_bytes == 16 && state.records[0].resize_site == 2);
    allocator_spy.next_key = 2000;
    assert(obs_core_mov(&state, &allocator, alias, 3, 3).key == 2000);
    assert(state.total_bytes == 3 && state.record_count == 1);
    assert(state.records[0].generation == 1 && state.records[0].alloc_site == 1);
    assert(state.records[0].resize_site == 3 && state.records[0].origin_test == 0);
    assert(obs_core_rem(&state, &allocator, 2000, 4) == 37);
    add(1000, 8, 5);
    assert(state.records[0].generation == 2 && state.records[0].alloc_site == 5);
    assert(state.record_count == 1 && state.total_bytes == 8);
}
static void test_suite_and_hits(void) {
    reset(2, 10, 3);
    assert(obs_core_begin(&state, 0));
    add(20, 8, 0);
    assert(obs_core_hit(&state, 0));
    assert(obs_core_hit(&state, 0));
    assert(obs_core_hit(&state, 8));
    assert(obs_core_hit(&state, 9));
    assert(state.hits[0] == 1 && state.hits[1] == 3);
    assert(obs_core_end(&state, 0));
    assert(state.record_count == 1 && state.records[0].origin_test == 0);
    assert(obs_core_begin(&state, 1));
    assert(obs_core_rem(&state, &allocator, 20, 1) == 37);
    assert(obs_core_end(&state, 1));
    assert(obs_core_finish(&state, 0, 0));
    assert(!state.record_count && !state.total_bytes && state.endpoint);
    assert(state.started == 2 && state.completed == 2);
}
static void test_cross_test_attribution(void) {
    /* File-qualified source sites and test ownership are manifest metadata;
     * the ledger keeps their opaque IDs and the actual executing test. */
    reset(2, 10, 3);
    assert(obs_core_begin(&state, 0));
    add(100, 8, 0);
    assert(obs_core_end(&state, 0));
    assert(obs_core_begin(&state, 1));
    allocator_spy.next_key = 200;
    assert(obs_core_mov(&state, &allocator, 100, 16, 3).key == 200);
    add(300, 13, 5);
    assert(obs_core_end(&state, 1));
    assert(obs_core_finish(&state, 5, 0));
    assert(state.record_count == 2 && state.total_bytes == 29);
    assert(state.records[0].generation == 1 && state.records[0].alloc_site == 0);
    assert(state.records[0].resize_site == 3 && state.records[0].origin_test == 0);
    assert(state.records[1].generation == 2 && state.records[1].alloc_site == 5);
    assert(state.records[1].resize_site == STASHA_OBSERVATION_NO_SITE);
    assert(state.records[1].origin_test == 1 && state.records[1].bytes == 13);
    assert(!allocator_spy.release_calls); /* no per-test subtraction/release */
}
static void test_invalid_release_is_pure(void) {
    /* Integers 101 and 999 never become pointers; spy must not be called. */
    start(); add(100, 8, 0);
    obs_core_rem(&state, &allocator, 101, 1);
    assert(state.reason == STASHA_OBSERVATION_UNKNOWN_BASE);
    assert(!allocator_spy.release_calls && state.total_bytes == 8);
    obs_core_rem(&state, &allocator, 100, 1);
    assert(!allocator_spy.release_calls); /* sticky, even valid later events */
    start();
    obs_core_rem(&state, &allocator, 999, 1);
    assert(state.reason == STASHA_OBSERVATION_UNKNOWN_BASE);
    assert(!allocator_spy.release_calls);
    start(); add(100, 8, 0);
    obs_core_rem(&state, &allocator, 100, 1);
    assert(allocator_spy.release_calls == 1);
    obs_core_rem(&state, &allocator, 100, 1);
    assert(state.reason == STASHA_OBSERVATION_UNKNOWN_BASE);
    assert(allocator_spy.release_calls == 1);
}
static void test_invalid_resize_is_pure(void) {
    start(); add(100, 8, 0);
    obs_core_mov(&state, &allocator, 101, 8, 1);
    assert(state.reason == STASHA_OBSERVATION_UNKNOWN_BASE);
    assert(!allocator_spy.resize_calls && state.total_bytes == 8);
    start(); add(100, 8, 0);
    obs_core_mov(&state, &allocator, 100, 0, 1);
    assert(state.reason == STASHA_OBSERVATION_ZERO_RESIZE);
    assert(!allocator_spy.resize_calls && state.total_bytes == 8);
    start();
    obs_core_mov(&state, &allocator, 0, 0, 1);
    assert(state.reason == STASHA_OBSERVATION_ZERO_RESIZE);
    assert(!allocator_spy.resize_calls && !allocator_spy.alloc_calls);
}
static void test_arithmetic_without_large_allocations(void) {
    _Static_assert(sizeof(size_t) == sizeof(uint64_t), "native LP64 tests");
    start(); add(100, (size_t)UINT64_MAX, 0);
    add(200, 0, 1);
    assert(state.total_bytes == UINT64_MAX && state.record_count == 2);
    unsigned calls = allocator_spy.alloc_calls;
    allocator_spy.next_key = 300;
    obs_core_new(&state, &allocator, 1, 1);
    assert(state.reason == STASHA_OBSERVATION_TOTAL_OVERFLOW);
    assert(allocator_spy.alloc_calls == calls && state.total_bytes == UINT64_MAX);
    start(); add(100, (size_t)(UINT64_MAX - 1), 0); add(200, 1, 1);
    obs_core_mov(&state, &allocator, 200, 2, 2);
    assert(state.reason == STASHA_OBSERVATION_TOTAL_OVERFLOW);
    assert(!allocator_spy.resize_calls && state.total_bytes == UINT64_MAX);
    start(); state.generation = UINT64_MAX;
    obs_core_new(&state, &allocator, 1, 0);
    assert(state.reason == STASHA_OBSERVATION_GENERATION_OVERFLOW);
    assert(!allocator_spy.alloc_calls);
}
static void test_capacity_and_duplicates(void) {
    start();
    for (uint32_t i = 0; i < STASHA_OBSERVATION_MAX_RECORDS; ++i)
        add((uintptr_t)i + 1, 0, 0);
    allocator_spy.next_key = 999999;
    obs_core_new(&state, &allocator, 1, 0);
    assert(state.reason == STASHA_OBSERVATION_RECORD_CAPACITY);
    assert(allocator_spy.alloc_calls == STASHA_OBSERVATION_MAX_RECORDS);
    start(); add(100, 1, 0);
    allocator_spy.next_key = 100;
    obs_core_new(&state, &allocator, 8, 0);
    assert(state.reason == STASHA_OBSERVATION_DUPLICATE_BASE);
    assert(state.record_count == 1 && state.total_bytes == 1);
    start(); add(100, 1, 0); add(200, 2, 1);
    allocator_spy.next_key = 200;
    obs_core_mov(&state, &allocator, 100, 8, 2);
    assert(state.reason == STASHA_OBSERVATION_DUPLICATE_BASE);
    assert(state.record_count == 2 && state.total_bytes == 3);
}
static void test_lifecycle_and_manifest_bounds(void) {
    reset(0, 0, 1);
    assert(!obs_core_finish(&state, 0, 0));
    assert(state.reason == STASHA_OBSERVATION_BAD_LIFECYCLE);
    reset(1, 0, 1);
    assert(obs_core_begin(&state, 0)); assert(obs_core_end(&state, 0));
    assert(obs_core_finish(&state, 0, 0)); /* coverage N/A is driver policy */
    assert(!obs_core_finish(&state, 0, 0));
    assert(state.reason == STASHA_OBSERVATION_BAD_LIFECYCLE);
    start(); assert(!obs_core_finish(&state, 0, 0));
    assert(state.reason == STASHA_OBSERVATION_BAD_LIFECYCLE);
    reset(2, 10, 3); assert(!obs_core_begin(&state, 1));
    assert(state.reason == STASHA_OBSERVATION_BAD_LIFECYCLE);
    start(); assert(!obs_core_begin(&state, 0));
    assert(state.reason == STASHA_OBSERVATION_BAD_LIFECYCLE);
    start(); assert(!obs_core_end(&state, 1));
    assert(state.reason == STASHA_OBSERVATION_BAD_LIFECYCLE);
    start(); obs_core_new(&state, &allocator, 8, 8);
    assert(state.reason == STASHA_OBSERVATION_BAD_SITE && !allocator_spy.alloc_calls);
    start(); assert(!obs_core_hit(&state, 10));
    assert(state.reason == STASHA_OBSERVATION_BAD_LINE);
    start(); assert(obs_core_end(&state, 0));
    assert(obs_core_finish(&state, 1, 1)); /* failing assertions can be complete */
    assert(state.assertions_failed == 1 && state.endpoint);
    start(); assert(obs_core_end(&state, 0));
    assert(!obs_core_finish(&state, UINT64_MAX, 1));
}
static void test_configuration_and_corruption(void) {
    memset(&state, 0, sizeof(state));
    assert(!obs_core_init(&state, ids, 0, 1, 1, 1));
    assert(state.reason == STASHA_OBSERVATION_BAD_CONFIGURATION);
    memset(&state, 0, sizeof(state));
    assert(!obs_core_init(&state, ids, 3, STASHA_OBSERVATION_MAX_TESTS + 1, 1, 1));
    memset(&state, 0, sizeof(state));
    const char *const bad_ids[5] = { "run\nwrong", "input_graph", "map", "config", "compiler" };
    assert(!obs_core_init(&state, bad_ids, 3, 1, 1, 1));
    start(); add(100, 8, 0); state.total_bytes = 0;
    assert(!obs_audit(&state));
    assert(state.reason == STASHA_OBSERVATION_CORRUPTED_LEDGER);
    start(); add(100, 8, 0); state.records[0].origin_test = 10;
    assert(!obs_audit(&state));
}
static void test_configuration_boundaries(void) {
    const uint32_t invalid_counts[][3] = {
        { STASHA_OBSERVATION_MAX_TESTS + 1u, 1u, 1u },
        { 1u, STASHA_OBSERVATION_MAX_LINES + 1u, 1u },
        { 1u, 1u, STASHA_OBSERVATION_MAX_SITES + 1u }
    };
    for (size_t i = 0; i < sizeof(invalid_counts) / sizeof(invalid_counts[0]); ++i) {
        memset(&state, 0, sizeof(state));
        assert(!obs_core_init(&state, ids, 3u, invalid_counts[i][0],
                             invalid_counts[i][1], invalid_counts[i][2]));
        assert(state.reason == STASHA_OBSERVATION_BAD_CONFIGURATION);
        assert(!state.initialized);
    }
    memset(&state, 0, sizeof(state));
    assert(!obs_core_init(&state, ids, 4u, 1u, 1u, 1u));
    assert(state.reason == STASHA_OBSERVATION_BAD_CONFIGURATION);
    memset(&state, 0, sizeof(state));
    assert(obs_core_init(&state, ids, 3u, STASHA_OBSERVATION_MAX_TESTS,
                        STASHA_OBSERVATION_MAX_LINES, STASHA_OBSERVATION_MAX_SITES));
    assert(!obs_core_init(&state, ids, 3u, 1u, 1u, 1u));
    assert(state.reason == STASHA_OBSERVATION_BAD_LIFECYCLE);

    char maximum[STASHA_OBSERVATION_MAX_ID_BYTES + 1u];
    char overlong[STASHA_OBSERVATION_MAX_ID_BYTES + 2u];
    memset(maximum, 'a', sizeof(maximum) - 1u); maximum[sizeof(maximum) - 1u] = 0;
    memset(overlong, 'b', sizeof(overlong) - 1u); overlong[sizeof(overlong) - 1u] = 0;
    const char *const invalid_ids[] = { NULL, "", "two words", "line\nbreak", overlong };
    for (unsigned field = 0; field < 5u; ++field) {
        const char *candidate_ids[5];
        memcpy(candidate_ids, ids, sizeof(candidate_ids));
        candidate_ids[field] = maximum;
        memset(&state, 0, sizeof(state));
        assert(obs_core_init(&state, candidate_ids, 3u, 1u, 1u, 1u));
        assert(strcmp(state.identities[field], maximum) == 0);
        for (size_t i = 0; i < sizeof(invalid_ids) / sizeof(invalid_ids[0]); ++i) {
            candidate_ids[field] = invalid_ids[i];
            memset(&state, 0, sizeof(state));
            assert(!obs_core_init(&state, candidate_ids, 3u, 1u, 1u, 1u));
            assert(state.reason == STASHA_OBSERVATION_BAD_CONFIGURATION);
            assert(!state.initialized);
        }
    }
}

typedef struct { char bytes[32768]; size_t length, fail_after; unsigned calls; } text_sink;
static text_sink sink;
static int write_text(void *context, const char *bytes, size_t count) {
    text_sink *out = context;
    ++out->calls;
    if (count > sizeof(out->bytes) - out->length - 1 ||
        count > out->fail_after - out->length) return 0;
    memcpy(out->bytes + out->length, bytes, count);
    out->length += count;
    out->bytes[out->length] = 0;
    return 1;
}
static void reset_sink(void) { memset(&sink, 0, sizeof(sink)); sink.fail_after = sizeof(sink.bytes); }
static void test_report_literal(void) {
    start(); add(100, 8, 0);
    assert(obs_core_hit(&state, 0)); assert(obs_core_hit(&state, 9));
    assert(obs_core_end(&state, 0)); assert(obs_core_finish(&state, 2, 0));
    reset_sink(); obs_writer writer = { &sink, write_text, 0, 0 };
    assert(obs_emit_report(&state, &writer));
    const char expected[] =
        "STASHA_OBSERVATION 3\nprofile native-sequential-direct-source-slice-v3\nruntime_abi 3\nmetric_versions 2 2\n"
        "run run01\ninput_graph input_graph01\nmap map01\nconfig config01\ncompiler compiler01\n"
        "enabled 3\ninventory 1 10 8\nstate 1 0 1\nassertions 2 0\ntests 1 1\ntest 0 2\n"
        "hits 0102\nallocations 1 8\nallocation 1 1 8 0 4294967295 0\nEND\n";
    assert(strcmp(sink.bytes, expected) == 0);
    assert(writer.count == sizeof(expected) - 1);
    reset_sink(); sink.fail_after = 20;
    writer = (obs_writer){ &sink, write_text, 0, 0 };
    assert(!obs_emit_report(&state, &writer) && writer.failed);
    reset_sink(); writer = (obs_writer){ &sink, write_text, STASHA_OBSERVATION_MAX_REPORT_BYTES, 0 };
    obs_write(&writer, "x", 1);
    assert(writer.failed && !sink.calls);
    start(); add(100, 8, 0);
    obs_core_rem(&state, &allocator, 101, 1);
    reset_sink(); writer = (obs_writer){ &sink, write_text, 0, 0 };
    assert(obs_emit_report(&state, &writer));
    assert(strstr(sink.bytes, "state 0 5 0\n") != NULL);
    assert(strstr(sink.bytes, "test 0 1\n") != NULL);
    assert(strstr(sink.bytes, "allocations 1 8\n") != NULL);
    reset(1, 0, 1); assert(obs_core_begin(&state, 0)); assert(obs_core_end(&state, 0));
    assert(obs_core_finish(&state, 0, 0));
    reset_sink(); writer = (obs_writer){ &sink, write_text, 0, 0 };
    assert(obs_emit_report(&state, &writer));
    assert(strstr(sink.bytes, "hits -\nallocations 0 0\nEND\n") != NULL);
}

typedef struct { size_t bytes; unsigned calls; } counting_sink;
static int count_text(void *context, const char *bytes, size_t count) {
    counting_sink *out = context;
    assert(bytes != NULL && count <= STASHA_OBSERVATION_MAX_REPORT_BYTES - out->bytes);
    out->bytes += count;
    ++out->calls;
    return 1;
}
static void test_maximum_report_bounds(void) {
    /* Construct only opaque integer records, never real allocations. */
    char maximum_id[STASHA_OBSERVATION_MAX_ID_BYTES + 1u];
    memset(maximum_id, 'f', sizeof(maximum_id) - 1u);
    maximum_id[sizeof(maximum_id) - 1u] = 0;
    const char *const maximum_ids[5] = {
        maximum_id, maximum_id, maximum_id, maximum_id, maximum_id
    };
    memset(&state, 0, sizeof(state));
    assert(obs_core_init(&state, maximum_ids, 3u, STASHA_OBSERVATION_MAX_TESTS,
                        STASHA_OBSERVATION_MAX_LINES, STASHA_OBSERVATION_MAX_SITES));
    for (uint32_t i = 0; i < STASHA_OBSERVATION_MAX_TESTS; ++i) {
        assert(obs_core_begin(&state, i));
        assert(obs_core_end(&state, i));
    }
    memset(state.hits, 0xff, sizeof(state.hits));
    state.record_count = STASHA_OBSERVATION_MAX_RECORDS;
    state.total_bytes = UINT64_MAX;
    state.generation = UINT64_MAX;
    for (uint32_t i = 0; i < state.record_count; ++i) {
        state.records[i] = (obs_record){
            (uintptr_t)i + 1u, UINT64_MAX - state.record_count + i + 1u,
            i ? 0u : UINT64_MAX, STASHA_OBSERVATION_EXPLICIT_USER_HEAP, STASHA_OBSERVATION_MAX_SITES - 1u,
            STASHA_OBSERVATION_NO_SITE, STASHA_OBSERVATION_MAX_TESTS - 1u
        };
    }
    assert(obs_core_finish(&state, UINT64_MAX, 0u));
    counting_sink counter = { 0, 0 };
    obs_writer writer = { &counter, count_text, 0, 0 };
    assert(obs_emit_report(&state, &writer));
    assert(writer.count == counter.bytes && counter.calls > state.record_count);
    assert(writer.count < STASHA_OBSERVATION_MAX_REPORT_BYTES);
    assert(writer.count <= 1640454u); /* Frozen conservative whole-report bound. */
    reset_sink();writer=(obs_writer){&sink,write_text,0,0};
    obs_printf(&writer,"allocation %" PRIu64 " 1 %" PRIu64 " %" PRIu32 " %" PRIu32 " %" PRIu32 "\n",
        UINT64_MAX,UINT64_MAX,STASHA_OBSERVATION_MAX_SITES-1u,UINT32_MAX,STASHA_OBSERVATION_MAX_TESTS-1u);
    assert(!writer.failed&&writer.count==77u&&sink.length==77u);
    /* The configured bound is inclusive and stops before calling the sink. */
    reset_sink();
    writer = (obs_writer){ &sink, write_text, STASHA_OBSERVATION_MAX_REPORT_BYTES - 1u, 0 };
    obs_write(&writer, "x", 1u);
    assert(writer.count == STASHA_OBSERVATION_MAX_REPORT_BYTES && sink.calls == 1u);
    obs_write(&writer, "y", 1u);
    assert(writer.failed && sink.calls == 1u && sink.length == 1u);
    reset_sink();writer=(obs_writer){&sink,write_text,STASHA_OBSERVATION_MAX_REPORT_BYTES+1u,0};
    obs_write(&writer,"x",1);assert(writer.failed&&!sink.calls);
    writer=(obs_writer){&sink,write_text,SIZE_MAX,0};obs_write(&writer,"x",1);assert(writer.failed&&!sink.calls);
}

typedef struct {
    unsigned opens, writes, closes, flags, mode;
    int open_error, close_error;
    ssize_t scripted_writes[8];
    int scripted_errors[8];
    unsigned scripted_count, scripted_index;
} io_spy;
static io_spy publication_spy;
static int spy_open_report(void *context, const char *path, int flags, unsigned mode) {
    io_spy *p = context;
    ++p->opens; p->flags = (unsigned)flags; p->mode = mode;
    assert(strcmp(path, "private-test-directory/report") == 0);
    if (p->open_error) { errno = p->open_error; return -1; }
    return 42; /* opaque fake descriptor; never passed to the OS */
}
static ssize_t spy_write_report(void *context, int fd, const char *bytes, size_t count) {
    io_spy *p = context;
    assert(fd == 42);
    ++p->writes;
    ssize_t result = (ssize_t)count;
    if (p->scripted_index < p->scripted_count) {
        unsigned i = p->scripted_index++;
        result = p->scripted_writes[i];
        errno = p->scripted_errors[i];
    }
    if (result > 0 && (size_t)result <= count) {
        assert(write_text(&sink, bytes, (size_t)result));
    }
    return result;
}
static int spy_close_report(void *context, int fd) {
    io_spy *p = context;
    assert(fd == 42);
    ++p->closes;
    if (p->close_error) { errno = p->close_error; return -1; }
    return 0;
}
static const obs_io mock_io = {
    &publication_spy, spy_open_report, spy_write_report, spy_close_report
};
static void ready_to_publish(void) {
    start();
    assert(obs_core_hit(&state, 2));
    assert(obs_core_end(&state, 0));
    strcpy(state.path, "private-test-directory/report");
    memset(&publication_spy, 0, sizeof(publication_spy));
    reset_sink();
}
static void test_publication_controls(void) {
    ready_to_publish();
    publication_spy.scripted_count = 4;
    publication_spy.scripted_writes[0] = -1;
    publication_spy.scripted_errors[0] = EINTR;
    publication_spy.scripted_writes[1] = 1;
    publication_spy.scripted_writes[2] = -1;
    publication_spy.scripted_errors[2] = EINTR;
    publication_spy.scripted_writes[3] = 2;
    assert(obs_finish_publish(&state, 0, 0, &mock_io) == 0);
    assert(publication_spy.opens == 1 && publication_spy.closes == 1);
    assert(publication_spy.scripted_index == 4);
    assert(publication_spy.flags == (O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC));
    assert(publication_spy.mode == 0600);
    assert(strncmp(sink.bytes, "STASHA_OBSERVATION 3\n", 21) == 0);
    assert(strstr(sink.bytes, "state 1 0 1\n") != NULL);
    assert(strcmp(sink.bytes + sink.length - 4, "END\n") == 0);
    assert(obs_finish_publish(&state, 0, 0, &mock_io) == 1);
    assert(publication_spy.opens == 1); /* no duplicate or overwrite attempt */

    ready_to_publish(); publication_spy.open_error = EEXIST;
    assert(obs_finish_publish(&state, 0, 0, &mock_io) == 1);
    assert(state.reason == STASHA_OBSERVATION_REPORT_IO);
    assert(publication_spy.opens == 1 && !publication_spy.writes && !publication_spy.closes);

    ready_to_publish(); publication_spy.open_error = EACCES;
    assert(obs_finish_publish(&state, 0, 0, &mock_io) == 1);
    assert(state.reason == STASHA_OBSERVATION_REPORT_IO && !publication_spy.writes);

    ready_to_publish(); publication_spy.scripted_count = 1;
    publication_spy.scripted_writes[0] = 0;
    assert(obs_finish_publish(&state, 0, 0, &mock_io) == 1);
    assert(state.reason == STASHA_OBSERVATION_REPORT_IO);
    assert(publication_spy.writes == 1 && publication_spy.closes == 1);

    ready_to_publish(); publication_spy.scripted_count = 2;
    publication_spy.scripted_writes[0] = 2;
    publication_spy.scripted_writes[1] = -1;
    publication_spy.scripted_errors[1] = ENOSPC;
    assert(obs_finish_publish(&state, 0, 0, &mock_io) == 1);
    assert(state.reason == STASHA_OBSERVATION_REPORT_IO);
    assert(publication_spy.writes == 2 && publication_spy.closes == 1 && sink.length == 2);

    ready_to_publish(); publication_spy.close_error = EIO;
    assert(obs_finish_publish(&state, 0, 0, &mock_io) == 1);
    assert(state.reason == STASHA_OBSERVATION_REPORT_IO && publication_spy.closes == 1);
    assert(strstr(sink.bytes, "state 1 0 1\n") != NULL); /* child failure wins */

    ready_to_publish(); publication_spy.close_error = EINTR;
    assert(obs_finish_publish(&state, 0, 0, &mock_io) == 1);
    assert(state.reason == STASHA_OBSERVATION_REPORT_IO && publication_spy.closes == 1);

    ready_to_publish(); state.path[0] = 0;
    assert(obs_finish_publish(&state, 0, 0, &mock_io) == 1);
    assert(state.reason == STASHA_OBSERVATION_REPORT_IO && !publication_spy.opens);
    assert(!allocator_spy.alloc_calls && !allocator_spy.resize_calls && !allocator_spy.release_calls);
}

static uintptr_t add_slice(uintptr_t key, size_t bytes, uint32_t site) {
    allocator_spy.next_key=key;
    obs_alloc_result result=obs_core_slice_new(&state,&allocator,bytes,site);
    assert(result.key==key&&result.error_number==allocator_spy.error_number&&!state.reason);
    assert(allocator_spy.last_size==bytes);
    return result.key;
}
static void test_mixed_domains_and_reuse(void) {
    start();add(100,7,0);add_slice(200,20,3);
    assert(state.record_count==2&&state.total_bytes==27&&state.generation==2);
    assert(state.records[0].domain==1&&state.records[1].domain==2);
    assert(state.records[1].alloc_site==3&&state.records[1].origin_test==0&&state.records[1].resize_site==UINT32_MAX);
    assert(obs_audit(&state));
    assert(obs_core_end(&state,0)&&obs_core_finish(&state,0,0));
    reset_sink();obs_writer writer={&sink,write_text,0,0};assert(obs_emit_report(&state,&writer));
    const char records[]="allocations 2 27\nallocation 1 1 7 0 4294967295 0\nallocation 2 2 20 3 4294967295 0\nEND\n";
    assert(sizeof(records)-1==86&&!strcmp(strstr(sink.bytes,"allocations "),records));
    start();add_slice(100,20,3);add(200,7,0);
    assert(obs_core_slice_rem(&state,&allocator,100,4)==37);
    add(100,11,1);assert(state.records[1].generation==3&&state.records[1].domain==1);
    assert(obs_core_rem(&state,&allocator,200,2)==37);
    add_slice(200,8,3);assert(state.records[1].generation==4&&state.records[1].domain==2);
    assert(obs_core_rem(&state,&allocator,100,2)==37&&obs_core_slice_rem(&state,&allocator,200,4)==37);
    assert(!state.record_count&&!state.total_bytes&&state.generation==4&&allocator_spy.release_calls==4);
    assert(obs_audit(&state));
    /* Suite attribution remains dynamic across source files/tests, with no implicit release. */
    reset(2,0,2);assert(obs_core_begin(&state,0));add_slice(400,12,3);assert(obs_core_end(&state,0));
    assert(obs_core_begin(&state,1));add_slice(500,8,3);assert(obs_core_end(&state,1));
    assert(obs_core_finish(&state,0,0));assert(state.records[0].origin_test==0&&state.records[1].origin_test==1);
    assert(state.total_bytes==20&&!allocator_spy.release_calls);
}
static void test_wrong_domains_before_callbacks(void) {
    for(unsigned op=0;op<4;++op){
        start();add_slice(100,20,3);unsigned calls=allocator_spy.alloc_calls;
        if(op==0)obs_core_rem(&state,&allocator,100,4);
        else obs_core_mov(&state,&allocator,100,op==1?0:op==2?8:SIZE_MAX,2);
        assert(state.reason==STASHA_OBSERVATION_WRONG_DOMAIN);
        assert(!allocator_spy.release_calls&&!allocator_spy.resize_calls&&allocator_spy.alloc_calls==calls);
        assert(state.record_count==1&&state.total_bytes==20&&state.generation==1);
    }
    start();add(100,7,0);obs_core_slice_rem(&state,&allocator,100,4);
    assert(state.reason==STASHA_OBSERVATION_WRONG_DOMAIN&&!allocator_spy.release_calls&&state.total_bytes==7);
    for(unsigned bad=0;bad<2;++bad){
        start();add_slice(100,20,3);state.records[0].domain=bad?UINT32_MAX:0;
        obs_core_slice_rem(&state,&allocator,100,4);
        assert(state.reason==STASHA_OBSERVATION_CORRUPTED_LEDGER&&!allocator_spy.release_calls);
        start();add(100,7,0);state.records[0].domain=bad?UINT32_MAX:0;
        obs_core_mov(&state,&allocator,100,8,2);
        assert(state.reason==STASHA_OBSERVATION_CORRUPTED_LEDGER&&!allocator_spy.resize_calls);
    }
    start();obs_core_new_domain(&state,&allocator,8,0,3);
    assert(state.reason==STASHA_OBSERVATION_CORRUPTED_LEDGER&&!allocator_spy.alloc_calls);
}
static void test_slice_null_and_invalid_is_pure(void) {
    start();errno=41;allocator_spy.error_number=ENOMEM;
    obs_alloc_result result=obs_core_slice_new(&state,&allocator,20,3);
    assert(!result.key&&result.error_number==ENOMEM&&errno==41);
    assert(!state.record_count&&!state.total_bytes&&!state.generation&&allocator_spy.alloc_calls==1);
    assert(obs_core_slice_rem(&state,&allocator,0,4)==41&&!allocator_spy.release_calls);
    start();errno=41;result=obs_core_slice_new(&state,&allocator,0,3);
    assert(!result.key&&result.error_number==41&&errno==41&&state.reason==STASHA_OBSERVATION_INVALID_SLICE_SIZE);
    assert(!allocator_spy.alloc_calls&&!state.record_count&&!state.generation);
    start();obs_core_slice_new(&state,&allocator,20,8);
    assert(state.reason==STASHA_OBSERVATION_BAD_SITE&&!allocator_spy.alloc_calls);
    for(unsigned which=0;which<3;++which){
        start();add_slice(100,20,3);
        if(which==2){obs_core_slice_rem(&state,&allocator,100,4);assert(allocator_spy.release_calls==1);}
        obs_core_slice_rem(&state,&allocator,which==0?101:which==1?999:100,4);
        assert(state.reason==STASHA_OBSERVATION_UNKNOWN_BASE&&allocator_spy.release_calls==(which==2));
    }
    start();add(100,7,0);allocator_spy.next_key=100;obs_core_slice_new(&state,&allocator,20,3);
    assert(state.reason==STASHA_OBSERVATION_DUPLICATE_BASE&&state.total_bytes==7&&state.record_count==1);
}
static void test_slice_arithmetic_and_corruption(void) {
    start();add(100,SIZE_MAX,0);unsigned calls=allocator_spy.alloc_calls;
    obs_core_slice_new(&state,&allocator,1,3);
    assert(state.reason==STASHA_OBSERVATION_TOTAL_OVERFLOW&&allocator_spy.alloc_calls==calls);
    start();state.generation=UINT64_MAX;obs_core_slice_new(&state,&allocator,1,3);
    assert(state.reason==STASHA_OBSERVATION_GENERATION_OVERFLOW&&!allocator_spy.alloc_calls);
    start();state.record_count=STASHA_OBSERVATION_MAX_RECORDS;
    for(uint32_t i=0;i<state.record_count;++i)state.records[i].domain=1;
    obs_core_slice_new(&state,&allocator,1,3);
    assert(state.reason==STASHA_OBSERVATION_RECORD_CAPACITY&&!allocator_spy.alloc_calls);
    for(unsigned which=0;which<5;++which){
        start();add(100,7,0);add_slice(200,20,3);
        if(which==0)state.records[1].domain=3;
        if(which==1)state.records[1].bytes=0;
        if(which==2)state.records[1].resize_site=2;
        if(which==3){state.records[0].bytes=UINT64_MAX;state.total_bytes=19;}
        if(which==4){state.records[0].domain=2;state.records[0].bytes=UINT64_MAX;state.total_bytes=19;}
        assert(!obs_audit(&state)&&state.reason==STASHA_OBSERVATION_CORRUPTED_LEDGER);
    }
}

int main(void) {
    test_balances();
    test_null_and_failure();
    test_resize_alias_reuse();
    test_suite_and_hits();
    test_cross_test_attribution();
    test_invalid_release_is_pure();
    test_invalid_resize_is_pure();
    test_arithmetic_without_large_allocations();
    test_capacity_and_duplicates();
    test_lifecycle_and_manifest_bounds();
    test_configuration_and_corruption();
    test_configuration_boundaries();
    test_report_literal();
    test_maximum_report_bounds();
    test_publication_controls();
    test_mixed_domains_and_reuse();
    test_wrong_domains_before_callbacks();
    test_slice_null_and_invalid_is_pure();
    test_slice_arithmetic_and_corruption();
    puts("observation runtime pure ledger tests: 19 groups passed");
    return 0;
}
