#include "cl_args_rt.h"
#include <stddef.h>

static int          g_argc  = 0;
static const char **g_argv  = NULL;

/* GCC/Clang constructor: runs before main(), receives the real argc/argv
   from the C runtime via a __libc_start_main intercept pattern on Linux,
   or via _NSGetArgc/_NSGetArgv on macOS. */
__attribute__((constructor))
static void cl_args_capture(void) {
#if defined(__APPLE__)
    extern int   *_NSGetArgc(void);
    extern char ***_NSGetArgv(void);
    g_argc = *_NSGetArgc();
    g_argv = (const char **)*_NSGetArgv();
#elif defined(__linux__)
    /* On Linux, read /proc/self/cmdline (NUL-separated argument list). */
    /* We re-use g_argc/g_argv as a static pointer into the aux vector
       by intercepting __libc_start_main. Since that is complex, fall
       back to /proc on Linux. This constructor sets up a static buffer. */
    static char   buf[65536];
    static const char *ptrs[512];
    int           fd;
    ssize_t       n;

    /* open /proc/self/cmdline */
    fd = -1;
    {
        /* inline syscall open to avoid pulling in stdio */
        const char path[] = "/proc/self/cmdline";
        __asm__ volatile (
            "syscall"
            : "=a"(fd)
            : "0"(2 /* SYS_open */), "D"(path), "S"(0 /* O_RDONLY */), "d"(0)
            : "rcx", "r11", "memory"
        );
    }
    if (fd < 0) return;
    n = 0;
    {
        __asm__ volatile (
            "syscall"
            : "=a"(n)
            : "0"(0 /* SYS_read */), "D"(fd), "S"(buf), "d"(sizeof(buf) - 1)
            : "rcx", "r11", "memory"
        );
    }
    {
        long rc;
        __asm__ volatile (
            "syscall" : "=a"(rc) : "0"(3 /* SYS_close */), "D"(fd) : "rcx", "r11"
        );
    }
    if (n <= 0) return;
    buf[n] = '\0';
    int argc = 0;
    const char *p = buf;
    const char *end = buf + n;
    while (p < end && argc < 511) {
        ptrs[argc++] = p;
        while (p < end && *p != '\0') p++;
        p++; /* skip NUL */
    }
    ptrs[argc] = NULL;
    g_argc = argc;
    g_argv = ptrs;
#endif
}

int stsha_argc(void) {
    return g_argc;
}

const char *stsha_argv_get(uint32_t idx) {
    if ((int)idx >= g_argc || g_argv == NULL) return NULL;
    return g_argv[idx];
}
