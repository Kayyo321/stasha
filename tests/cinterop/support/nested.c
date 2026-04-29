#include "nested_outer.h"

int32_t inner_make(inner_t *out, int32_t v) { out->value = v; return v; }

int32_t outer_extract(const inner_t *p) { return p->value; }

inner_t outer_double(const inner_t *p) {
    inner_t r;
    r.value = p->value * 2;
    return r;
}
