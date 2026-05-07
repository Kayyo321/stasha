/* ── Stasha Crash Protection Runtime ────────────────────────────────────────
 *
 * Installed automatically at program startup via @llvm.global_ctors.
 * Catches fatal OS signals and emits a descriptive crash report before
 * running @[[exit]] blocks and @[[crash]] user-defined cleanup blocks.
 *
 * API used by compiler-generated code:
 *
 *   __crash_runtime_install()
 *       Install signal handlers (SIGSEGV/SIGABRT/SIGFPE/SIGILL/SIGBUS)
 *       and set up a sigaltstack for stack-overflow detection.
 *
 *   __crash_runtime_register_block(fn)
 *       Register the @[[crash]] block dispatcher (NULL if none).
 *       Called once from the generated crash-init constructor.
 *
 * TLS breadcrumb (written by generated code before each C lib call):
 *
 *   __stasha_crash_file   — source file name (or NULL)
 *   __stasha_crash_fn     — C function name being called (or NULL)
 *   __stasha_crash_line   — source line number
 */

#pragma once
#include <stdint.h>

void __crash_runtime_install(void);
void __crash_runtime_register_block(void (*fn)(void));

extern _Thread_local const char    *__stasha_crash_file;
extern _Thread_local const char    *__stasha_crash_fn;
extern _Thread_local uint32_t       __stasha_crash_line;
