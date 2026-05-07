#include "outarray.h"

void arr_zero4(int32_t out[4]) {
    out[0] = 0; out[1] = 0; out[2] = 0; out[3] = 0;
}

void arr_iota(int32_t *out, int32_t n) {
    for (int32_t i = 0; i < n; i++) out[i] = i;
}

int32_t arr_max(const int32_t in[], int32_t n, int32_t *idx_out) {
    int32_t best = in[0], idx = 0;
    for (int32_t i = 1; i < n; i++) {
        if (in[i] > best) { best = in[i]; idx = i; }
    }
    *idx_out = idx;
    return best;
}

void arr_running_sum(const int32_t *in, int32_t n, int32_t sum_out[]) {
    int32_t s = 0;
    for (int32_t i = 0; i < n; i++) {
        s += in[i];
        sum_out[i] = s;
    }
}
