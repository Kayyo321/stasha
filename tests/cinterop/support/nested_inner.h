#ifndef CINTEROP_NESTED_INNER_H
#define CINTEROP_NESTED_INNER_H

#include <stdint.h>

typedef struct {
    int32_t value;
} inner_t;

int32_t inner_make(inner_t *out, int32_t v);

#endif
