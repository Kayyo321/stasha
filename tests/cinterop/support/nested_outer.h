#ifndef CINTEROP_NESTED_OUTER_H
#define CINTEROP_NESTED_OUTER_H

/* Nested-include test: this header pulls in nested_inner.h via a quoted
 * include.  cheader must transitively process the included file and
 * surface its types/fns alongside the ones declared here, so Stasha can
 * reference `inner_t` without naming nested_inner.h directly. */
#include "nested_inner.h"

int32_t outer_extract(const inner_t *p);
inner_t outer_double(const inner_t *p);

#endif
