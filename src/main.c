#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/common.h"
#include "parser/parser.h"
#include "codegen/codegen.h"
#include "linker/linker.h"

static heap_t source_heap = {0};

static char *read_file(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        log_err("could not open file '%s'", path);
        return Null;
    }

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);

    source_heap = allocate((usize_t)size + 1, sizeof(char));
    char *buf = source_heap.pointer;
    fread(buf, 1, (size_t)size, file);
    buf[size] = '\0';
    fclose(file);

    return buf;
}

int main(int argc, char **argv) {
    if (open_logger() != Ok) return 1;

    if (argc < 2) {
        log_err("usage: stasha <input.sts> [-o <output>]");
        quit(Err);
    }

    const char *input_path = argv[1];
    const char *output_path = "a.out";

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
            output_path = argv[++i];
    }

    char *source = read_file(input_path);
    if (!source) quit(Err);

    log_msg("parsing '%s'", input_path);
    node_t *ast = parse(source);
    if (!ast) {
        log_err("parsing failed");
        quit(Err);
    }

    char obj_path[512];
    snprintf(obj_path, sizeof(obj_path), "%s.o", output_path);

    log_msg("generating code");
    if (codegen(ast, obj_path) != Ok) {
        log_err("code generation failed");
        quit(Err);
    }

    log_msg("linking");
    if (link_object(obj_path, output_path) != Ok) {
        remove(obj_path);
        log_err("linking failed");
        quit(Err);
    }
    remove(obj_path);

    log_msg("compiled '%s' -> '%s'", input_path, output_path);

    ast_free_all();
    deallocate(source_heap);

    quit(Ok);
}
