#include "outbuf.h"

void out_set_i32(int32_t *out, int32_t v) { *out = v; }

void out_pair(int32_t *a_out, int32_t *b_out, int32_t v) {
    *a_out = v;
    *b_out = -v;
}

void out_fill(int32_t *out, int32_t n, int32_t v) {
    for (int32_t i = 0; i < n; i++) out[i] = v;
}

int32_t out_div(int32_t a, int32_t b, int32_t *q_out, int32_t *r_out) {
    if (b == 0) return -1;
    *q_out = a / b;
    *r_out = a % b;
    return 0;
}
