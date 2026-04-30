#ifndef CINTEROP_OUTARRAY_H
#define CINTEROP_OUTARRAY_H

#include <stdint.h>

/* Array-shaped output parameters.  In C `T out[N]` decays to `T *` for
 * a function parameter; cheader treats both spellings identically.
 * Includes a sized-array decl plus an unsized-array decl. */

void arr_zero4(int32_t out[4]);
void arr_iota(int32_t *out, int32_t n);
int32_t arr_max(const int32_t in[], int32_t n, int32_t *idx_out);

/* Two-output: writes the running sum into `sum_out[i]`. */
void arr_running_sum(const int32_t *in, int32_t n, int32_t sum_out[]);

#endif
