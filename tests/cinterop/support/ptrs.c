#include "ptrs.h"

void *ptr_id(void *p) { return p; }

int32_t arr_sum_const(const int32_t *xs, int32_t n) {
    int32_t sum = 0;
    for (int32_t i = 0; i < n; i++) sum += xs[i];
    return sum;
}

void arr_double(int32_t *xs, int32_t n) {
    for (int32_t i = 0; i < n; i++) xs[i] *= 2;
}
