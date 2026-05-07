#include "bools.h"

_Bool   bool_not(_Bool b)            { return !b; }
_Bool   bool_and(_Bool a, _Bool b)   { return a && b; }
int32_t bool_to_int(_Bool b)         { return b ? 1 : 0; }
_Bool   bool_from_int(int32_t v)     { return v != 0; }
