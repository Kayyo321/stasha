#ifndef NEG_BITFIELD_H
#define NEG_BITFIELD_H

#include <stdint.h>

/* C bitfield: target-specific layout, sub-byte storage.  Stasha's struct
 * ABI does not model this — surfacing the type as a normal struct would
 * silently break every field that follows.  cheader+main.c must reject
 * the user's header with a precise diagnostic. */

typedef struct {
    int32_t a;
    int32_t flags : 3;
    int32_t kind  : 5;
    int32_t b;
} flag_record_t;

#endif
