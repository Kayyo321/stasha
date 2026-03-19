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

/* ── module import resolution ── */

/* source buffers for imported modules — freed after all parsing is done */
static heap_t  *imp_heaps     = NULL;
static usize_t  imp_heap_cnt  = 0;
static usize_t  imp_heap_cap  = 0;
static heap_t   imp_list_heap = {NULL, 0, NULL};

static char *read_imp_source(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return Null;

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);

    heap_t h = allocate((usize_t)size + 1, sizeof(char));
    char *buf = h.pointer;
    fread(buf, 1, (size_t)size, file);
    buf[size] = '\0';
    fclose(file);

    /* grow the tracking list */
    if (imp_heap_cnt >= imp_heap_cap) {
        usize_t new_cap = imp_heap_cap < 8 ? 8 : imp_heap_cap * 2;
        if (imp_list_heap.pointer == Null)
            imp_list_heap = allocate(new_cap, sizeof(heap_t));
        else
            imp_list_heap = reallocate(imp_list_heap, new_cap * sizeof(heap_t));
        imp_heaps    = imp_list_heap.pointer;
        imp_heap_cap = new_cap;
    }
    imp_heaps[imp_heap_cnt++] = h;
    return buf;
}

static void free_imp_sources(void) {
    for (usize_t i = 0; i < imp_heap_cnt; i++)
        deallocate(imp_heaps[i]);
    if (imp_list_heap.pointer != Null)
        deallocate(imp_list_heap);
    imp_heaps    = NULL;
    imp_heap_cnt = 0;
    imp_heap_cap = 0;
    imp_list_heap.pointer = NULL;
    imp_list_heap.size    = 0;
    imp_list_heap.priv    = NULL;
}

/*
 * resolve_imports — walk every NodeImpDecl in `ast`, find the corresponding
 * "<dir>/<modname>.sts" file, parse it, and splice all of its declarations
 * (types, functions, variables, lib declarations) into `ast->as.module.decls`.
 *
 * All declarations from the imported module are merged, including `int`
 * (private) helpers, because `ext` functions may call them internally.
 * The module boundary is enforced by the fact that the importing source
 * never names those symbols — if it tries to, the programmer made an error.
 */
static void resolve_imports(node_t *ast, const char *input_path) {
    /* derive the directory of the primary file */
    char dir[512];
    strncpy(dir, input_path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    char *sep = strrchr(dir, '/');
    if (!sep) sep = strrchr(dir, '\\');
    if (sep) {
        *sep = '\0';
    } else {
        dir[0] = '.'; dir[1] = '\0';
    }

    /*
     * Iterate with an index (not a snapshot count) so that transitively
     * imported modules are also resolved if they contain their own `imp`
     * statements.
     */
    for (usize_t i = 0; i < ast->as.module.decls.count; i++) {
        node_t *decl = ast->as.module.decls.items[i];
        if (decl->kind != NodeImpDecl) continue;

        const char *mod_name = decl->as.imp_decl.module_name;
        char mod_path[1024];
        snprintf(mod_path, sizeof(mod_path), "%s/%s.sts", dir, mod_name);

        log_msg("importing '%s'", mod_path);

        char *src = read_imp_source(mod_path);
        if (!src) {
            log_err("line %lu: cannot find module '%s' (looked for '%s')",
                    decl->line, mod_name, mod_path);
            continue;
        }

        node_t *mod_ast = parse(src);
        if (!mod_ast) {
            log_err("line %lu: failed to parse module '%s'", decl->line, mod_name);
            continue;
        }

        /* splice every declaration from the imported module */
        for (usize_t j = 0; j < mod_ast->as.module.decls.count; j++) {
            node_t *d = mod_ast->as.module.decls.items[j];
            if (d->kind == NodeImpDecl) continue;  /* handled by outer loop */
            node_list_push(&ast->as.module.decls, d);
        }
    }
}

int main(int argc, char **argv) {
    if (open_logger() != Ok) return 1;

    if (argc < 2) {
        log_err("usage: stasha <input.sts> [-o <output>]");
        log_err("       stasha test <input.sts> [-o <output>]");
        quit(Err);
    }

    /* check for 'test' subcommand */
    boolean_t test_mode = False;
    int file_arg = 1;
    if (strcmp(argv[1], "test") == 0) {
        test_mode = True;
        file_arg = 2;
        if (argc < 3) {
            log_err("usage: stasha test <input.sts> [-o <output>]");
            quit(Err);
        }
    }

    const char *input_path = argv[file_arg];
    const char *output_path = test_mode ? "a.test" : "a.out";

    for (int i = file_arg + 1; i < argc; i++) {
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

    resolve_imports(ast, input_path);
    free_imp_sources();

    char obj_path[512];
    snprintf(obj_path, sizeof(obj_path), "%s.o", output_path);

    log_msg("generating code%s", test_mode ? " (test mode)" : "");
    if (codegen(ast, obj_path, test_mode) != Ok) {
        log_err("code generation failed");
        quit(Err);
    }

    /* collect custom library paths from `lib "name" from "path"` declarations */
    const char *extra_lib_buf[64];
    usize_t extra_lib_count = 0;
    for (usize_t i = 0; i < ast->as.module.decls.count; i++) {
        node_t *d = ast->as.module.decls.items[i];
        if (d->kind == NodeLib && d->as.lib_decl.path
                && extra_lib_count < 63)
            extra_lib_buf[extra_lib_count++] = d->as.lib_decl.path;
    }
    extra_lib_buf[extra_lib_count] = Null; /* NULL-terminate */

    log_msg("linking");
    if (link_object(obj_path, output_path,
                    extra_lib_count > 0 ? extra_lib_buf : Null) != Ok) {
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
