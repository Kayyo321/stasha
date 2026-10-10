/* Hosted codegen probe, not an alternate allocator. Compile the actual zone
 * implementation under private names, then count the calls emitted by Stasha.
 * This validates release count/order for the fixture only; it does not qualify
 * allocation arithmetic, alignment, OOM, or the general ownership analysis. */
#define __zone_alloc __cleanup_real_zone_alloc
#define __zone_free __cleanup_real_zone_free
#define __zone_move __cleanup_real_zone_move
#include "../../src/runtime/zone_runtime.c"
#undef __zone_alloc
#undef __zone_free
#undef __zone_move
#include <stdio.h>

static struct { void *state; unsigned id; } live[64];
static unsigned next_id;
static int installed;

static void probe_failure(const char *reason) {
    fprintf(stderr, "cleanup zone probe: %s\n", reason);
    fflush(NULL);
    _Exit(91);
}

static void probe_exit(void) {
    for (unsigned i = 0; i < 64; ++i)
        if (live[i].state) probe_failure("zone not released");
}

void *__zone_alloc(void **zone_ptr, size_t size) {
    if (!installed) {
        if (atexit(probe_exit)) probe_failure("cannot install exit check");
        installed = 1;
    }
    int fresh = zone_ptr && !*zone_ptr;
    void *result = __cleanup_real_zone_alloc(zone_ptr, size);
    if (!result) probe_failure("allocation failed");
    if (fresh) {
        for (unsigned i = 0; i < 64; ++i) {
            if (!live[i].state) {
                live[i].state = *zone_ptr;
                live[i].id = ++next_id;
                return result;
            }
        }
        probe_failure("too many live zones");
    }
    return result;
}

void __zone_free(void **zone_ptr) {
    if (!zone_ptr || !*zone_ptr) probe_failure("duplicate or empty zone release");
    for (unsigned i = 0; i < 64; ++i) {
        if (live[i].state == *zone_ptr) {
            printf("zone.release %u\n", live[i].id);
            live[i].state = NULL;
            __cleanup_real_zone_free(zone_ptr);
            return;
        }
    }
    probe_failure("release of untracked zone");
}

void *__zone_move(void **zone_ptr, void *ptr, size_t size) {
    return __cleanup_real_zone_move(zone_ptr, ptr, size);
}
