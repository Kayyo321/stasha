#ifndef CINTEROP_QUALS_H
#define CINTEROP_QUALS_H

#include <stdint.h>

/* Type qualifiers that are syntactically meaningful in C but irrelevant
 * to Stasha's ABI lowering: cheader must accept and discard them rather
 * than choke or treat them as identifiers.
 *   - `volatile`   on an argument
 *   - `restrict` / `__restrict` / `__restrict__` on pointer params
 *   - `register`   on a parameter (legacy)
 *   - `const` at multiple positions
 */

int32_t qual_load_volatile(volatile int32_t *p);
int32_t qual_sum_restrict(int32_t *restrict a, int32_t *restrict b, int32_t n);
int32_t qual_sum_underscore(int32_t *__restrict a, int32_t *__restrict__ b, int32_t n);
int32_t qual_register_arg(register int32_t v);
int32_t qual_const_chain(const int32_t * const xs, const int32_t n);

#endif
