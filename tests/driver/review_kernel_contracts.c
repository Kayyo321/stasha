/* Independent source/target contract regressions. This is not codegen,
 * object, or qualified-kernel evidence. See review_kernel_contracts.sh. */
#define CommonAllowStdlibAllocators
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "parser/parser.h"
#include "analysis/kernel.h"

int main(void) {
    struct { const char *name, *text, *triple; int allowed; } cases[] = {
        {"plain scalar boundary", "mod t; ext fn f(stack u64 a): u64 { ret a; }", "x86_64-unknown-none-elf", 1},
        {"valid panic hook", "mod t; ext fn __stasha_panic(stack u32 reason): void;", "x86_64-unknown-none-elf", 1},
        {"CPU header override", "mod t; @[[target: \"haswell\"]] ext fn f(void): u64 { ret 1; }", "x86_64-unknown-none-elf", 0},
        {"panic argument ABI", "mod t; ext fn __stasha_panic(stack u64 reason): void; ext fn f(void): void { __stasha_panic(1); }", "x86_64-unknown-none-elf", 0},
        {"panic result ABI", "mod t; ext fn __stasha_panic(stack u32 reason): u64; ext fn f(void): u64 { ret __stasha_panic(1); }", "x86_64-unknown-none-elf", 0},
        {"panic renamed import", "mod t; @[[export_name: \"malloc\"]] ext fn __stasha_panic(stack u32 reason): void;", "x86_64-unknown-none-elf", 0},
        {"panic alias definition", "mod t; @[[export_name: \"__stasha_panic\"]] ext fn p(stack u32 reason): void {}", "x86_64-unknown-none-elf", 0},
        {"reserved memcpy definition", "mod t; ext fn memcpy(void): u64 { ret 0; }", "x86_64-unknown-none-elf", 0},
        {"reserved memcpy alias", "mod t; @[[export_name: \"memcpy\"]] ext fn alternate(void): u64 { ret 0; }", "x86_64-unknown-none-elf", 0},
        {"reserved memmove definition", "mod t; ext fn memmove(void): u64 { ret 0; }", "x86_64-unknown-none-elf", 0},
        {"reserved memmove alias", "mod t; @[[export_name: \"memmove\"]] ext fn alternate(void): u64 { ret 0; }", "x86_64-unknown-none-elf", 0},
        {"reserved memset definition", "mod t; ext fn memset(void): u64 { ret 0; }", "x86_64-unknown-none-elf", 0},
        {"reserved memset alias", "mod t; @[[export_name: \"memset\"]] ext fn alternate(void): u64 { ret 0; }", "x86_64-unknown-none-elf", 0},
        {"explicit Mach-O freestanding triple", "mod t; ext fn f(void): u64 { ret 1; }", "x86_64-unknown-none-macho", 0},
        {"explicit Mach-O Linux triple", "mod t; ext fn f(void): u64 { ret 1; }", "x86_64-unknown-linux-macho", 0},
        {"explicit COFF freestanding triple", "mod t; ext fn f(void): u64 { ret 1; }", "x86_64-unknown-none-coff", 0},
    };
    unsigned failures = 0;
    diag_set_render_enabled(False);
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        restore_diagnostic_counts(0, 0);
        diag_clear_captured();
        diag_set_file("review-kernel-contract.sts");
        diag_register_source("review-kernel-contract.sts", cases[i].text);
        pp_stream_t *pp = pp_process(cases[i].text, "review-kernel-contract.sts", NULL, 0);
        node_t *ast = pp ? parse_from_stream(pp) : NULL;
        if (!ast || get_error_count()) {
            fprintf(stderr, "FIXTURE PARSE FAILURE: %s\n", cases[i].name);
            return 2;
        }
        sts_target_spec target;
        int admitted = sts_target_init(&target, cases[i].triple) == 0;
        int allowed = admitted && analyze_kernel_profile(ast, &target) == Ok;
        if (allowed != cases[i].allowed) {
            fprintf(stderr, "FAIL: %s unexpectedly %s\n", cases[i].name, allowed ? "accepted" : "rejected");
            ++failures;
        } else if (admitted && !allowed && !get_error_count()) {
            fprintf(stderr, "FAIL: %s rejected without diagnostic\n", cases[i].name);
            ++failures;
        }
        ast_free_all();
        pp_stream_free(pp);
    }
    assert(scan_and_deallocate() == Ok);
    printf("independent kernel contract review: %u failures in %zu bounded cases\n",
           failures, sizeof(cases) / sizeof(cases[0]));
    return failures ? 1 : 0;
}
