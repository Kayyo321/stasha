#include "quals.h"

int32_t qual_load_volatile(volatile int32_t *p) { return *p; }

int32_t qual_sum_restrict(int32_t *restrict a, int32_t *restrict b, int32_t n) {
    int32_t s = 0;
    for (int32_t i = 0; i < n; i++) s += a[i] + b[i];
    return s;
}

int32_t qual_sum_underscore(int32_t *__restrict a, int32_t *__restrict__ b, int32_t n) {
    return qual_sum_restrict(a, b, n);
}

int32_t qual_register_arg(register int32_t v) { return v + 1; }

int32_t qual_const_chain(const int32_t * const xs, const int32_t n) {
    int32_t s = 0;
    for (int32_t i = 0; i < n; i++) s += xs[i];
    return s;
}
