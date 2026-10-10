/* Reviewed, test-only freestanding memory ABI shim, shared by host and boot.
 * GCC also gets -fno-tree-loop-distribute-patterns: do not lower these loops
 * into recursive libcalls. The runner audits leaf code and zero imports.
 * Counters prove that the Stasha lowering really executes each helper. */
#include "component.h"
uint64_t k0_memcpy_calls, k0_memmove_calls, k0_memset_calls;
#ifndef K0_OMIT_MEMSET
void *memset(void *dst, int value, size_t n) {
    ++k0_memset_calls;
    uint8_t *p = dst;
    for (size_t i = 0; i < n; ++i) p[i] = (uint8_t)value;
    return dst;
}
#endif
#ifndef K0_OMIT_MEMCPY
void *memcpy(void *dst, const void *src, size_t n) {
    ++k0_memcpy_calls;
    uint8_t *d = dst;
    const uint8_t *s = src;
    for (size_t i = 0; i < n; ++i) d[i] = s[i];
    return dst;
}
#endif
#ifndef K0_OMIT_MEMMOVE
void *memmove(void *dst, const void *src, size_t n) {
    ++k0_memmove_calls;
    uint8_t *d = dst;
    const uint8_t *s = src;
    if ((uintptr_t)d <= (uintptr_t)s) {
        for (size_t i = 0; i < n; ++i) d[i] = s[i];
    } else {
        while (n) { --n; d[n] = s[n]; }
    }
    return dst;
}
#endif
