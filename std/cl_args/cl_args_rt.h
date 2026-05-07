#pragma once
#include <stdint.h>

/* Thin runtime shim: captures argc/argv before Stasha main() runs.
   stsha_argc() / stsha_argv_get(n) give Stasha code access to them. */

int          stsha_argc(void);
const char  *stsha_argv_get(uint32_t idx);
