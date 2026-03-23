#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__APPLE__) || defined(__linux__)
#  include <unistd.h>
#  include <sys/wait.h>
#endif
#if defined(__APPLE__)
/* Forward-declare to avoid pulling in mach-o/dyld.h (conflicts with boolean_t). */
extern int _NSGetExecutablePath(char *buf, unsigned int *bufsize);
#endif

#include "common/common.h"
#include "parser/parser.h"
#include "codegen/codegen.h"
#include "linker/linker.h"

#define STASHA_VERSION "0.1.0"

/* ── binary directory (for stdlib resolution) ── */

static char bin_dir[512] = {0};

/*
 * Populate bin_dir with the directory containing the stasha binary.
 * Used to locate bin/stdlib/ for `libimp "name" from std;`.
 */
static void init_bin_dir(const char *argv0) {
#if defined(__APPLE__)
    char exe[512];
    uint32_t size = (uint32_t)sizeof(exe);
    if (_NSGetExecutablePath(exe, &size) == 0) {
        char *sep = strrchr(exe, '/');
        if (sep) { *sep = '\0'; strncpy(bin_dir, exe, sizeof(bin_dir) - 1); return; }
    }
#elif defined(__linux__)
    char exe[512];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = '\0';
        char *sep = strrchr(exe, '/');
        if (sep) { *sep = '\0'; strncpy(bin_dir, exe, sizeof(bin_dir) - 1); return; }
    }
#endif
    /* fallback: derive from argv0 */
    strncpy(bin_dir, argv0, sizeof(bin_dir) - 1);
    bin_dir[sizeof(bin_dir) - 1] = '\0';
    char *sep = strrchr(bin_dir, '/');
    if (!sep) sep = strrchr(bin_dir, '\\');
    if (sep) *sep = '\0';
    else { bin_dir[0] = '.'; bin_dir[1] = '\0'; }
}

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
 * is_lib_backed — check whether `mod_name` matches any `lib "name" from "path"`
 * declaration in the primary AST.  If so, the module's compiled code already
 * lives in the .a and we should only import type/signature information.
 */
static boolean_t is_lib_backed(node_t *ast, const char *mod_name) {
    for (usize_t i = 0; i < ast->as.module.decls.count; i++) {
        node_t *d = ast->as.module.decls.items[i];
        if (d->kind == NodeLib && d->as.lib_decl.path != Null
            && strcmp(d->as.lib_decl.name, mod_name) == 0)
            return True;
        if (d->kind == NodeLibImp
            && strcmp(d->as.libimp_decl.name, mod_name) == 0)
            return True;
    }
    return False;
}

/*
 * find_lib_archive_path — return the `from "path"` value for a library-backed
 * module, or NULL if not found.
 */
/* Resolved stdlib archive paths are stored here so the returned pointer stays valid. */
static char stdlib_arc_path_buf[64][512];
static usize_t stdlib_arc_path_cnt = 0;

static const char *find_lib_archive_path(node_t *ast, const char *mod_name) {
    for (usize_t i = 0; i < ast->as.module.decls.count; i++) {
        node_t *d = ast->as.module.decls.items[i];
        if (d->kind == NodeLib && d->as.lib_decl.path != Null
            && strcmp(d->as.lib_decl.name, mod_name) == 0)
            return d->as.lib_decl.path;
        if (d->kind == NodeLibImp
            && strcmp(d->as.libimp_decl.name, mod_name) == 0) {
            if (d->as.libimp_decl.from_std) {
                if (stdlib_arc_path_cnt >= 64) return NULL;
                char *buf = stdlib_arc_path_buf[stdlib_arc_path_cnt++];
                snprintf(buf, 512, "%s/stdlib/lib%s.a", bin_dir, mod_name);
                return buf;
            }
            return d->as.libimp_decl.path;
        }
    }
    return NULL;
}

/*
 * mod_name_to_rel_path — convert a dotted module name to a relative filesystem
 * path by replacing '.' separators with '/'.
 * e.g. "printer.typewriter" -> "printer/typewriter"
 */
static void mod_name_to_rel_path(const char *mod_name, char *out, usize_t out_size) {
    strncpy(out, mod_name, out_size - 1);
    out[out_size - 1] = '\0';
    for (char *c = out; *c; c++)
        if (*c == '.') *c = '/';
}

/*
 * resolve_imports — walk every NodeImpDecl in `ast`, find the corresponding
 * "<dir>/<mod/path>.sts" file, parse it, and splice all of its declarations
 * (types, functions, variables, lib declarations) into `ast->as.module.decls`.
 *
 * Module names are dot-separated paths mirroring the directory structure:
 * "printer.typewriter" resolves to "<dir>/printer/typewriter.sts".
 *
 * When a module is also declared via `lib "name" from "path.a"`, the module
 * is "library-backed": its compiled code already exists in the .a archive.
 * In that case we still splice type declarations (structs, enums, aliases)
 * so the compiler knows about layouts and signatures, but we tag every
 * spliced node with `from_lib = True` so that codegen skips body/initializer
 * generation and lets the linker resolve symbols from the archive instead.
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
        if (decl->kind != NodeImpDecl && decl->kind != NodeLibImp) continue;

        const char *mod_name = (decl->kind == NodeLibImp)
            ? decl->as.libimp_decl.name
            : decl->as.imp_decl.module_name;
        char mod_rel[512];
        mod_name_to_rel_path(mod_name, mod_rel, sizeof(mod_rel));
        char mod_path[1024];
        snprintf(mod_path, sizeof(mod_path), "%s/%s.sts", dir, mod_rel);

        boolean_t lib_backed = is_lib_backed(ast, mod_name);

        log_msg("importing '%s'%s", mod_path,
                lib_backed ? " (library-backed)" : "");

        char *src = read_imp_source(mod_path);
        if (!src && lib_backed) {
            /* Fallback 1 (stdlib): check <bin_dir>/stdlib/<name>.sts directly. */
            if (decl->kind == NodeLibImp && decl->as.libimp_decl.from_std) {
                char stdlib_src[1024];
                snprintf(stdlib_src, sizeof(stdlib_src),
                         "%s/stdlib/%s.sts", bin_dir, mod_name);
                log_msg("importing '%s' (stdlib, fallback)", stdlib_src);
                src = read_imp_source(stdlib_src);
                if (src)
                    snprintf(mod_path, sizeof(mod_path), "%s", stdlib_src);
            }
        }
        if (!src && lib_backed) {
            /* Fallback 2: look for the .sts alongside the .a archive.
             * If the archive path is absolute, use it directly; otherwise
             * resolve it relative to dir first. */
            const char *arc = find_lib_archive_path(ast, mod_name);
            if (arc) {
                char arc_full[1024];
                if (arc[0] == '/' || (arc[0] && arc[1] == ':'))
                    strncpy(arc_full, arc, sizeof(arc_full) - 1);
                else
                    snprintf(arc_full, sizeof(arc_full), "%s/%s", dir, arc);
                arc_full[sizeof(arc_full) - 1] = '\0';
                char *arc_sep = strrchr(arc_full, '/');
                if (!arc_sep) arc_sep = strrchr(arc_full, '\\');
                if (arc_sep) {
                    *arc_sep = '\0';
                    char fallback_path[1024];
                    snprintf(fallback_path, sizeof(fallback_path),
                             "%s/%s.sts", arc_full, mod_name);
                    log_msg("importing '%s' (library-backed, fallback)", fallback_path);
                    src = read_imp_source(fallback_path);
                    if (src)
                        snprintf(mod_path, sizeof(mod_path), "%s", fallback_path);
                }
            }
        }
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

        /* splice declarations from the imported module */
        for (usize_t j = 0; j < mod_ast->as.module.decls.count; j++) {
            node_t *d = mod_ast->as.module.decls.items[j];
            if (d->kind == NodeImpDecl) continue;  /* handled by outer loop */

            if (lib_backed)
                d->from_lib = True;

            node_list_push(&ast->as.module.decls, d);
        }
    }
}

/* ── sts.sproj project file ── */

#define SPROJ_MAX_LIBS 64

typedef struct {
    char arc_path[512]; /* path to .a archive */
    char sts_path[512]; /* path to .sts interface file (may be empty) */
} sproj_lib_t;

typedef struct {
    char        main_path[512];
    char        output_path[512];
    boolean_t   is_library;
    boolean_t   has_output;
    sproj_lib_t ext_libs[SPROJ_MAX_LIBS];
    usize_t     ext_lib_count;
} sproj_t;

/*
 * Parse a sts.sproj file at `path` into `out`.
 * Format:
 *   main     = "src/main.sts"
 *   binary   = "bin/name"          (or library = "bin/name.a")
 *   ext_libs = []
 *   ext_libs = [ ("libs/x.a" : "libs/x.sts"), ... ]
 *
 * Lines starting with # are comments.
 * Returns True if parsing succeeded and a main path was found.
 */
static boolean_t parse_sproj(const char *path, sproj_t *out) {
    FILE *f = fopen(path, "r");
    if (!f) return False;

    memset(out, 0, sizeof(*out));

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        /* strip # comments */
        char *comment = strchr(line, '#');
        if (comment) *comment = '\0';

        /* trim leading whitespace */
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '\0' || *s == '\n' || *s == '\r') continue;

        /* find '=' separator */
        char *eq = strchr(s, '=');
        if (!eq) continue;

        /* extract key */
        char *kend = eq - 1;
        while (kend >= s && (*kend == ' ' || *kend == '\t')) kend--;
        if (kend < s) continue;
        usize_t klen = (usize_t)(kend - s + 1);
        char key[64];
        if (klen >= sizeof(key)) continue;
        memcpy(key, s, klen);
        key[klen] = '\0';

        /* skip whitespace after '=' */
        char *v = eq + 1;
        while (*v == ' ' || *v == '\t') v++;

        /* string value: "..." or '...' */
        if (*v == '"' || *v == '\'') {
            char q = *v++;
            char *vend = strchr(v, q);
            if (!vend) continue;
            usize_t vlen = (usize_t)(vend - v);
            if (strcmp(key, "main") == 0 && vlen < sizeof(out->main_path)) {
                memcpy(out->main_path, v, vlen);
                out->main_path[vlen] = '\0';
            } else if (strcmp(key, "binary") == 0 && vlen < sizeof(out->output_path)) {
                memcpy(out->output_path, v, vlen);
                out->output_path[vlen] = '\0';
                out->has_output = True;
                out->is_library = False;
            } else if (strcmp(key, "library") == 0 && vlen < sizeof(out->output_path)) {
                memcpy(out->output_path, v, vlen);
                out->output_path[vlen] = '\0';
                out->has_output = True;
                out->is_library = True;
            }
            continue;
        }

        /* array value: [ ... ] — only parse for ext_libs */
        if (*v != '[' || strcmp(key, "ext_libs") != 0) continue;
        v++; /* skip '[' */

        /* parse comma-separated ("arc" : "sts") tuples */
        while (out->ext_lib_count < SPROJ_MAX_LIBS) {
            while (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r') v++;
            if (*v == ']' || *v == '\0') break;
            if (*v != '(') { v++; continue; }
            v++; /* skip '(' */

            /* first string: archive path */
            while (*v == ' ' || *v == '\t') v++;
            if (*v != '"' && *v != '\'') { v++; continue; }
            char q1 = *v++;
            char *a1end = strchr(v, q1);
            if (!a1end) break;
            usize_t a1len = (usize_t)(a1end - v);
            if (a1len < sizeof(out->ext_libs[out->ext_lib_count].arc_path)) {
                memcpy(out->ext_libs[out->ext_lib_count].arc_path, v, a1len);
                out->ext_libs[out->ext_lib_count].arc_path[a1len] = '\0';
            }
            v = a1end + 1;

            /* ':' separator */
            while (*v == ' ' || *v == '\t') v++;
            if (*v == ':') v++;

            /* second string: .sts interface path */
            while (*v == ' ' || *v == '\t') v++;
            if (*v == '"' || *v == '\'') {
                char q2 = *v++;
                char *a2end = strchr(v, q2);
                if (a2end) {
                    usize_t a2len = (usize_t)(a2end - v);
                    if (a2len < sizeof(out->ext_libs[out->ext_lib_count].sts_path)) {
                        memcpy(out->ext_libs[out->ext_lib_count].sts_path, v, a2len);
                        out->ext_libs[out->ext_lib_count].sts_path[a2len] = '\0';
                    }
                    v = a2end + 1;
                }
            }

            /* skip to ')' */
            while (*v && *v != ')') v++;
            if (*v == ')') v++;

            out->ext_lib_count++;

            /* skip comma */
            while (*v == ' ' || *v == '\t') v++;
            if (*v == ',') v++;
        }
    }

    fclose(f);
    return out->main_path[0] != '\0';
}

/* ── CLI helpers ── */

typedef enum {
    EmitExe,   /* stasha build  — link into executable  */
    EmitLib,   /* stasha lib    — archive into .a        */
    EmitDylib, /* stasha dylib  — link into .so/.dylib   */
    EmitTest,  /* stasha test   — executable in test mode */
} emit_mode_t;

static void print_version(void) {
    printf("stasha %s\n", STASHA_VERSION);
}

static void print_help(void) {
    printf(
        "stasha %s\n"
        "\n"
        "Usage:\n"
        "  stasha [build] <file.sts> [-o <output>]   Compile to executable\n"
        "  stasha lib     <file.sts> [-o <output>]   Compile to static library\n"
        "  stasha dylib   <file.sts> [-o <output>]   Compile to dynamic library\n"
        "  stasha test    <file.sts> [-o <output>]   Compile and run tests\n"
        "\n"
        "Options:\n"
        "  -o <output>        Set output path\n"
        "                       build default: a.out\n"
        "                       lib   default: lib<name>.a  (derived from filename)\n"
        "                       dylib default: lib<name>.dylib / .so\n"
        "                       test  default: a.test\n"
        "  -g                 Emit DWARF debug info (enables source-level debugging)\n"
        "  --target <triple>  Cross-compile for the given target triple\n"
        "  --version          Print version and exit\n"
        "  -h, --help         Print this help and exit\n"
        "\n"
        "Examples:\n"
        "  stasha main.sts                            # -> a.out\n"
        "  stasha build main.sts -o myapp             # -> myapp\n"
        "  stasha build main.sts -g -o myapp          # -> myapp (with debug info)\n"
        "  stasha lib   mathlib.sts                   # -> libmathlib.a\n"
        "  stasha lib   mathlib.sts -o out.a          # -> out.a\n"
        "  stasha dylib mathlib.sts                   # -> libmathlib.dylib\n"
        "  stasha test  main.sts                      # run tests\n"
        "  stasha build main.sts --target x86_64-linux-gnu\n",
        STASHA_VERSION
    );
}

/*
 * Derive a default library output name from the input filename.
 * "path/to/mathlib.sts" -> "libmathlib.a"
 */
static void derive_lib_name(const char *input_path, char *out, usize_t out_size) {
    const char *base = strrchr(input_path, '/');
    if (!base) base = strrchr(input_path, '\\');
    base = base ? base + 1 : input_path;

    char stem[256];
    strncpy(stem, base, sizeof(stem) - 1);
    stem[sizeof(stem) - 1] = '\0';
    char *dot = strrchr(stem, '.');
    if (dot) *dot = '\0';

    snprintf(out, out_size, "lib%s.a", stem);
}

/*
 * Derive a default dynamic library output name.
 * "path/to/mathlib.sts" -> "libmathlib.dylib" (macOS) or "libmathlib.so" (Linux)
 */
static void derive_dylib_name(const char *input_path, char *out, usize_t out_size) {
    const char *base = strrchr(input_path, '/');
    if (!base) base = strrchr(input_path, '\\');
    base = base ? base + 1 : input_path;

    char stem[256];
    strncpy(stem, base, sizeof(stem) - 1);
    stem[sizeof(stem) - 1] = '\0';
    char *dot = strrchr(stem, '.');
    if (dot) *dot = '\0';

#if defined(__APPLE__)
    snprintf(out, out_size, "lib%s.dylib", stem);
#elif defined(_WIN32)
    snprintf(out, out_size, "%s.dll", stem);
#else
    snprintf(out, out_size, "lib%s.so", stem);
#endif
}

int main(int argc, char **argv) {
    init_bin_dir(argv[0]);
    if (open_logger() != Ok) return 1;

    /* ── early flags that don't need a file ── */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0) {
            print_version();
            return 0;
        }
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_help();
            return 0;
        }
    }

    if (argc < 2) {
        print_help();
        quit(Err);
    }

    /* ── subcommand detection ── */
    emit_mode_t mode = EmitExe;
    int file_arg = 1;

    if (strcmp(argv[1], "build") == 0) {
        mode = EmitExe;
        file_arg = 2;
    } else if (strcmp(argv[1], "lib") == 0) {
        mode = EmitLib;
        file_arg = 2;
    } else if (strcmp(argv[1], "dylib") == 0) {
        mode = EmitDylib;
        file_arg = 2;
    } else if (strcmp(argv[1], "test") == 0) {
        mode = EmitTest;
        file_arg = 2;
    }
    /* else: argv[1] is the file — default to EmitExe */

    /* ── project file fallback: look for sts.sproj when no file arg given ── */
    static char proj_input_buf[512];
    static char proj_output_buf[512];
    static sproj_t sproj_cfg;

    if (file_arg >= argc) {
        if (!parse_sproj("sts.sproj", &sproj_cfg)) {
            log_err("expected <file.sts> after '%s'", argv[file_arg - 1]);
            quit(Err);
        }
        strncpy(proj_input_buf, sproj_cfg.main_path, sizeof(proj_input_buf) - 1);
        if (sproj_cfg.has_output) {
            strncpy(proj_output_buf, sproj_cfg.output_path, sizeof(proj_output_buf) - 1);
            if (sproj_cfg.is_library && mode == EmitExe)
                mode = EmitLib;
        }
        log_msg("project file: main='%s'%s", proj_input_buf,
                sproj_cfg.has_output ? "" : " (no output specified)");
        file_arg = -1; /* sentinel: input_path already set */
    }

    const char *input_path = (file_arg == -1) ? proj_input_buf : argv[file_arg];

    /* ── default output path ── */
    char lib_name_buf[300];
    const char *output_path;
    if (file_arg == -1 && sproj_cfg.has_output) {
        output_path = proj_output_buf;
    } else {
        switch (mode) {
            case EmitLib:
                derive_lib_name(input_path, lib_name_buf, sizeof(lib_name_buf));
                output_path = lib_name_buf;
                break;
            case EmitDylib:
                derive_dylib_name(input_path, lib_name_buf, sizeof(lib_name_buf));
                output_path = lib_name_buf;
                break;
            case EmitTest:
                output_path = "a.test";
                break;
            default:
                output_path = "a.out";
                break;
        }
    }

    /* ── parse remaining options ── */
    const char *target_triple = Null;
    boolean_t debug_mode = False;
    int opts_start = (file_arg == -1) ? (int)argc : file_arg + 1;
    for (int i = opts_start; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
            output_path = argv[++i];
        else if (strcmp(argv[i], "--target") == 0 && i + 1 < argc)
            target_triple = argv[++i];
        else if (strcmp(argv[i], "-g") == 0)
            debug_mode = True;
    }

    /* ── compile ── */
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

    boolean_t test_mode = (mode == EmitTest) ? True : False;
    log_msg("generating code%s%s", test_mode ? " (test mode)" : "",
            debug_mode ? " (debug)" : "");
    if (codegen(ast, obj_path, test_mode, target_triple, input_path, debug_mode) != Ok) {
        log_err("code generation failed");
        quit(Err);
    }

    if (mode == EmitLib) {
        log_msg("archiving");
        if (archive_object(obj_path, output_path) != Ok) {
            remove(obj_path);
            log_err("archiving failed");
            quit(Err);
        }
        remove(obj_path);
    } else if (mode == EmitDylib) {
        /* derive the directory of the input file for resolving relative lib paths */
        char input_dir[512];
        strncpy(input_dir, input_path, sizeof(input_dir) - 1);
        input_dir[sizeof(input_dir) - 1] = '\0';
        char *input_sep = strrchr(input_dir, '/');
        if (!input_sep) input_sep = strrchr(input_dir, '\\');
        if (input_sep) *input_sep = '\0';
        else { input_dir[0] = '.'; input_dir[1] = '\0'; }

        /* collect custom library paths */
        const char *extra_lib_buf[64];
        static char resolved_lib_paths[64][1024];
        usize_t extra_lib_count = 0;
        for (usize_t i = 0; i < ast->as.module.decls.count; i++) {
            node_t *d = ast->as.module.decls.items[i];
            const char *lib_path = Null;
            if (d->kind == NodeLib && d->as.lib_decl.path && extra_lib_count < 63)
                lib_path = d->as.lib_decl.path;
            else if (d->kind == NodeLibImp && extra_lib_count < 63) {
                if (d->as.libimp_decl.from_std) {
                    snprintf(resolved_lib_paths[extra_lib_count],
                             sizeof(resolved_lib_paths[extra_lib_count]),
                             "%s/stdlib/lib%s.a", bin_dir, d->as.libimp_decl.name);
                    extra_lib_buf[extra_lib_count] = resolved_lib_paths[extra_lib_count];
                    extra_lib_count++;
                    continue;
                }
                lib_path = d->as.libimp_decl.path;
            }
            if (lib_path && extra_lib_count < 63) {
                if (lib_path[0] != '/' && !(lib_path[0] && lib_path[1] == ':')) {
                    snprintf(resolved_lib_paths[extra_lib_count],
                             sizeof(resolved_lib_paths[extra_lib_count]),
                             "%s/%s", input_dir, lib_path);
                    extra_lib_buf[extra_lib_count] = resolved_lib_paths[extra_lib_count];
                } else {
                    extra_lib_buf[extra_lib_count] = lib_path;
                }
                extra_lib_count++;
            }
        }
        /* append ext_libs from sts.sproj (project-mode) */
        if (file_arg == -1) {
            for (usize_t i = 0; i < sproj_cfg.ext_lib_count && extra_lib_count < 63; i++) {
                if (sproj_cfg.ext_libs[i].arc_path[0])
                    extra_lib_buf[extra_lib_count++] = sproj_cfg.ext_libs[i].arc_path;
            }
        }
        extra_lib_buf[extra_lib_count] = Null;

        log_msg("linking dynamic library");
        if (link_dynamic(obj_path, output_path,
                         extra_lib_count > 0 ? extra_lib_buf : Null) != Ok) {
            remove(obj_path);
            log_err("dynamic linking failed");
            quit(Err);
        }
#if defined(__APPLE__)
        if (debug_mode) {
            char dsym_path[600];
            snprintf(dsym_path, sizeof(dsym_path), "%s.dSYM", output_path);
            log_msg("extracting debug info -> '%s'", dsym_path);
            pid_t pid = fork();
            if (pid == 0) {
                execlp("dsymutil", "dsymutil", output_path,
                       "-o", dsym_path, (char *)Null);
                _exit(1);
            } else if (pid > 0) {
                int status;
                waitpid(pid, &status, 0);
            }
        }
#endif
        remove(obj_path);
    } else {
        /* derive the directory of the input file for resolving relative lib paths */
        char input_dir[512];
        strncpy(input_dir, input_path, sizeof(input_dir) - 1);
        input_dir[sizeof(input_dir) - 1] = '\0';
        char *input_sep = strrchr(input_dir, '/');
        if (!input_sep) input_sep = strrchr(input_dir, '\\');
        if (input_sep) *input_sep = '\0';
        else { input_dir[0] = '.'; input_dir[1] = '\0'; }

        /* collect custom library paths from lib/libimp declarations */
        const char *extra_lib_buf[64];
        static char resolved_lib_paths[64][1024];
        usize_t extra_lib_count = 0;
        for (usize_t i = 0; i < ast->as.module.decls.count; i++) {
            node_t *d = ast->as.module.decls.items[i];
            const char *lib_path = Null;
            if (d->kind == NodeLib && d->as.lib_decl.path && extra_lib_count < 63)
                lib_path = d->as.lib_decl.path;
            else if (d->kind == NodeLibImp && extra_lib_count < 63) {
                if (d->as.libimp_decl.from_std) {
                    snprintf(resolved_lib_paths[extra_lib_count],
                             sizeof(resolved_lib_paths[extra_lib_count]),
                             "%s/stdlib/lib%s.a", bin_dir, d->as.libimp_decl.name);
                    extra_lib_buf[extra_lib_count] = resolved_lib_paths[extra_lib_count];
                    extra_lib_count++;
                    continue;
                }
                lib_path = d->as.libimp_decl.path;
            }
            if (lib_path && extra_lib_count < 63) {
                if (lib_path[0] != '/' && !(lib_path[0] && lib_path[1] == ':')) {
                    /* relative path — resolve against input file's directory */
                    snprintf(resolved_lib_paths[extra_lib_count],
                             sizeof(resolved_lib_paths[extra_lib_count]),
                             "%s/%s", input_dir, lib_path);
                    extra_lib_buf[extra_lib_count] = resolved_lib_paths[extra_lib_count];
                } else {
                    extra_lib_buf[extra_lib_count] = lib_path;
                }
                extra_lib_count++;
            }
        }
        /* append ext_libs from sts.sproj (project-mode) */
        if (file_arg == -1) {
            for (usize_t i = 0; i < sproj_cfg.ext_lib_count && extra_lib_count < 63; i++) {
                if (sproj_cfg.ext_libs[i].arc_path[0])
                    extra_lib_buf[extra_lib_count++] = sproj_cfg.ext_libs[i].arc_path;
            }
        }
        extra_lib_buf[extra_lib_count] = Null; /* NULL-terminate */

        log_msg("linking");
        if (link_object(obj_path, output_path,
                        extra_lib_count > 0 ? extra_lib_buf : Null) != Ok) {
            remove(obj_path);
            log_err("linking failed");
            quit(Err);
        }
#if defined(__APPLE__)
        if (debug_mode) {
            /* On macOS, DWARF lives in the .o file. Run dsymutil to aggregate
               debug info into a .dSYM bundle next to the executable. */
            char dsym_path[600];
            snprintf(dsym_path, sizeof(dsym_path), "%s.dSYM", output_path);
            log_msg("extracting debug info -> '%s'", dsym_path);
            pid_t pid = fork();
            if (pid == 0) {
                execlp("dsymutil", "dsymutil", output_path,
                       "-o", dsym_path, (char *)Null);
                _exit(1);
            } else if (pid > 0) {
                int status;
                waitpid(pid, &status, 0);
            }
        }
#endif
        remove(obj_path);
    }

    log_msg("compiled '%s' -> '%s'", input_path, output_path);

    ast_free_all();
    deallocate(source_heap);

    quit(Ok);
}
