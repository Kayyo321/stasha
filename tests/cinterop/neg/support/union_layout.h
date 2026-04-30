#ifndef NEG_UNION_LAYOUT_H
#define NEG_UNION_LAYOUT_H

#include <stdint.h>

/* C union: overlapping storage with widest-member size and target-
 * specific alignment.  Stasha does not model union layout, so
 * cheader+main.c must reject the user's header with a "union"-bearing
 * diagnostic rather than silently treating it as a struct. */

typedef union {
    int32_t i;
    float   f;
} value_t;

#endif
