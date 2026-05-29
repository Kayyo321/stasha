#include "clock_rt.h"

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

int64_t sts_clock_now_ns(void) {
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    /* Compute ns = c * 1e9 / f without overflow: split into seconds + fractional. */
    int64_t sec  = c.QuadPart / f.QuadPart;
    int64_t frac = c.QuadPart % f.QuadPart;
    return sec * 1000000000LL + (frac * 1000000000LL) / f.QuadPart;
}

int64_t sts_clock_ticks_per_sec(void) {
    return 1000000000LL;
}

#elif defined(__APPLE__)

#include <mach/mach_time.h>

int64_t sts_clock_now_ns(void) {
    static mach_timebase_info_data_t tb = {0, 0};
    if (tb.denom == 0) {
        mach_timebase_info(&tb);
    }
    return (int64_t)((mach_absolute_time() * (uint64_t)tb.numer) / (uint64_t)tb.denom);
}

int64_t sts_clock_ticks_per_sec(void) {
    return 1000000000LL;
}

#else

#include <time.h>

int64_t sts_clock_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

int64_t sts_clock_ticks_per_sec(void) {
    return 1000000000LL;
}

#endif
