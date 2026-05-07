#include "crash_runtime.h"
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ── TLS breadcrumb — defined here, extern-declared in crash_runtime.h ── */
_Thread_local const char    *__stasha_crash_file = 0;
_Thread_local const char    *__stasha_crash_fn   = 0;
_Thread_local uint32_t       __stasha_crash_line  = 0;

/* ── state ── */
static volatile sig_atomic_t  __crash_active    = 0;
static void                 (*__crash_block_fn)(void) = 0;
static char                   __crash_alt_stack[8192];

/* ── signal-safe output helpers ── */
static void crash_write(const char *s) {
    if (!s) return;
    size_t n = 0;
    while (s[n]) n++;
    write(STDERR_FILENO, s, n);
}

static void crash_write_hex(unsigned long v) {
    char buf[20];
    int i = 19;
    buf[i] = '\0';
    do {
        int d = (int)(v & 0xf);
        buf[--i] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        v >>= 4;
    } while (v && i > 2);
    buf[--i] = 'x';
    buf[--i] = '0';
    write(STDERR_FILENO, buf + i, (size_t)(19 - i));
}

static void crash_write_dec(unsigned long v) {
    char buf[22];
    int i = 21;
    buf[i] = '\0';
    if (v == 0) { crash_write("0"); return; }
    while (v) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    write(STDERR_FILENO, buf + i, (size_t)(21 - i));
}

/* ── crash handler ── */
static void __crash_handler(int sig, siginfo_t *info, void *ctx) {
    (void)ctx;
    if (__crash_active) { _Exit(128 + sig); }
    __crash_active = 1;

    crash_write("\ncrash: ");
    switch (sig) {
        case SIGSEGV: crash_write("SIGSEGV (segmentation fault)"); break;
        case SIGABRT: crash_write("SIGABRT (abort)");              break;
        case SIGFPE:  crash_write("SIGFPE (floating point exception)"); break;
        case SIGILL:  crash_write("SIGILL (illegal instruction)");  break;
        case SIGBUS:  crash_write("SIGBUS (bus error)");            break;
        default:
            crash_write("signal ");
            crash_write_dec((unsigned long)sig);
            break;
    }

    if (info && (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL || sig == SIGFPE)) {
        crash_write(" at address ");
        crash_write_hex((unsigned long)(uintptr_t)info->si_addr);
    }
    crash_write("\n");

    if (__stasha_crash_fn) {
        crash_write("last call: ");
        crash_write(__stasha_crash_fn);
        crash_write("()");
        if (__stasha_crash_file) {
            crash_write(" — ");
            crash_write(__stasha_crash_file);
            crash_write(":");
            crash_write_dec((unsigned long)__stasha_crash_line);
        }
        crash_write("\n");
    }
    crash_write("\n");

#if defined(__APPLE__) || defined(__linux__)
    void *bt[32];
    extern int backtrace(void **, int);
    extern void backtrace_symbols_fd(void *const *, int, int);
    int n = backtrace(bt, 32);
    if (n > 0) {
        crash_write("stack trace:\n");
        backtrace_symbols_fd(bt, n, STDERR_FILENO);
        crash_write("\n");
    }
#endif

    if (__crash_block_fn) __crash_block_fn();

    /* Run @[[exit]] blocks via exit() → @llvm.global_dtors */
    exit(128 + sig);

    /* Restore default and re-raise for correct exit code (unreachable if exit runs) */
    signal(sig, SIG_DFL);
    raise(sig);
}

/* ── public API ── */
void __crash_runtime_register_block(void (*fn)(void)) {
    __crash_block_fn = fn;
}

void __crash_runtime_install(void) {
    stack_t ss;
    ss.ss_sp    = __crash_alt_stack;
    ss.ss_size  = sizeof(__crash_alt_stack);
    ss.ss_flags = 0;
    sigaltstack(&ss, 0);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = __crash_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;

    sigaction(SIGSEGV, &sa, 0);
    sigaction(SIGABRT, &sa, 0);
    sigaction(SIGFPE,  &sa, 0);
    sigaction(SIGILL,  &sa, 0);
    sigaction(SIGBUS,  &sa, 0);
}
