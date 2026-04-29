#ifndef CINTEROP_BOOLS_H
#define CINTEROP_BOOLS_H

#include <stdint.h>

/* `_Bool` is the C99 keyword.  `bool` from <stdbool.h> is a macro that
 * expands to `_Bool` — we deliberately use `_Bool` directly here so the
 * cheader scanner doesn't need a real C preprocessor to handle it. */

_Bool   bool_not(_Bool b);
_Bool   bool_and(_Bool a, _Bool b);
int32_t bool_to_int(_Bool b);
_Bool   bool_from_int(int32_t v);

#endif
