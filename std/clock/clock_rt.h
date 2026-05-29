#pragma once
#include <stdint.h>

/* Portable monotonic-clock runtime shim. Returns nanoseconds since an
   unspecified-but-monotonic epoch, identical semantics across POSIX,
   macOS, and Windows. Backs the stsstdlib/time/clock module so it does
   not have to deal with clockid_t / LARGE_INTEGER / mach_absolute_time
   directly. */

int64_t sts_clock_now_ns(void);
int64_t sts_clock_ticks_per_sec(void);
