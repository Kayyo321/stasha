#ifndef STASHA_OBSERVATION_OPTIONS_H
#define STASHA_OBSERVATION_OPTIONS_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "../../std/crypto/sha256_rt.h"
#include "../runtime/observation_runtime.h"

/* All observation limits are independent from the ordinary language ABI. */
#define STS_OBS_SOURCE_LIMIT (16u * 1024u * 1024u)
#define STS_OBS_SOURCE_LINES 1000000u
#define STS_OBS_FILE_LIMIT 256u
#define STS_OBS_EDGE_LIMIT 255u
#define STS_OBS_DECL_LIMIT 65536u
#define STS_OBS_ANCHOR_LIMIT 65536u
#define STS_OBS_MANIFEST_LIMIT (32u * 1024u * 1024u)
#define STS_OBS_METADATA_LIMIT (32u * 1024u * 1024u)
#define STS_OBS_IDENTIFIER_LIMIT 127u
#define STS_OBS_MODULE_LIMIT 255u
#define STS_OBS_SYMBOL_LIMIT 511u
#define STS_OBS_ROOT_PATH_LIMIT 511u
#define STS_OBS_IMPORT_PATH_LIMIT 1023u
#define STS_OBS_PATH_LIMIT 4095u
#define STS_OBS_LABEL_LIMIT 4096u
#define STS_OBS_LOOKUP_POLICY "requested-entry-directory-v1"
#define STS_OBS_DUPLICATE_POLICY "reject-v1"
#define STS_OBS_ALIAS_POLICY "global-unique-exported-target-v1"
#define STS_OBS_ADMISSION_POLICY "closed-direct-source-scalar-v2"
#define STS_OBS_RESOURCE_POLICY "direct-source-bounds-v2"
enum { STS_OBS_ROOT = 0, STS_OBS_IMPORT = 1 };
typedef struct {
    uint32_t file_id, role;
    char *lookup_path, *canonical_path, *requested_module, *declared_module, *bytes;
    size_t byte_length;
    uint32_t physical_line_count;
    uint64_t snapshot_device, snapshot_inode; /* Revalidation only; never hashed/deduplicated. */
    char content_id[129];
} sts_observation_source;
typedef struct {
    uint32_t parent_file_id, child_file_id, line, col, ordinal;
    char *requested_module, *lookup_path;
} sts_observation_edge;
typedef struct sts_observation_graph {
    char *invocation_directory, *original_root_spelling, *entry_directory;
    uint32_t file_count, edge_count, physical_line_count;
    size_t byte_length;
    sts_observation_source files[STS_OBS_FILE_LIMIT];
    sts_observation_edge edges[STS_OBS_EDGE_LIMIT];
    char input_graph_id[129];
    int frozen, failed;
} sts_observation_graph;

typedef struct {
    int warn_memleak, has_max_bytes, has_min_coverage;
    uint64_t max_bytes;
    uint32_t min_coverage, enabled_mask;
    char run_id[129], input_graph_id[129], config_id[129], compiler_id[129];
    const char *source_path;
    uint32_t source_line_count; /* Root-only convenience, never graph identity. */
    sts_observation_graph *graph;
} sts_observation_config;

enum {
    STS_OBS_ELIGIBLE = 0, STS_OBS_NONEXEC = 1, STS_OBS_TEST = 2,
    STS_OBS_ROOT_MAIN = 3, STS_OBS_INACTIVE = 4, STS_OBS_UNREACHABLE = 5
};
/* Stable AST node_kind_t values copied into the v2 schema; compiler asserts
 * these mirrors so introducing/changing a kind cannot silently reinterpret it. */
enum { STS_OBS_DECL_FN=1, STS_OBS_DECL_VAR=2, STS_OBS_DECL_TYPE=3,
       STS_OBS_DECL_IMPORT=6, STS_OBS_DECL_COMPTIME_ASSERT=27, STS_OBS_DECL_TEST=58 };
enum { STS_OBS_NEW = 1, STS_OBS_MOV = 2, STS_OBS_REM = 3 };
enum { STS_OBS_ANCHOR_STATEMENT = 1, STS_OBS_ANCHOR_CONTROL = 2, STS_OBS_ANCHOR_DEFER = 3 };
typedef struct {
    uint32_t line, kind, owner_test;
    char *owner;
    uint32_t file_id, decl_id, col, end_line, end_col;
} sts_observation_site;
typedef struct {
    uint32_t line; char *name;
    uint32_t file_id, decl_id, col;
} sts_observation_test;
typedef struct {
    uint32_t file_id, role, physical_line_count;
    char content_id[129];
    uint32_t *reason_masks; /* Physical-line indexed, including unused slot 0. */
} sts_observation_file_manifest;
typedef struct {
    uint32_t decl_id, file_id, ordinal_in_file, kind, line, col, end_line, end_col;
    char *module_name, *name;
    uint32_t source_linkage, selected;
} sts_observation_declaration;
typedef struct {
    uint32_t anchor_id, decl_id, file_id, line, col, end_line, end_col;
    uint32_t kind, exclusion_reason, has_coverage_id, coverage_id;
} sts_observation_anchor;
typedef struct {
    char map_id[129];
    uint32_t test_count, line_count, site_count;
    uint32_t *lines, *line_file_ids;
    sts_observation_site *sites;
    sts_observation_test *tests;
    uint8_t *exclusions; /* Legacy root-only view; not serialized or authoritative. */
    uint32_t file_count, decl_count, anchor_count;
    sts_observation_file_manifest *files;
    sts_observation_declaration *declarations;
    sts_observation_anchor *anchors;
    size_t serialized_size;
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

/* Canonical v2 hash writer. No struct padding or host integer representation
 * enters any digest. Optional capture supports independent known-answer tests. */
typedef struct {
    sts_sha256_ctx hash;
    size_t size, limit, capture_capacity;
    unsigned char *capture;
    int failed;
} sts_observation_encoder;
static inline void sts_observation_encode_init(sts_observation_encoder *e, size_t limit) {
    memset(e, 0, sizeof(*e)); e->limit = limit; sts_sha256_init(&e->hash);
}
static inline void sts_observation_encode_raw(sts_observation_encoder *e, const void *p, size_t n) {
    if (e->failed) return;
    if ((n && !p) || e->size > e->limit || n > e->limit - e->size ||
        (e->capture && (e->size > e->capture_capacity || n > e->capture_capacity - e->size))) {
        e->failed = 1; return;
    }
    if (e->capture && n) memcpy(e->capture + e->size, p, n);
    if (n) sts_sha256_update(&e->hash, (const uint8_t *)p, (uint64_t)n);
    e->size += n;
}
static inline void sts_observation_encode_u32(sts_observation_encoder *e, uint32_t v) {
    unsigned char b[4]; for (unsigned i=0;i<4;++i) b[3-i]=(unsigned char)(v>>(8*i));
    sts_observation_encode_raw(e,b,4);
}
static inline void sts_observation_encode_u64(sts_observation_encoder *e, uint64_t v) {
    unsigned char b[8]; for (unsigned i=0;i<8;++i) b[7-i]=(unsigned char)(v>>(8*i));
    sts_observation_encode_raw(e,b,8);
}
static inline void sts_observation_encode_bytes(sts_observation_encoder *e, const void *p, size_t n) {
    sts_observation_encode_u64(e,(uint64_t)n); sts_observation_encode_raw(e,p,n);
}
static inline void sts_observation_encode_text(sts_observation_encoder *e, const char *p) {
    if (!p) {e->failed=1;return;} sts_observation_encode_bytes(e,p,strlen(p));
}
static inline void sts_observation_encode_optional_text(sts_observation_encoder *e, const char *p) {
    sts_observation_encode_u32(e,p != NULL); if(p) sts_observation_encode_text(e,p);
}
static inline int sts_observation_digest_hex_value(unsigned char c) {
    return c >= '0' && c <= '9' ? c-'0' : c >= 'a' && c <= 'f' ? c-'a'+10 : -1;
}
static inline void sts_observation_encode_digest(sts_observation_encoder *e, const char *s) {
    unsigned char b[32];
    if (!s || strlen(s)!=64) {e->failed=1;return;}
    for(unsigned i=0;i<32;++i) {
        int a=sts_observation_digest_hex_value((unsigned char)s[i*2]);
        int z=sts_observation_digest_hex_value((unsigned char)s[i*2+1]);
        if(a<0||z<0){e->failed=1;return;} b[i]=(unsigned char)((a<<4)|z);
    }
    sts_observation_encode_raw(e,b,32);
}
static inline void sts_observation_encode_optional_u32(sts_observation_encoder *e, int present, uint32_t v) {
    sts_observation_encode_u32(e,present ? 1u : 0u); if(present) sts_observation_encode_u32(e,v);
}
/* Configuration field order is frozen with the v2 encoding vectors. */
static inline int sts_observation_config_digest(sts_observation_config *c,const char *runtime_id,
                                                 const char *target,int optimization,int debug) {
    if(!c||!target||strlen(target)>255||optimization<0||optimization>3||debug<0||debug>1||
       c->warn_memleak<0||c->warn_memleak>1||c->has_max_bytes<0||c->has_max_bytes>1||
       c->has_min_coverage<0||c->has_min_coverage>1||c->min_coverage>100||
       c->enabled_mask!=(uint32_t)(((c->warn_memleak||c->has_max_bytes)?1:0)|(c->has_min_coverage?2:0))||!c->enabled_mask)return -1;
    sts_observation_encoder encoded;sts_observation_encode_init(&encoded,STS_OBS_METADATA_LIMIT);
    sts_observation_encode_text(&encoded,"stasha-observation-config-v2");
    sts_observation_encode_u32(&encoded,2);sts_observation_encode_u32(&encoded,2);
    sts_observation_encode_text(&encoded,STASHA_OBSERVATION_PROFILE);
    sts_observation_encode_u32(&encoded,1);sts_observation_encode_u32(&encoded,2);
    sts_observation_encode_text(&encoded,STS_OBS_RESOURCE_POLICY);sts_observation_encode_text(&encoded,STS_OBS_LOOKUP_POLICY);
    sts_observation_encode_text(&encoded,STS_OBS_DUPLICATE_POLICY);sts_observation_encode_text(&encoded,STS_OBS_ALIAS_POLICY);
    sts_observation_encode_text(&encoded,STS_OBS_ADMISSION_POLICY);
    const uint32_t limits[]={STS_OBS_FILE_LIMIT,STS_OBS_EDGE_LIMIT,STS_OBS_SOURCE_LIMIT,STS_OBS_SOURCE_LINES,
        STS_OBS_IDENTIFIER_LIMIT,STS_OBS_MODULE_LIMIT,STS_OBS_SYMBOL_LIMIT,STS_OBS_ROOT_PATH_LIMIT,STS_OBS_IMPORT_PATH_LIMIT,
        STS_OBS_PATH_LIMIT,STS_OBS_LABEL_LIMIT,STS_OBS_DECL_LIMIT,STS_OBS_ANCHOR_LIMIT,STASHA_OBSERVATION_MAX_TESTS,STASHA_OBSERVATION_MAX_LINES,
        STASHA_OBSERVATION_MAX_SITES,STASHA_OBSERVATION_MAX_RECORDS,STASHA_OBSERVATION_MAX_REPORT_BYTES,STS_OBS_MANIFEST_LIMIT,STS_OBS_METADATA_LIMIT};
    sts_observation_encode_u32(&encoded,sizeof(limits)/sizeof(limits[0]));
    for(size_t i=0;i<sizeof(limits)/sizeof(limits[0]);++i)sts_observation_encode_u32(&encoded,limits[i]);
    sts_observation_encode_text(&encoded,target);sts_observation_encode_digest(&encoded,c->compiler_id);
    sts_observation_encode_digest(&encoded,runtime_id);sts_observation_encode_u32(&encoded,(uint32_t)optimization);
    sts_observation_encode_u32(&encoded,(uint32_t)debug);sts_observation_encode_u32(&encoded,c->enabled_mask);
    sts_observation_encode_u32(&encoded,c->warn_memleak?1:0);
    sts_observation_encode_u32(&encoded,c->has_max_bytes?1:0);if(c->has_max_bytes)sts_observation_encode_u64(&encoded,c->max_bytes);
    sts_observation_encode_optional_u32(&encoded,c->has_min_coverage,c->min_coverage);
    if(encoded.failed)return -1;
    sts_observation_digest_finish(&encoded.hash,c->config_id);
    return 0;
}
static inline int sts_observation_manifest_digest(const sts_observation_config *c, sts_observation_manifest *m) {
    if(!c||!m||m->file_count>STS_OBS_FILE_LIMIT||m->decl_count>STS_OBS_DECL_LIMIT||
       m->anchor_count>STS_OBS_ANCHOR_LIMIT||m->test_count>STASHA_OBSERVATION_MAX_TESTS||
       m->line_count>STASHA_OBSERVATION_MAX_LINES||m->site_count>STASHA_OBSERVATION_MAX_SITES||
       (m->file_count&&!m->files)||(m->decl_count&&!m->declarations)||(m->anchor_count&&!m->anchors)||
       (m->test_count&&!m->tests)||(m->line_count&&(!m->lines||!m->line_file_ids))||(m->site_count&&!m->sites))return -1;
    uint32_t total_lines=0;
    for(uint32_t i=0;i<m->file_count;++i){
        if(m->files[i].physical_line_count>STS_OBS_SOURCE_LINES-total_lines||!m->files[i].reason_masks)return -1;
        total_lines+=m->files[i].physical_line_count;
    }
    sts_observation_encoder e; sts_observation_encode_init(&e,STS_OBS_MANIFEST_LIMIT);
#define OBS_U(v) sts_observation_encode_u32(&e,(v))
    sts_observation_encode_text(&e,"stasha-observation-map-v2"); OBS_U(2);
    sts_observation_encode_digest(&e,c->input_graph_id); sts_observation_encode_digest(&e,c->config_id);
    OBS_U(m->file_count);
    for(uint32_t i=0;i<m->file_count;++i) {
        const sts_observation_file_manifest *f=&m->files[i];
        OBS_U(f->file_id); OBS_U(f->role); OBS_U(f->physical_line_count); sts_observation_encode_digest(&e,f->content_id);
    }
    OBS_U(m->decl_count);
    for(uint32_t i=0;i<m->decl_count;++i) {
        const sts_observation_declaration *d=&m->declarations[i];
        OBS_U(d->decl_id); OBS_U(d->file_id); OBS_U(d->ordinal_in_file); OBS_U(d->kind);
        OBS_U(d->line); OBS_U(d->col); OBS_U(d->end_line); OBS_U(d->end_col);
        sts_observation_encode_text(&e,d->module_name); sts_observation_encode_text(&e,d->name);
        OBS_U(d->source_linkage); OBS_U(d->selected);
    }
    OBS_U(m->test_count);
    for(uint32_t i=0;i<m->test_count;++i) {
        const sts_observation_test *t=&m->tests[i];
        OBS_U(i); OBS_U(t->decl_id); OBS_U(t->file_id); OBS_U(t->line); OBS_U(t->col); sts_observation_encode_text(&e,t->name);
    }
    OBS_U(m->anchor_count);
    for(uint32_t i=0;i<m->anchor_count;++i) {
        const sts_observation_anchor *a=&m->anchors[i];
        OBS_U(a->anchor_id); OBS_U(a->decl_id); OBS_U(a->file_id); OBS_U(a->line); OBS_U(a->col);
        OBS_U(a->end_line); OBS_U(a->end_col); OBS_U(a->kind); OBS_U(a->exclusion_reason);
        sts_observation_encode_optional_u32(&e,a->has_coverage_id,a->coverage_id);
    }
    OBS_U(m->site_count);
    for(uint32_t i=0;i<m->site_count;++i) {
        const sts_observation_site *s=&m->sites[i];
        OBS_U(i); OBS_U(s->decl_id); OBS_U(s->file_id); OBS_U(s->line); OBS_U(s->col);
        OBS_U(s->end_line); OBS_U(s->end_col); OBS_U(s->kind);
        sts_observation_encode_optional_u32(&e,s->owner_test != UINT32_MAX,s->owner_test);
    }
    OBS_U(m->line_count);
    for(uint32_t i=0;i<m->line_count;++i) {OBS_U(i);OBS_U(m->line_file_ids[i]);OBS_U(m->lines[i]);}
    OBS_U(m->file_count);
    for(uint32_t i=0;i<m->file_count;++i) {
        const sts_observation_file_manifest *f=&m->files[i]; OBS_U(f->file_id); OBS_U(f->physical_line_count);
        for(uint32_t j=1;j<=f->physical_line_count;++j) {OBS_U(j);OBS_U(f->reason_masks[j]);}
    }
#undef OBS_U
    if(e.failed){m->map_id[0]=0;m->serialized_size=0;return -1;}
    m->serialized_size=e.size; sts_observation_digest_finish(&e.hash,m->map_id); return 0;
}

#endif
