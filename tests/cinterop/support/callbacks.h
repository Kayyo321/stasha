#ifndef CINTEROP_CALLBACKS_H
#define CINTEROP_CALLBACKS_H

#include <stdint.h>

typedef int32_t (*cb_int_op)(int32_t);

/* Register a Stasha-supplied callback, then invoke it from C. */
void    cb_register(cb_int_op cb);
int32_t cb_invoke(int32_t n);

/* C-side function whose address is exposed to Stasha. */
int32_t cb_doubler(int32_t n);

/* Linear search using a Stasha predicate. */
int32_t cb_find(const int32_t *arr, int32_t n, int32_t (*pred)(int32_t));

#endif
