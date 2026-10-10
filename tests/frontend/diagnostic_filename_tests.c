/* Diagnostic paths must survive caller buffer reuse and nested file switches. */
#define CommonAllowStdlibAllocators
#include "common/common.h"
#include <stdio.h>
#include <string.h>

static void register_temporary(void) {
    char path[] = "imported/module.sts";
    diag_register_source(path, "stack i32 value = 1;\n");
    diag_set_file(path);
    memset(path, 'x', sizeof(path)-1);
}
int main(void) {
    register_temporary();
    if (strcmp(diag_get_file(), "imported/module.sts")) return 1;
    const char *saved = diag_get_file();
    char next[] = "other/module.sts";
    diag_set_file(next);
    memset(next, 'y', sizeof(next)-1);
    if (strcmp(diag_get_file(), "other/module.sts")) return 2;
    diag_register_source("imported/module.sts", "stack i32 value = 2;\n");
    diag_set_file(saved);
    if (strcmp(diag_get_file(), "imported/module.sts")) return 3;
    diag_begin_warning("filename registry update probe");
    diag_span(SRC_LOC(1, 1, 5), True, "updated source");
    diag_finish();
    diag_set_file(NULL);
    if (strcmp(diag_get_file(), "<unknown>")) return 4;
    puts("diagnostic filename ownership: PASS");
    return 0;
}
