#ifndef STASHA_OBSERVATION_OPTIONS_H
#define STASHA_OBSERVATION_OPTIONS_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "../../std/crypto/sha256_rt.h"
#include "../runtime/observation_runtime.h"

/* Separate from the language ABI. Inputs are frozen before lowering; the
 * copied manifest survives AST cleanup and belongs to the driver. */
typedef struct {
    int warn_memleak, has_max_bytes, has_min_coverage;
    uint64_t max_bytes;
    uint32_t min_coverage, enabled_mask;
    char run_id[129], source_id[129], config_id[129], compiler_id[129];
    const char *source_path;
    uint32_t source_line_count;
} sts_observation_config;

enum {
    STS_OBS_ELIGIBLE = 0, STS_OBS_NONEXEC = 1, STS_OBS_TEST = 2,
    STS_OBS_ROOT_MAIN = 3, STS_OBS_INACTIVE = 4, STS_OBS_UNREACHABLE = 5
};
enum { STS_OBS_NEW = 1, STS_OBS_MOV = 2, STS_OBS_REM = 3 };

typedef struct {
    uint32_t line, kind, owner_test;
    char *owner;
} sts_observation_site;
typedef struct { uint32_t line; char *name; } sts_observation_test;
typedef struct {
    char map_id[129];
    uint32_t test_count, line_count, site_count;
    uint32_t *lines;
    sts_observation_site *sites;
    sts_observation_test *tests;
    uint8_t *exclusions; /* Indexed by physical source line, including slot 0. */
} sts_observation_manifest;

static inline int sts_observation_parse_decimal(const char *text, uint64_t maximum,
                                                 uint64_t *out) {
    uint64_t value = 0;
    if (!text || !*text) return 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (*p < '0' || *p > '9') return 0;
        uint64_t digit = (uint64_t)(*p - '0');
        if (digit > maximum || value > (maximum - digit) / 10) return 0;
        value = value * 10 + digit;
    }
    *out = value;
    return 1;
}

/* Inclusive integer comparison; no floating point or overflowing 100*covered. */
static inline int sts_observation_coverage_pass(uint64_t covered, uint64_t eligible,
                                                uint32_t minimum) {
    if (!eligible || covered > eligible || minimum > 100) return 0;
    uint64_t needed = (eligible / 100) * minimum;
    uint64_t tail = (eligible % 100) * minimum;
    needed += tail / 100 + (tail % 100 != 0);
    return covered >= needed;
}

static inline void sts_observation_digest_finish(sts_sha256_ctx *ctx, char out[129]) {
    static const char hex[] = "0123456789abcdef";
    uint8_t digest[STS_SHA256_DIGEST_LEN];
    sts_sha256_final(digest, ctx);
    for (unsigned i = 0; i < STS_SHA256_DIGEST_LEN; ++i) {
        out[2*i] = hex[digest[i] >> 4];
        out[2*i+1] = hex[digest[i] & 15];
    }
    out[64] = '\0';
}
static inline void sts_observation_digest(const void *bytes, size_t size, char out[129]) {
    sts_sha256_ctx ctx;
    sts_sha256_init(&ctx);
    sts_sha256_update(&ctx, (const uint8_t *)bytes, (uint64_t)size);
    sts_observation_digest_finish(&ctx, out);
}

#endif
