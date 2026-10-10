#define CommonAllowStdlibAllocators
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "parser/parser.h"
#include "analysis/kernel.h"

int main(void) {
    struct { const char *name, *text; int allowed; } cases[] = {
        {"scalar function", "mod t; ext fn add(stack u64 a, stack u64 b): u64 { ret a + b; }", 1},
        {"initialized loop", "mod t; ext fn count(stack u32 n): u32 { stack u32 i = 0; while (i < n) { i++; } ret i; }", 1},
        {"explicit raw access", "mod t; ext fn read(stack u64 *rw p): u64 { unsafe { ret p[0]; } }", 1},
        {"uninitialized local", "mod t; fn f(void): u64 { stack u64 a; ret a; }", 0},
        {"heap storage", "mod t; fn f(void): void { heap u64 a = 1; }", 0},
        {"float declaration", "mod t; stack f64 x = 1.0;", 0},
        {"aliased float pointer", "mod t; type F: f64; fn f(stack F *rw p): void {}", 0},
        {"hosted print", "mod t; fn f(void): void { print.('hi'); }", 0},
        {"implicit hosted call", "mod t; fn f(void): void { printf(\"hi\"); }", 0},
        {"explicit hosted import", "mod t; ext fn malloc(stack u64 n): void *rw;", 0},
        {"raw access boundary", "mod t; fn f(stack u64 *rw p): u64 { ret p[0]; }", 0},
        {"aggregate ABI", "mod t; type S: struct { u64 a; }; ext fn f(stack S a): u64 { ret 0; }", 0},
        {"hosted import", "mod t; libimp \"memory\" from std;", 0},
        {"panic body", "mod t; ext fn __stasha_panic(stack u32 reason): void {}", 0},
        {"panic wrong ABI", "mod t; ext fn __stasha_panic(stack u64 reason): void;", 0},
        {"CPU override", "mod t; @[[target: \"haswell\"]] ext fn f(void): void {}", 0},
        {"reserved export", "mod t; @[[export_name: \"__stasha_panic\"]] ext fn f(stack u32 reason): void {}", 0},
        {"panic declaration", "mod t; ext fn __stasha_panic(stack u32 reason): void;", 1},
    };
    sts_target_spec target;
    assert(sts_target_init(&target, "x86_64-unknown-none-elf") == 0);
    diag_set_render_enabled(False);
    for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {
        restore_diagnostic_counts(0, 0);
        diag_clear_captured();
        diag_set_file("kernel-profile-test.sts");
        diag_register_source("kernel-profile-test.sts", cases[i].text);
        pp_stream_t *pp = pp_process(cases[i].text, "kernel-profile-test.sts", NULL, 0);
        node_t *ast = pp ? parse_from_stream(pp) : NULL;
        if (!ast || get_error_count()) {
            fprintf(stderr, "fixture parse failure: %s\n", cases[i].name);
            return 1;
        }
        result_t result = analyze_kernel_profile(ast, &target);
        if ((result == Ok) != cases[i].allowed) {
            fprintf(stderr, "profile mismatch: %s\n", cases[i].name);
            return 1;
        }
        if (!cases[i].allowed) assert(get_error_count() > 0);
        ast_free_all(); pp_stream_free(pp);
    }
    assert(scan_and_deallocate() == Ok);
    puts("kernel capability validator: 18 source cases passed (not codegen qualification)");
}
