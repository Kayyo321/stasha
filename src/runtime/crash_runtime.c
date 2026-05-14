#if defined(__linux__) && !defined(_GNU_SOURCE)
#  define _GNU_SOURCE
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#  define _DARWIN_C_SOURCE 1
#endif

#include "crash_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifndef _WIN32
#include <signal.h>
#include <unistd.h>
#endif

/* ── TLS breadcrumb — defined here, extern-declared in crash_runtime.h ── */
_Thread_local const char    *__stasha_crash_file = 0;
_Thread_local const char    *__stasha_crash_fn   = 0;
_Thread_local uint32_t       __stasha_crash_line  = 0;

/* ── state ── */
static void                 (*__crash_block_fn)(void) = 0;

#ifndef _WIN32

static volatile sig_atomic_t  __crash_active    = 0;
static char                   __crash_alt_stack[8192];

/* ── signal-safe output helpers ── */
static void crash_write(const char *s) {
    if (!s) return;
    size_t n = 0;
    while (s[n]) n++;
    fwrite(s, 1, n, stderr);
    fflush(stderr);
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
    fwrite(buf + i, 1, (size_t)(19 - i), stderr);
    fflush(stderr);
}

static void crash_write_dec(unsigned long v) {
    char buf[22];
    int i = 21;
    buf[i] = '\0';
    if (v == 0) { crash_write("0"); return; }
    while (v) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    fwrite(buf + i, 1, (size_t)(21 - i), stderr);
    fflush(stderr);
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
        fflush(stderr);
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

#else /* _WIN32 */

#include <windows.h>
#include <dbghelp.h>

static volatile LONG __crash_active_w = 0;

static const char *win_exception_name(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:         return "EXCEPTION_ACCESS_VIOLATION (segmentation fault)";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:    return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_DATATYPE_MISALIGNMENT:    return "EXCEPTION_DATATYPE_MISALIGNMENT";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:       return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_FLT_INVALID_OPERATION:    return "EXCEPTION_FLT_INVALID_OPERATION";
        case EXCEPTION_FLT_OVERFLOW:             return "EXCEPTION_FLT_OVERFLOW";
        case EXCEPTION_FLT_STACK_CHECK:          return "EXCEPTION_FLT_STACK_CHECK";
        case EXCEPTION_FLT_UNDERFLOW:            return "EXCEPTION_FLT_UNDERFLOW";
        case EXCEPTION_ILLEGAL_INSTRUCTION:      return "EXCEPTION_ILLEGAL_INSTRUCTION";
        case EXCEPTION_IN_PAGE_ERROR:            return "EXCEPTION_IN_PAGE_ERROR";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:       return "EXCEPTION_INT_DIVIDE_BY_ZERO";
        case EXCEPTION_INT_OVERFLOW:             return "EXCEPTION_INT_OVERFLOW";
        case EXCEPTION_PRIV_INSTRUCTION:         return "EXCEPTION_PRIV_INSTRUCTION";
        case EXCEPTION_STACK_OVERFLOW:           return "EXCEPTION_STACK_OVERFLOW";
        case EXCEPTION_BREAKPOINT:               return "EXCEPTION_BREAKPOINT";
        default:                                 return "unhandled exception";
    }
}

static int win_is_fatal(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        case EXCEPTION_DATATYPE_MISALIGNMENT:
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        case EXCEPTION_FLT_INVALID_OPERATION:
        case EXCEPTION_FLT_OVERFLOW:
        case EXCEPTION_FLT_STACK_CHECK:
        case EXCEPTION_FLT_UNDERFLOW:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_IN_PAGE_ERROR:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_INT_OVERFLOW:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_STACK_OVERFLOW:
            return 1;
        default:
            return 0;
    }
}

static void win_print_stack(void) {
    void *frames[32];
    USHORT n = RtlCaptureStackBackTrace(1, 32, frames, NULL);
    if (n == 0) return;

    HANDLE proc = GetCurrentProcess();
    static int sym_initialised = 0;
    if (!sym_initialised) {
        SymSetOptions(SymGetOptions() | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
        SymInitialize(proc, NULL, TRUE);
        sym_initialised = 1;
    }

    fprintf(stderr, "stack trace:\n");
    char buf[sizeof(SYMBOL_INFO) + 512];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen   = 512;
    for (USHORT i = 0; i < n; i++) {
        DWORD64 addr = (DWORD64)(uintptr_t)frames[i];
        DWORD64 disp = 0;
        const char *name = "?";
        if (SymFromAddr(proc, addr, &disp, sym)) name = sym->Name;

        IMAGEHLP_LINE64 line; line.SizeOfStruct = sizeof(line);
        DWORD line_disp = 0;
        if (SymGetLineFromAddr64(proc, addr, &line_disp, &line))
            fprintf(stderr, "  %2u  %s  (%s:%lu)\n",
                    (unsigned)i, name, line.FileName, (unsigned long)line.LineNumber);
        else
            fprintf(stderr, "  %2u  %s  +0x%llx\n",
                    (unsigned)i, name, (unsigned long long)disp);
    }
    fflush(stderr);
}

static LONG WINAPI __crash_vectored(EXCEPTION_POINTERS *ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (!win_is_fatal(code)) return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedCompareExchange(&__crash_active_w, 1, 0) != 0) {
        TerminateProcess(GetCurrentProcess(), (UINT)(0x80000000 | code));
        return EXCEPTION_CONTINUE_SEARCH;
    }

    fprintf(stderr, "\ncrash: %s", win_exception_name(code));
    if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) {
        if (ep->ExceptionRecord->NumberParameters >= 2)
            fprintf(stderr, " at address 0x%llx",
                    (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1]);
    }
    fputc('\n', stderr);

    if (__stasha_crash_fn) {
        fprintf(stderr, "last call: %s()", __stasha_crash_fn);
        if (__stasha_crash_file)
            fprintf(stderr, " - %s:%u", __stasha_crash_file,
                    (unsigned)__stasha_crash_line);
        fputc('\n', stderr);
    }
    fputc('\n', stderr);

    win_print_stack();

    if (__crash_block_fn) __crash_block_fn();

    /* Run @[[exit]] blocks via exit() → llvm.global_dtors */
    exit((int)(0x80000000 | code));
    return EXCEPTION_EXECUTE_HANDLER; /* unreachable */
}

void __crash_runtime_install(void) {
    AddVectoredExceptionHandler(1, __crash_vectored);
}

#endif /* _WIN32 */

/* ── public API (cross-platform) ── */
void __crash_runtime_register_block(void (*fn)(void)) {
    __crash_block_fn = fn;
}
