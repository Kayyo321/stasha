/* Isolate the production linker/audit implementation from Stasha codegen. */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
extern "C" {
#include "linker/linker.h"
void log_err(const char *fmt, ...) {
    va_list args; va_start(args, fmt); vfprintf(stderr, fmt, args); va_end(args);
    fputc('\n', stderr);
}
}
int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "audit")) return audit_kernel_object(argv[2]) == Ok ? 0 : 1;
    if (argc == 5 && !strcmp(argv[1], "link")) return link_object(argv[2], argv[3], nullptr, argv[4]) == Ok ? 0 : 1;
    return 2;
}
