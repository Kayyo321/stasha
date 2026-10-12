#ifndef STASHA_OBSERVATION_RUNTIME_H
#define STASHA_OBSERVATION_RUNTIME_H

/* Separately versioned from the language ABI. Sequential native LP64 only. */
#include <stddef.h>
#include <stdint.h>

#define STASHA_OBSERVATION_ABI_VERSION 2u
#define STASHA_OBSERVATION_SCHEMA_VERSION 2u
#define STASHA_OBSERVATION_ALLOCATION_VERSION 1u
#define STASHA_OBSERVATION_LINE_VERSION 2u
#define STASHA_OBSERVATION_PROFILE "native-sequential-direct-source-v2"
#define STASHA_OBSERVATION_ALLOCATIONS 1u
#define STASHA_OBSERVATION_COVERAGE 2u
#define STASHA_OBSERVATION_MAX_TESTS 4096u
#define STASHA_OBSERVATION_MAX_LINES 65536u
#define STASHA_OBSERVATION_MAX_SITES 65536u
#define STASHA_OBSERVATION_MAX_RECORDS 16384u
#define STASHA_OBSERVATION_MAX_REPORT_BYTES (2u * 1024u * 1024u)
#define STASHA_OBSERVATION_MAX_ID_BYTES 128u
#define STASHA_OBSERVATION_MAX_PATH_BYTES 4095u
#define STASHA_OBSERVATION_NO_SITE UINT32_MAX
#define STASHA_OBSERVATION_NO_TEST UINT32_MAX

enum stasha_observation_reason {
    STASHA_OBSERVATION_OK = 0,
    STASHA_OBSERVATION_BAD_CONFIGURATION = 1,
    STASHA_OBSERVATION_BAD_LIFECYCLE = 2,
    STASHA_OBSERVATION_BAD_SITE = 3,
    STASHA_OBSERVATION_BAD_LINE = 4,
    STASHA_OBSERVATION_UNKNOWN_BASE = 5,
    STASHA_OBSERVATION_ZERO_RESIZE = 6,
    STASHA_OBSERVATION_RECORD_CAPACITY = 7,
    STASHA_OBSERVATION_TOTAL_OVERFLOW = 8,
    STASHA_OBSERVATION_DUPLICATE_BASE = 9,
    STASHA_OBSERVATION_GENERATION_OVERFLOW = 10,
    STASHA_OBSERVATION_CORRUPTED_LEDGER = 11,
    STASHA_OBSERVATION_REPORT_IO = 12
};

#ifdef __cplusplus
extern "C" {
#endif

/* The second identity binds the complete frozen input graph, including a
 * zero-import root graph. There is deliberately no v1 initialization alias:
 * an old generated image must not silently enter this protocol. */
void __stasha_obs_init_v2(const char *run_id, const char *input_graph_id,
    const char *map_id, const char *config_id, const char *compiler_id,
    uint32_t enabled_mask, uint32_t test_count, uint32_t line_count,
    uint32_t site_count);
void __stasha_obs_test_begin(uint32_t test_id);
void __stasha_obs_test_end(uint32_t test_id);
void __stasha_obs_hit(uint32_t line_id);
void *__stasha_obs_new(size_t requested_bytes, uint32_t site_id);
void *__stasha_obs_mov(void *base, size_t requested_bytes, uint32_t site_id);
void __stasha_obs_rem(void *base, uint32_t site_id);
/* Called after the final test returns/cleans up and the harness summary.
 * Returns zero only if measurement and exclusive report publication succeeded.
 * The caller must independently retain its assertion/test failure status. */
int __stasha_obs_finish(uint64_t assertions_passed, uint64_t assertions_failed);

#ifdef __cplusplus
}
#endif

#endif
