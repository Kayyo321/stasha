#ifndef CINTEROP_OUTBUF_H
#define CINTEROP_OUTBUF_H

#include <stdint.h>

/* "Output via pointer" idiom from C: caller owns the buffer, callee
 * writes into it.  Tests `T *out` where T is a scalar, plus the
 * common `T *out, int32_t n` shape. */

void out_set_i32(int32_t *out, int32_t v);
void out_pair(int32_t *a_out, int32_t *b_out, int32_t v);
void out_fill(int32_t *out, int32_t n, int32_t v);
int32_t out_div(int32_t a, int32_t b, int32_t *q_out, int32_t *r_out);

#endif
