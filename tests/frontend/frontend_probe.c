/* Syntax-only subprocess driver for the bounded deterministic mutation corpus. */
#define CommonAllowStdlibAllocators
#include <stdio.h>
#include <stdlib.h>
#include "parser/parser.h"

int main(int argc, char **argv) {
    if (argc != 2) return 64;
    FILE *file = fopen(argv[1], "rb");
    if (!file) return 66;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return 74; }
    long length = ftell(file);
    if (length < 0 || length > 1024 * 1024) { fclose(file); return 65; }
    rewind(file);
    char *source = calloc((size_t)length + 1, 1);
    if (!source) { fclose(file); return 71; }
    size_t got = fread(source, 1, (size_t)length, file);
    int read_error = ferror(file);
    fclose(file);
    if (read_error || got != (size_t)length) { free(source); return 74; }
    pp_stream_t *stream = pp_process(source, argv[1], NULL, 0);
    if (!stream) { free(source); return 70; }
    node_t *ast = parse_from_stream(stream);
    int result = (!ast || get_error_count()) ? 1 : 0;
    printf("parse_result=%s errors=%lu\n", ast ? "ast" : "null", get_error_count());
    ast_free_all();
    pp_stream_free(stream);
    free(source);
    if (scan_and_deallocate() != Ok) return 71;
    return result;
}
