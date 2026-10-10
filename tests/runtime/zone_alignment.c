#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdalign.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "runtime/zone_runtime.h"
int main(void) {
    void *zone = NULL;
    assert(__zone_alloc(NULL, 1) == NULL);
    assert(__zone_alloc(&zone, SIZE_MAX) == NULL && zone == NULL);
    for (size_t size = 0; size < 257; ++size) {
        unsigned char *p = __zone_alloc(&zone, size);
        assert(p && (uintptr_t)p % alignof(max_align_t) == 0);
        memset(p, 0x5a, size ? size : 1);
    }
    unsigned char *large = __zone_alloc(&zone, 65537);
    assert(large && (uintptr_t)large % alignof(max_align_t) == 0);
    memset(large, 0xa5, 65537);
    assert(__zone_alloc(&zone, SIZE_MAX - alignof(max_align_t)) == NULL);
    for (size_t i = 0; i < 65537; ++i) assert(large[i] == 0xa5);
    unsigned char *copy = __zone_move(&zone, large, 65537);
    assert(copy && memcmp(copy, large, 65537) == 0);
    __zone_free(&zone); assert(zone == NULL);
    assert(copy[65536] == 0xa5); free(copy);
    __zone_free(&zone); __zone_free(NULL);
    puts("zone alignment, size overflow, rollover, move and release checks passed");
}
