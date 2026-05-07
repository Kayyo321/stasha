#ifndef CINTEROP_PTRS_H
#define CINTEROP_PTRS_H

#include <stdint.h>

void *ptr_id(void *p);
int32_t arr_sum_const(const int32_t *xs, int32_t n);
void arr_double(int32_t *xs, int32_t n);

#endif
