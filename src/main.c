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
#include "preprocessor/preprocessor.h"
#include "codegen/codegen.h"
#include "linker/linker.h"
#include "tooling/editor.h"
#include "cinterop/cheader.h"
#include "cinterop/cheader.c"

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

static void compile_cleanup(void) {
    ast_free_all();
    if (source_heap.pointer) {
        deallocate(source_heap);
        source_heap.pointer = NULL; /* clear so re-entrant compile_file() is safe */
        source_heap.size    = 0;
    }
}

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

/* ── pre-scan: collect exported macro sets from imported modules ── */
/*
 * Before preprocessing the main file we quick-scan it for `imp` declarations,
 * preprocess each referenced module, and collect their exported macro sets.
 * Those sets are then passed to pp_process() for the main file so that
 * external macros (ext macro fn/let) become available for expansion.
 *
 * We keep the intermediate pp_stream_t objects alive in a static array
 * (they own the export sets) and free them after the main pp_process finishes.
 */

#define MAX_PP_IMP_STREAMS 64

static pp_stream_t *s_pp_imp_streams[MAX_PP_IMP_STREAMS];
static int          s_pp_imp_cnt = 0;

static void free_pp_imp_streams(void) {
    for (int i = 0; i < s_pp_imp_cnt; i++)
        pp_stream_free(s_pp_imp_streams[i]);
    s_pp_imp_cnt = 0;
}

#define MAX_PP_IMPORT_SETS 64
static pp_macro_set_t *s_import_sets[MAX_PP_IMPORT_SETS];
static int             s_import_set_cnt = 0;

/*
 * gather_import_macro_sets — lex `source` to find `imp` declarations, then
 * preprocess each imported .sts file to extract its ext-macro exports.
 *
 * Returns a pointer to the static s_import_sets array; writes count to *out_count.
 * After use, call free_pp_imp_streams() to release the intermediate streams.
 */
static pp_macro_set_t **gather_import_macro_sets(const char *source,
                                                   const char *source_path,
                                                   int *out_count) {
    s_import_set_cnt = 0;
    s_pp_imp_cnt     = 0;
    *out_count       = 0;

    /* Derive source directory for resolving relative imports. */
    char dir[512];
    strncpy(dir, source_path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    {
        char *sep = strrchr(dir, '/');
        if (!sep) sep = strrchr(dir, '\\');
        if (sep)  *sep = '\0';
        else { dir[0] = '.'; dir[1] = '\0'; }
    }

    /* Quick-lex the source to collect imported module names. */
    char mod_names[MAX_PP_IMPORT_SETS][256];
    int  mod_count = 0;

    lexer_t lex;
    init_lexer(&lex, source);
    for (;;) {
        token_t tok = next_token(&lex);
        if (tok.kind == TokEof) break;

        if (tok.kind != TokImp) continue;
        if (mod_count >= MAX_PP_IMPORT_SETS) break;

        /* imp  dotted.name  ( = alias )?  ; */
        char mod[256];
        int  mlen = 0;
        tok = next_token(&lex);
        while (tok.kind == TokIdent && mlen < 254) {
            int tlen = (int)tok.length;
            if (mlen + tlen >= 254) break;
            memcpy(mod + mlen, tok.start, (size_t)tlen);
            mlen += tlen;
            tok = next_token(&lex);
            if (tok.kind == TokDot) {
                tok = next_token(&lex);
                if (tok.kind == TokIdent && mlen < 253) {
                    mod[mlen++] = '.';
                    continue;
                }
                break;
            }
            break;
        }
        mod[mlen] = '\0';

        /* Skip remainder (optional alias, etc.) up to ';'. */
        while (tok.kind != TokSemicolon && tok.kind != TokEof)
            tok = next_token(&lex);

        if (mlen > 0) {
            strncpy(mod_names[mod_count++], mod, 255);
            mod_names[mod_count - 1][255] = '\0';
        }
    }

    /* Preprocess each imported module and collect ext-macro exports. */
    for (int i = 0; i < mod_count; i++) {
        /* Convert dotted name → relative filesystem path. */
        char rel[512];
        strncpy(rel, mod_names[i], sizeof(rel) - 1);
        for (char *c = rel; *c; c++) if (*c == '.') *c = '/';

        char mod_path[1024];
        snprintf(mod_path, sizeof(mod_path), "%s/%s.sts", dir, rel);

        char *src = read_imp_source(mod_path);
        if (!src) continue;

        pp_stream_t *mod_pp = pp_process(src, mod_path, NULL, 0);
        if (!mod_pp) continue;

        pp_macro_set_t *exp = pp_get_exports(mod_pp);
        if (exp && (exp->fn_count > 0 || exp->let_count > 0)
                && s_import_set_cnt < MAX_PP_IMPORT_SETS
                && s_pp_imp_cnt < MAX_PP_IMP_STREAMS) {
            s_import_sets[s_import_set_cnt++] = exp;
            s_pp_imp_streams[s_pp_imp_cnt++]  = mod_pp;
        } else {
            pp_stream_free(mod_pp);
        }
    }

    *out_count = s_import_set_cnt;
    return s_import_set_cnt > 0 ? s_import_sets : NULL;
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
            diag_begin_error("cannot find module '%s'", mod_name);
            diag_span(SRC_LOC(decl->line, decl->col ? decl->col : 1, 1), True,
                      "looked for '%s'", mod_path);
            diag_finish();
            continue;
        }

        /* Preprocess and parse the imported file.
           pp_process registers the source with the diagnostic system,
           and parse_from_stream restores the diagnostic file as it reads. */
        pp_stream_t *mod_pp = pp_process(src, mod_path, Null, 0);
        node_t *mod_ast = mod_pp ? parse_from_stream(mod_pp) : Null;
        pp_stream_free(mod_pp);
        /* Restore primary file context after parsing the import. */
        diag_set_file(input_path);
        if (!mod_ast) {
            diag_begin_error("failed to parse module '%s'", mod_name);
            diag_span(SRC_LOC(decl->line, decl->col ? decl->col : 1, 1), True,
                      "imported here");
            diag_finish();
            continue;
        }

        /* splice declarations from the imported module */
        for (usize_t j = 0; j < mod_ast->as.module.decls.count; j++) {
            node_t *d = mod_ast->as.module.decls.items[j];
            if (d->kind == NodeImpDecl) continue;  /* handled by outer loop */

            if (lib_backed)
                d->from_lib = True;

            /* tag every spliced declaration with its source module name so
             * codegen can mangle symbols correctly (e.g. "net.socket" →
             * "net__socket__symbol").  module_name stays NULL for root-module
             * declarations so they are never mangled. */
            d->module_name = ast_strdup(mod_name, strlen(mod_name));

            /* propagate module_name into inline struct/union methods so their
             * mangled names include the module prefix */
            if (d->kind == NodeTypeDecl) {
                for (usize_t m = 0; m < d->as.type_decl.methods.count; m++)
                    d->as.type_decl.methods.items[m]->module_name = d->module_name;
            }

            node_list_push(&ast->as.module.decls, d);
        }
    }
}

static c_typedef_t *find_cheader_typedef(cheader_result_t *r, const char *name) {
    for (usize_t i = 0; i < r->tdef_count; i++)
        if (strcmp(r->tdefs[i].name, name) == 0) return &r->tdefs[i];
    return Null;
}

static boolean_t map_c_type(node_t *src_node, cheader_result_t *r,
                            const char *decl_name, c_type_t *ct,
                            type_info_t *out, long *array_len_out) {
    if (!ct || !out) return False;
    *out = NO_TYPE;
    if (array_len_out) *array_len_out = 0;

    boolean_t quiet_skip = decl_name
        && ((decl_name[0] == '_' && decl_name[1] == '_')
            || strcmp(decl_name, "arg") == 0);

    if (ct->kind == CTypeArray) {
        if (array_len_out) *array_len_out = ct->array_len;
        return map_c_type(src_node, r, decl_name, ct->elem, out, Null);
    }

    if (ct->kind == CTypePointer) {
        type_info_t inner = NO_TYPE;
        if (!ct->elem || !map_c_type(src_node, r, decl_name, ct->elem, &inner, Null))
            return False;
        if (inner.is_pointer || inner.base == TypeFnPtr) {
            if (!quiet_skip) {
                diag_begin_warning("skipping unsupported C declaration '%s'", decl_name ? decl_name : "?");
                diag_span(DIAG_NODE(src_node), True,
                          "nested pointers and function pointers are not supported here yet");
                diag_finish();
            }
            return False;
        }
        *out = inner;
        out->is_pointer = True;
        out->ptr_perm = ct->is_const ? PtrRead : PtrReadWrite;
        return True;
    }

    if (ct->kind == CTypeTypedefRef && ct->name) {
        if (strcmp(ct->name, "size_t") == 0
                || strcmp(ct->name, "uint64_t") == 0
                || strcmp(ct->name, "uintptr_t") == 0) {
            out->base = TypeU64; return True;
        }
        if (strcmp(ct->name, "ssize_t") == 0
                || strcmp(ct->name, "ptrdiff_t") == 0
                || strcmp(ct->name, "int64_t") == 0) {
            out->base = TypeI64; return True;
        }
        if (strcmp(ct->name, "int32_t") == 0) { out->base = TypeI32; return True; }
        if (strcmp(ct->name, "uint32_t") == 0) { out->base = TypeU32; return True; }
        if (strcmp(ct->name, "int16_t") == 0) { out->base = TypeI16; return True; }
        if (strcmp(ct->name, "uint16_t") == 0) { out->base = TypeU16; return True; }
        if (strcmp(ct->name, "int8_t") == 0) { out->base = TypeI8; return True; }
        if (strcmp(ct->name, "uint8_t") == 0) { out->base = TypeU8; return True; }
        c_typedef_t *td = find_cheader_typedef(r, ct->name);
        if (td) return map_c_type(src_node, r, decl_name, &td->actual, out, array_len_out);
        out->base = TypeUser;
        out->user_name = ast_strdup(ct->name, strlen(ct->name));
        return True;
    }

    switch (ct->kind) {
        case CTypeVoid:      out->base = TypeVoid; return True;
        case CTypeChar:
        case CTypeSChar:     out->base = TypeI8; return True;
        case CTypeUChar:     out->base = TypeU8; return True;
        case CTypeShort:     out->base = TypeI16; return True;
        case CTypeUShort:    out->base = TypeU16; return True;
        case CTypeInt:       out->base = TypeI32; return True;
        case CTypeUInt:      out->base = TypeU32; return True;
        case CTypeLong:
        case CTypeLongLong:  out->base = TypeI64; return True;
        case CTypeULong:
        case CTypeULongLong: out->base = TypeU64; return True;
        case CTypeFloat:     out->base = TypeF32; return True;
        case CTypeDouble:    out->base = TypeF64; return True;
        case CTypeStructRef:
        case CTypeUnionRef:
        case CTypeEnumRef:
            if (!ct->name) break;
            out->base = TypeUser;
            out->user_name = ast_strdup(ct->name, strlen(ct->name));
            return True;
        default:
            break;
    }

    if (!quiet_skip) {
        diag_begin_warning("skipping unsupported C declaration '%s'", decl_name ? decl_name : "?");
        diag_span(DIAG_NODE(src_node), True, "could not map this C type into Stasha");
        diag_finish();
    }
    return False;
}

static node_t *make_cheader_int_lit(node_t *src_node, long value) {
    node_t *n = make_node(NodeIntLitExpr, src_node->line);
    n->col = src_node->col;
    n->source_file = src_node->source_file;
    n->as.int_lit.value = value;
    return n;
}

static void process_cheader_decls(node_t *ast, const char *input_path) {
    node_list_t generated;
    node_list_init(&generated);

    for (usize_t i = 0; i < ast->as.module.decls.count; i++) {
        node_t *decl = ast->as.module.decls.items[i];
        if (decl->kind != NodeCHeader) continue;

        cheader_result_t result;
        char resolved[2048];
        if (!parse_cheader_file(decl->as.cheader_decl.path,
                                decl->as.cheader_decl.search_dirs,
                                input_path, &result, resolved, sizeof(resolved))) {
            diag_begin_error("cannot find C header '%s'", decl->as.cheader_decl.path);
            diag_span(DIAG_NODE(decl), True, "header declared here");
            diag_finish();
            continue;
        }

        for (usize_t j = 0; j < result.struct_count; j++) {
            c_struct_t *st = &result.structs[j];
            if (!st->name || !st->name[0]) continue;
            node_t *tn = make_node(NodeTypeDecl, decl->line);
            tn->col = decl->col;
            tn->source_file = decl->source_file;
            tn->is_c_extern = True;
            tn->as.type_decl.name = ast_strdup(st->name, strlen(st->name));
            tn->as.type_decl.decl_kind = st->is_union ? TypeDeclUnion : TypeDeclStruct;
            tn->as.type_decl.attr_flags |= AttrCLayout;
            node_list_init(&tn->as.type_decl.fields);
            node_list_init(&tn->as.type_decl.methods);
            node_list_init(&tn->as.type_decl.variants);

            for (usize_t k = 0; k < st->field_count; k++) {
                c_field_t *f = &st->fields[k];
                type_info_t ti;
                long array_len = 0;
                if (!map_c_type(decl, &result, f->name, &f->type, &ti, &array_len))
                    continue;
                node_t *field = make_node(NodeVarDecl, decl->line);
                field->col = decl->col;
                field->source_file = decl->source_file;
                field->is_c_extern = True;
                field->as.var_decl.name = ast_strdup(f->name, strlen(f->name));
                field->as.var_decl.type = ti;
                field->as.var_decl.storage = StorageStack;
                if (array_len > 0) {
                    field->as.var_decl.flags          |= VdeclArray;
                    field->as.var_decl.array_ndim       = 1;
                    field->as.var_decl.array_sizes[0]   = array_len;
                }
                node_list_push(&tn->as.type_decl.fields, field);
            }
            node_list_push(&generated, tn);
        }

        for (usize_t j = 0; j < result.enum_count; j++) {
            c_enum_t *en = &result.enums[j];
            if (!en->name || !en->name[0]) continue;
            node_t *tn = make_node(NodeTypeDecl, decl->line);
            tn->col = decl->col;
            tn->source_file = decl->source_file;
            tn->is_c_extern = True;
            tn->as.type_decl.name = ast_strdup(en->name, strlen(en->name));
            tn->as.type_decl.decl_kind = TypeDeclEnum;
            node_list_init(&tn->as.type_decl.fields);
            node_list_init(&tn->as.type_decl.methods);
            node_list_init(&tn->as.type_decl.variants);
            for (usize_t k = 0; k < en->variant_count; k++) {
                node_t *var = make_node(NodeEnumVariant, decl->line);
                var->col = decl->col;
                var->source_file = decl->source_file;
                var->is_c_extern = True;
                var->as.enum_variant.name = ast_strdup(en->variants[k].name, strlen(en->variants[k].name));
                node_list_push(&tn->as.type_decl.variants, var);
                if (!ch_result_has_const(&result, en->variants[k].name)) {
                    node_t *cn = make_node(NodeVarDecl, decl->line);
                    cn->col = decl->col;
                    cn->source_file = decl->source_file;
                    cn->is_c_extern = True;
                    cn->as.var_decl.name = ast_strdup(en->variants[k].name, strlen(en->variants[k].name));
                    cn->as.var_decl.type.base = TypeI32;
                    cn->as.var_decl.storage = StorageStack;
                    cn->as.var_decl.flags |= VdeclConst;
                    cn->as.var_decl.init = make_cheader_int_lit(decl, en->variants[k].value);
                    node_list_push(&generated, cn);
                }
            }
            node_list_push(&generated, tn);
        }

        for (usize_t j = 0; j < result.tdef_count; j++) {
            c_typedef_t *td = &result.tdefs[j];
            type_info_t ti;
            if (!map_c_type(decl, &result, td->name, &td->actual, &ti, Null))
                continue;
            node_t *tn = make_node(NodeTypeDecl, decl->line);
            tn->col = decl->col;
            tn->source_file = decl->source_file;
            tn->is_c_extern = True;
            tn->as.type_decl.name = ast_strdup(td->name, strlen(td->name));
            tn->as.type_decl.decl_kind = TypeDeclAlias;
            tn->as.type_decl.alias_type = ti;
            node_list_init(&tn->as.type_decl.fields);
            node_list_init(&tn->as.type_decl.methods);
            node_list_init(&tn->as.type_decl.variants);
            node_list_push(&generated, tn);
        }

        for (usize_t j = 0; j < result.fn_count; j++) {
            c_fn_t *fn = &result.fns[j];
            type_info_t ret;
            if (!map_c_type(decl, &result, fn->name, &fn->ret, &ret, Null))
                continue;
            node_t *fd = make_node(NodeFnDecl, decl->line);
            fd->col = decl->col;
            fd->source_file = decl->source_file;
            fd->from_lib = True;
            fd->is_c_extern = True;
            fd->as.fn_decl.name = ast_strdup(fn->name, strlen(fn->name));
            fd->as.fn_decl.linkage = LinkageExternal;
            fd->as.fn_decl.return_types = alloc_type_array(1);
            fd->as.fn_decl.return_types[0] = ret;
            fd->as.fn_decl.return_count = 1;
            fd->as.fn_decl.body = Null;
            fd->as.fn_decl.is_variadic = fn->is_variadic;
            node_list_init(&fd->as.fn_decl.params);
            for (usize_t k = 0; k < fn->param_count; k++) {
                type_info_t pti;
                if (!map_c_type(decl, &result, fn->params[k].name, &fn->params[k].type, &pti, Null))
                    goto skip_fn_decl;
                node_t *param = make_node(NodeVarDecl, decl->line);
                param->col = decl->col;
                param->source_file = decl->source_file;
                param->is_c_extern = True;
                param->as.var_decl.name = ast_strdup(fn->params[k].name, strlen(fn->params[k].name));
                param->as.var_decl.type = pti;
                param->as.var_decl.storage = StorageStack;
                node_list_push(&fd->as.fn_decl.params, param);
            }
            node_list_push(&generated, fd);
            continue;
skip_fn_decl:
            ;
        }

        for (usize_t j = 0; j < result.const_count; j++) {
            c_const_t *cn = &result.consts[j];
            node_t *vd = make_node(NodeVarDecl, decl->line);
            vd->col = decl->col;
            vd->source_file = decl->source_file;
            vd->is_c_extern = True;
            vd->as.var_decl.name = ast_strdup(cn->name, strlen(cn->name));
            vd->as.var_decl.type.base = TypeI64;
            vd->as.var_decl.storage = StorageStack;
            vd->as.var_decl.flags |= VdeclConst;
            vd->as.var_decl.init = make_cheader_int_lit(decl, cn->value);
            node_list_push(&generated, vd);
        }

        free_cheader_result(&result);
    }

    for (usize_t i = 0; i < generated.count; i++)
        node_list_push(&ast->as.module.decls, generated.items[i]);
}

/* ── sts.sproj project file ── */

#define SPROJ_MAX_LIBS    64
#define SPROJ_MAX_MODULES 16

typedef struct {
    char arc_path[512]; /* path to .a archive */
    char sts_path[512]; /* path to .sts interface file (may be empty) */
} sproj_lib_t;

typedef struct {
    char name[64];
} sproj_system_lib_t;

/* Type of artifact a [module] section produces. */
typedef enum {
    SprojModExe,    /* executable (default) */
    SprojModLib,    /* static library (.a)  */
    SprojModDylib,  /* dynamic library      */
    SprojModTest,   /* test runner          */
} sproj_mod_type_t;

/*
 * A named build module defined inside sts.sproj via a [name] section header.
 * Settings override the root-level defaults.
 *
 * Example:
 *   [debug]
 *   output   = "bin/myapp_debug"
 *   debug    = true
 *   optimize = 0
 *
 *   [release]
 *   output   = "bin/myapp"
 *   optimize = 3
 *
 *   [mytest]
 *   type   = "test"
 *   output = "bin/myapp_test"
 */
typedef struct {
    char              name[64];
    char              output[512];
    sproj_mod_type_t  type;
    boolean_t         debug;
    int               optimize;     /* -1 = inherit root default (2) */
    boolean_t         has_output;
    boolean_t         has_type;
    boolean_t         has_debug;
    boolean_t         has_optimize;
    sproj_lib_t       ext_libs[SPROJ_MAX_LIBS];
    usize_t           ext_lib_count;
    sproj_system_lib_t system_libs[SPROJ_MAX_LIBS];
    usize_t            system_lib_count;
} sproj_module_t;

typedef struct {
    char             main_path[512];
    char             output_path[512];
    boolean_t        is_library;
    boolean_t        has_output;
    sproj_lib_t      ext_libs[SPROJ_MAX_LIBS];
    usize_t          ext_lib_count;
    sproj_system_lib_t system_libs[SPROJ_MAX_LIBS];
    usize_t            system_lib_count;
    /* Named build modules defined via [name] section headers. */
    sproj_module_t   modules[SPROJ_MAX_MODULES];
    usize_t          module_count;
} sproj_t;

/*
 * Parse an ext_libs array starting at `*vp` into `libs`/`lib_cnt`.
 * Advances *vp past the closing ']'.
 * Format: [ ("arc.a" : "iface.sts"), ... ]
 */
static void parse_sproj_ext_libs(char **vp, sproj_lib_t *libs, usize_t *lib_cnt) {
    char *v = *vp;
    v++; /* skip '[' */
    while (*lib_cnt < SPROJ_MAX_LIBS) {
        while (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r') v++;
        if (*v == ']' || *v == '\0') break;
        if (*v != '(') { v++; continue; }
        v++;

        /* first string: archive path */
        while (*v == ' ' || *v == '\t') v++;
        if (*v != '"' && *v != '\'') { v++; continue; }
        char q1 = *v++;
        char *a1end = strchr(v, q1);
        if (!a1end) break;
        usize_t a1len = (usize_t)(a1end - v);
        if (a1len < sizeof(libs[*lib_cnt].arc_path)) {
            memcpy(libs[*lib_cnt].arc_path, v, a1len);
            libs[*lib_cnt].arc_path[a1len] = '\0';
        }
        v = a1end + 1;

        /* ':' separator */
        while (*v == ' ' || *v == '\t') v++;
        if (*v == ':') v++;

        /* second string: .sts interface path (optional) */
        while (*v == ' ' || *v == '\t') v++;
        if (*v == '"' || *v == '\'') {
            char q2 = *v++;
            char *a2end = strchr(v, q2);
            if (a2end) {
                usize_t a2len = (usize_t)(a2end - v);
                if (a2len < sizeof(libs[*lib_cnt].sts_path)) {
                    memcpy(libs[*lib_cnt].sts_path, v, a2len);
                    libs[*lib_cnt].sts_path[a2len] = '\0';
                }
                v = a2end + 1;
            }
        }

        while (*v && *v != ')') v++;
        if (*v == ')') v++;
        (*lib_cnt)++;

        while (*v == ' ' || *v == '\t') v++;
        if (*v == ',') v++;
    }
    *vp = v;
}

static void parse_sproj_system_libs(char **vp, sproj_system_lib_t *libs, usize_t *lib_cnt) {
    char *v = *vp;
    v++;
    while (*lib_cnt < SPROJ_MAX_LIBS) {
        while (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r') v++;
        if (*v == ']' || *v == '\0') break;
        if (*v != '"' && *v != '\'') { v++; continue; }
        char q = *v++;
        char *end = strchr(v, q);
        if (!end) break;
        usize_t len = (usize_t)(end - v);
        if (len >= sizeof(libs[*lib_cnt].name)) len = sizeof(libs[*lib_cnt].name) - 1;
        memcpy(libs[*lib_cnt].name, v, len);
        libs[*lib_cnt].name[len] = '\0';
        (*lib_cnt)++;
        v = end + 1;
        while (*v == ' ' || *v == '\t') v++;
        if (*v == ',') v++;
    }
    *vp = v;
}

/*
 * Parse a sts.sproj file at `path` into `out`.
 *
 * Root-level format (backward compatible):
 *   main     = "src/main.sts"
 *   binary   = "bin/name"        (or: library = "bin/name.a")
 *   ext_libs = []
 *   ext_libs = [ ("libs/x.a" : "libs/x.sts"), ... ]
 *
 * Module sections (optional):
 *   [module_name]
 *   output   = "bin/name_debug"
 *   type     = "test"            # exe | lib | dylib | test (default: exe)
 *   debug    = true              # emit debug info
 *   optimize = 0                 # LLVM opt level 0-3
 *   ext_libs = [ ... ]           # additional archives for this module
 *
 * Lines starting with '#' are comments.
 * Returns True if parsing succeeded and a root main path was found.
 */
static boolean_t parse_sproj(const char *path, sproj_t *out) {
    FILE *f = fopen(path, "r");
    if (!f) return False;

    memset(out, 0, sizeof(*out));

    /* cur_mod: index into out->modules of the [section] being parsed.
     * -1 means we are in the root scope. */
    int cur_mod = -1;

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        /* strip # comments */
        char *comment = strchr(line, '#');
        if (comment) *comment = '\0';

        /* trim leading whitespace */
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '\0' || *s == '\n' || *s == '\r') continue;

        /* ── [section] header: begin a new named module ── */
        if (*s == '[') {
            char *end = strchr(s + 1, ']');
            if (end && end > s + 1 && out->module_count < SPROJ_MAX_MODULES) {
                cur_mod = (int)out->module_count;
                sproj_module_t *m = &out->modules[cur_mod];
                memset(m, 0, sizeof(*m));
                usize_t nlen = (usize_t)(end - (s + 1));
                if (nlen >= sizeof(m->name)) nlen = sizeof(m->name) - 1;
                memcpy(m->name, s + 1, nlen);
                m->name[nlen] = '\0';
                m->optimize   = -1; /* inherit root default */
                out->module_count++;
            }
            continue;
        }

        /* ── key = value ── */
        char *eq = strchr(s, '=');
        if (!eq) continue;

        /* extract key (trim trailing whitespace) */
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

        /* ── string value: "..." or '...' ── */
        if (*v == '"' || *v == '\'') {
            char q = *v++;
            char *vend = strchr(v, q);
            if (!vend) continue;
            usize_t vlen = (usize_t)(vend - v);

            if (cur_mod < 0) {
                /* root scope */
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
            } else {
                /* module scope */
                sproj_module_t *m = &out->modules[cur_mod];
                if ((strcmp(key, "output") == 0 || strcmp(key, "binary") == 0)
                        && vlen < sizeof(m->output)) {
                    memcpy(m->output, v, vlen);
                    m->output[vlen] = '\0';
                    m->has_output = True;
                    if (!m->has_type) m->type = SprojModExe;
                } else if (strcmp(key, "library") == 0 && vlen < sizeof(m->output)) {
                    memcpy(m->output, v, vlen);
                    m->output[vlen] = '\0';
                    m->has_output = True;
                    m->has_type   = True;
                    m->type       = SprojModLib;
                } else if (strcmp(key, "dylib") == 0 && vlen < sizeof(m->output)) {
                    memcpy(m->output, v, vlen);
                    m->output[vlen] = '\0';
                    m->has_output = True;
                    m->has_type   = True;
                    m->type       = SprojModDylib;
                } else if (strcmp(key, "type") == 0) {
                    m->has_type = True;
                    if (vlen == 4 && strncmp(v, "test",  4) == 0) m->type = SprojModTest;
                    else if (vlen == 3 && strncmp(v, "lib",   3) == 0) m->type = SprojModLib;
                    else if (vlen == 5 && strncmp(v, "dylib", 5) == 0) m->type = SprojModDylib;
                    else m->type = SprojModExe;
                }
            }
            continue;
        }

        /* ── boolean / integer values (module scope only) ── */
        if (cur_mod >= 0) {
            sproj_module_t *m = &out->modules[cur_mod];
            if (strcmp(key, "debug") == 0) {
                m->debug     = (strncmp(v, "true", 4) == 0) ? True : False;
                m->has_debug = True;
                continue;
            }
            if (strcmp(key, "optimize") == 0) {
                int lvl = (int)strtol(v, NULL, 10);
                if (lvl >= 0 && lvl <= 3) {
                    m->optimize     = lvl;
                    m->has_optimize = True;
                }
                continue;
            }
        }

        if (*v != '[') continue;

        if (strcmp(key, "ext_libs") == 0) {
            if (cur_mod < 0) parse_sproj_ext_libs(&v, out->ext_libs, &out->ext_lib_count);
            else {
                sproj_module_t *m = &out->modules[cur_mod];
                parse_sproj_ext_libs(&v, m->ext_libs, &m->ext_lib_count);
            }
            continue;
        }
        if (strcmp(key, "system_libs") == 0) {
            if (cur_mod < 0) parse_sproj_system_libs(&v, out->system_libs, &out->system_lib_count);
            else {
                sproj_module_t *m = &out->modules[cur_mod];
                parse_sproj_system_libs(&v, m->system_libs, &m->system_lib_count);
            }
        }
    }

    fclose(f);
    return out->main_path[0] != '\0';
}

/* Find a module by name; returns NULL if not found. */
static sproj_module_t *sproj_find_module(sproj_t *proj, const char *name) {
    for (usize_t i = 0; i < proj->module_count; i++)
        if (strcmp(proj->modules[i].name, name) == 0)
            return &proj->modules[i];
    return NULL;
}

/* ── CLI helpers ── */

typedef enum {
    EmitExe,   /* stasha build  — link into executable  */
    EmitLib,   /* stasha lib    — archive into .a        */
    EmitDylib, /* stasha dylib  — link into .so/.dylib   */
    EmitTest,  /* stasha test   — executable in test mode */
} emit_mode_t;

static void print_editor_help(void);

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
        "  stasha check   <file.sts>                 Parse and type-check; emit JSON diagnostics\n"
        "  stasha tokens  <file.sts>                 Lex source; emit JSON token stream\n"
        "  stasha symbols <file.sts>                 Parse source; emit JSON symbols\n"
        "  stasha definition <file.sts>              Resolve definition at --line/--col; emit JSON\n"
        "\n"
        "Options:\n"
        "  -o <output>        Set output path\n"
        "                       build default: a.out\n"
        "                       lib   default: lib<name>.a  (derived from filename)\n"
        "                       dylib default: lib<name>.dylib / .so\n"
        "                       test  default: a.test\n"
        "  -o=<level>         Set LLVM optimization level (0=none, 1=less, 2=default, 3=aggressive)\n"
        "                       Default: 2 (LLVM default)\n"
        "  -g                 Emit DWARF debug info (enables source-level debugging)\n"
        "  --target <triple>  Cross-compile for the given target triple\n"
        "  --stdin            Read source from stdin instead of the filesystem\n"
        "  --path <path>      Virtual path to use with --stdin\n"
        "  --line <n>         Zero-based line for editor commands\n"
        "  --col <n>          Zero-based column for editor commands\n"
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
        "  stasha build main.sts --target x86_64-linux-gnu\n"
        "\n"
        "Project mode (sts.sproj in current directory):\n"
        "  stasha                  Build using root config in sts.sproj\n"
        "  stasha build            Build using root config in sts.sproj\n"
        "  stasha build debug      Build the [debug] module from sts.sproj\n"
        "  stasha build release    Build the [release] module from sts.sproj\n"
        "  stasha test             Build and run all [type = \"test\"] modules\n",
        STASHA_VERSION
    );
    printf("\n");
    print_editor_help();
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

static void print_editor_help(void) {
    printf(
        "Editor tooling:\n"
        "  stasha check   <file.sts> [--stdin --path <virtual-path>]\n"
        "  stasha tokens  <file.sts> [--stdin --path <virtual-path>]\n"
        "  stasha symbols <file.sts> [--stdin --path <virtual-path>]\n"
        "  stasha definition <file.sts> --line <n> --col <n> [--stdin --path <virtual-path>]\n"
        "\n"
        "These commands emit JSON for editor integrations.\n"
    );
}

static result_t run_editor_symbols(const char *source, const char *input_path) {
    diag_set_file(input_path);
    diag_register_source(input_path, source);
    diag_clear_captured();

    int imp_macro_count = 0;
    pp_macro_set_t **imp_macros = gather_import_macro_sets(source, input_path,
                                                           &imp_macro_count);
    pp_stream_t *pp = pp_process(source, input_path, imp_macros, imp_macro_count);
    node_t *ast = pp ? parse_from_stream(pp) : Null;
    pp_stream_free(pp);
    free_pp_imp_streams();
    if (ast) {
        resolve_imports(ast, input_path);
        process_cheader_decls(ast, input_path);
    }
    free_imp_sources();

    if (ast)
        editor_print_symbols_json(ast, input_path);
    else
        editor_print_diagnostics_json();

    return get_error_count() > 0 ? Err : Ok;
}

static result_t run_editor_check(const char *source, const char *input_path) {
    diag_set_file(input_path);
    diag_register_source(input_path, source);
    diag_clear_captured();

    int imp_macro_count = 0;
    pp_macro_set_t **imp_macros = gather_import_macro_sets(source, input_path,
                                                           &imp_macro_count);
    pp_stream_t *pp = pp_process(source, input_path, imp_macros, imp_macro_count);
    node_t *ast = pp ? parse_from_stream(pp) : Null;
    pp_stream_free(pp);
    free_pp_imp_streams();
    if (!ast) {
        free_imp_sources();
        editor_print_diagnostics_json();
        return Err;
    }

    resolve_imports(ast, input_path);
    process_cheader_decls(ast, input_path);
    free_imp_sources();

    char obj_path[512];
    snprintf(obj_path, sizeof(obj_path), "/tmp/stasha-check-%d.o", (int)getpid());
    if (codegen(ast, obj_path, False, Null, input_path, False, 0) == Ok)
        remove(obj_path);

    editor_print_diagnostics_json();
    return get_error_count() > 0 ? Err : Ok;
}

static result_t run_editor_definition(const char *source, const char *input_path,
                                      usize_t line, usize_t col) {
    diag_set_file(input_path);
    diag_register_source(input_path, source);
    diag_clear_captured();

    int imp_macro_count = 0;
    pp_macro_set_t **imp_macros = gather_import_macro_sets(source, input_path,
                                                           &imp_macro_count);
    pp_stream_t *pp = pp_process(source, input_path, imp_macros, imp_macro_count);
    node_t *ast = pp ? parse_from_stream(pp) : Null;
    pp_stream_free(pp);
    free_pp_imp_streams();
    if (!ast) {
        free_imp_sources();
        editor_print_diagnostics_json();
        return Err;
    }

    resolve_imports(ast, input_path);
    process_cheader_decls(ast, input_path);
    free_imp_sources();
    editor_print_definition_json(ast, input_path, line, col);
    return Ok;
}

/* ── core compilation pipeline ── */

/*
 * Parameters for a single compilation invocation.
 * `extra_lib_paths` is a NULL-terminated array of additional archive paths to
 * link (from sts.sproj ext_libs or -l flags).  May be NULL if unused.
 */
typedef struct {
    const char      *input_path;
    const char      *output_path;
    emit_mode_t      mode;
    boolean_t        debug_mode;
    int              opt_level;      /* LLVM optimisation level 0-3 */
    const char      *target_triple;  /* NULL = host target */
    const char     **extra_lib_paths;
    usize_t          extra_lib_count;
    /* When non-NULL, use this source text instead of reading input_path.
     * The caller owns and must free this buffer; compile_file() does NOT free it. */
    char            *source_override;
} cfile_params_t;

/*
 * compile_file — full pipeline: read → preprocess → parse → codegen → link.
 *
 * Handles EmitExe, EmitLib, EmitDylib, and EmitTest modes.
 * Leaves no temporary .o files behind on success or failure.
 * Returns Ok on success, Err on any failure.
 */
static result_t compile_file(const cfile_params_t *p) {
    /* ── read source (from file or caller-supplied buffer) ── */
    char *source;
    if (p->source_override) {
        source = p->source_override;
        /* source_heap stays NULL; compile_cleanup() won't try to free it */
    } else {
        source = read_file(p->input_path);
        if (!source) return Err;
    }

    diag_set_file(p->input_path);

    /* ── preprocess ── */
    int imp_macro_count = 0;
    pp_macro_set_t **imp_macros = gather_import_macro_sets(source, p->input_path,
                                                           &imp_macro_count);
    pp_stream_t *pp  = pp_process(source, p->input_path, imp_macros, imp_macro_count);
    node_t      *ast = pp ? parse_from_stream(pp) : Null;
    pp_stream_free(pp);
    free_pp_imp_streams();

    if (!ast) {
        log_err("parsing failed");
        compile_cleanup();
        return Err;
    }

    /* When building a library, propagate the module's own name to root
     * declarations so the archive uses consistent mangled symbols. */
    if (p->mode == EmitLib) {
        const char *self_mod = ast->as.module.name;
        if (self_mod && self_mod[0]) {
            node_list_t *rdecls = &ast->as.module.decls;
            for (usize_t i = 0; i < rdecls->count; i++) {
                node_t *d = rdecls->items[i];
                if (!d->module_name) {
                    d->module_name = ast_strdup(self_mod, strlen(self_mod));
                    if (d->kind == NodeTypeDecl) {
                        for (usize_t m = 0; m < d->as.type_decl.methods.count; m++) {
                            node_t *meth = d->as.type_decl.methods.items[m];
                            if (!meth->module_name) meth->module_name = d->module_name;
                        }
                    }
                }
            }
        }
    }

    resolve_imports(ast, p->input_path);
    process_cheader_decls(ast, p->input_path);
    free_imp_sources();

    /* ── codegen ── */
    char obj_path[512];
    snprintf(obj_path, sizeof(obj_path), "%s.o", p->output_path);

    boolean_t test_mode = (p->mode == EmitTest) ? True : False;
    log_msg("generating code%s%s", test_mode ? " (test mode)" : "",
            p->debug_mode ? " (debug)" : "");
    if (codegen(ast, obj_path, test_mode, p->target_triple, p->input_path,
                p->debug_mode, p->opt_level) != Ok) {
        log_err("code generation failed");
        compile_cleanup();
        return Err;
    }

    /* ── derive directory of input for relative lib resolution ── */
    char input_dir[512];
    strncpy(input_dir, p->input_path, sizeof(input_dir) - 1);
    input_dir[sizeof(input_dir) - 1] = '\0';
    {
        char *sep = strrchr(input_dir, '/');
        if (!sep) sep = strrchr(input_dir, '\\');
        if (sep) *sep = '\0';
        else { input_dir[0] = '.'; input_dir[1] = '\0'; }
    }

    /* ── collect library paths from AST lib/libimp declarations ── */
    const char *ast_lib_buf[64];
    char        ast_resolved[64][1024];
    usize_t     ast_lib_count = 0;

    for (usize_t i = 0; i < ast->as.module.decls.count; i++) {
        node_t *d = ast->as.module.decls.items[i];
        const char *lib_path = Null;
        if (d->kind == NodeLib && d->as.lib_decl.path && ast_lib_count < 63)
            lib_path = d->as.lib_decl.path;
        else if (d->kind == NodeLibImp && ast_lib_count < 63) {
            if (d->as.libimp_decl.from_std) {
                usize_t idx1 = ast_lib_count;
                snprintf(ast_resolved[idx1], sizeof(ast_resolved[idx1]),
                         "%s/stdlib/lib%s.a", bin_dir, d->as.libimp_decl.name);
                ast_lib_buf[ast_lib_count++] = ast_resolved[idx1];
                continue;
            }
            lib_path = d->as.libimp_decl.path;
        }
        if (lib_path && ast_lib_count < 63) {
            if (lib_path[0] != '/' && !(lib_path[0] && lib_path[1] == ':')) {
                usize_t idx2 = ast_lib_count;
                snprintf(ast_resolved[idx2], sizeof(ast_resolved[idx2]),
                         "%s/%s", input_dir, lib_path);
                ast_lib_buf[ast_lib_count++] = ast_resolved[idx2];
            } else {
                ast_lib_buf[ast_lib_count++] = lib_path;
            }
        }
    }

    /* ── build final library list: AST libs + caller-supplied extras ── */
    const char *all_libs[128];
    usize_t     all_lib_count = 0;

    for (usize_t i = 0; i < ast_lib_count && all_lib_count < 127; i++)
        all_libs[all_lib_count++] = ast_lib_buf[i];
    if (p->extra_lib_paths) {
        for (usize_t i = 0; i < p->extra_lib_count && all_lib_count < 127; i++)
            all_libs[all_lib_count++] = p->extra_lib_paths[i];
    }

    /* ── link / archive ── */
    result_t link_result = Ok;

    if (p->mode == EmitLib) {
        log_msg("archiving");
        link_result = archive_object(obj_path, p->output_path);
        if (link_result != Ok) log_err("archiving failed");

    } else if (p->mode == EmitDylib) {
        all_libs[all_lib_count] = Null;
        log_msg("linking dynamic library");
        link_result = link_dynamic(obj_path, p->output_path,
                                   all_lib_count > 0 ? all_libs : Null);
        if (link_result != Ok) log_err("dynamic linking failed");
#if defined(__APPLE__)
        if (link_result == Ok && p->debug_mode) {
            char dsym_path[600];
            snprintf(dsym_path, sizeof(dsym_path), "%s.dSYM", p->output_path);
            log_msg("extracting debug info -> '%s'", dsym_path);
            pid_t pid = fork();
            if (pid == 0) {
                execlp("dsymutil", "dsymutil", p->output_path, "-o", dsym_path, (char *)Null);
                _exit(1);
            } else if (pid > 0) {
                int status; waitpid(pid, &status, 0);
            }
        }
#endif

    } else {
        /* EmitExe / EmitTest: always link the thread and zone runtimes. */
        char rt_path[512];
        snprintf(rt_path, sizeof(rt_path), "%s/thread_runtime.a", bin_dir);
        if (all_lib_count < 127) all_libs[all_lib_count++] = rt_path;
        static char zone_rt_path[512];
        snprintf(zone_rt_path, sizeof(zone_rt_path), "%s/zone_runtime.a", bin_dir);
        if (all_lib_count < 127) all_libs[all_lib_count++] = zone_rt_path;
        all_libs[all_lib_count] = Null;

        log_msg("linking");
        link_result = link_object(obj_path, p->output_path,
                                  all_lib_count > 0 ? all_libs : Null);
        if (link_result != Ok) log_err("linking failed");
#if defined(__APPLE__)
        if (link_result == Ok && p->debug_mode) {
            char dsym_path[600];
            snprintf(dsym_path, sizeof(dsym_path), "%s.dSYM", p->output_path);
            log_msg("extracting debug info -> '%s'", dsym_path);
            pid_t pid = fork();
            if (pid == 0) {
                execlp("dsymutil", "dsymutil", p->output_path, "-o", dsym_path, (char *)Null);
                _exit(1);
            } else if (pid > 0) {
                int status; waitpid(pid, &status, 0);
            }
        }
#endif
    }

    remove(obj_path);
    compile_cleanup();

    if (link_result != Ok) return Err;

    log_msg("compiled '%s' -> '%s'", p->input_path, p->output_path);
    return Ok;
}

/* ── project-mode test runner ── */

/*
 * run_project_tests — build and run every module whose type is "test".
 *
 * `proj_dir` is the directory containing the sts.sproj file (used to resolve
 * relative paths in the module configurations).
 *
 * For each test module:
 *   1. Compile to a test binary using EmitTest mode.
 *   2. Fork and execute the binary.
 *   3. Report [PASS] / [FAIL] based on the exit code.
 *
 * Returns 0 if all test modules passed, 1 if any failed.
 */
static int run_project_tests(sproj_t *proj, const char *proj_dir) {
    /* Count how many test modules exist first. */
    usize_t test_mod_count = 0;
    for (usize_t i = 0; i < proj->module_count; i++)
        if (proj->modules[i].type == SprojModTest) test_mod_count++;

    if (test_mod_count == 0) {
        /* No [test] modules defined; fall through to default test build. */
        return -1;
    }

    int pass = 0, fail = 0;

    for (usize_t i = 0; i < proj->module_count; i++) {
        sproj_module_t *m = &proj->modules[i];
        if (m->type != SprojModTest) continue;

        /* ── resolve input path ── */
        char full_input[1024];
        snprintf(full_input, sizeof(full_input), "%s/%s", proj_dir, proj->main_path);

        /* ── resolve output path ── */
        char full_output[1024];
        if (m->has_output)
            snprintf(full_output, sizeof(full_output), "%s/%s", proj_dir, m->output);
        else
            snprintf(full_output, sizeof(full_output), "%s/a_%s.test", proj_dir, m->name);

        /* ── collect extra lib paths: root ext_libs then module ext_libs ── */
        const char *lib_ptrs[SPROJ_MAX_LIBS * 2 + 1];
        char        sys_lib_buf[SPROJ_MAX_LIBS * 2][128];
        usize_t     lib_count = 0;
        for (usize_t j = 0; j < proj->ext_lib_count && lib_count < SPROJ_MAX_LIBS; j++)
            if (proj->ext_libs[j].arc_path[0])
                lib_ptrs[lib_count++] = proj->ext_libs[j].arc_path;
        for (usize_t j = 0; j < m->ext_lib_count && lib_count < SPROJ_MAX_LIBS; j++)
            if (m->ext_libs[j].arc_path[0])
                lib_ptrs[lib_count++] = m->ext_libs[j].arc_path;
        for (usize_t j = 0; j < proj->system_lib_count && lib_count < SPROJ_MAX_LIBS * 2; j++) {
            snprintf(sys_lib_buf[lib_count], sizeof(sys_lib_buf[lib_count]),
                     "-l%s", proj->system_libs[j].name);
            lib_ptrs[lib_count] = sys_lib_buf[lib_count];
            lib_count++;
        }
        for (usize_t j = 0; j < m->system_lib_count && lib_count < SPROJ_MAX_LIBS * 2; j++) {
            snprintf(sys_lib_buf[lib_count], sizeof(sys_lib_buf[lib_count]),
                     "-l%s", m->system_libs[j].name);
            lib_ptrs[lib_count] = sys_lib_buf[lib_count];
            lib_count++;
        }
        lib_ptrs[lib_count] = NULL;

        boolean_t dbg = m->has_debug    ? m->debug    : False;
        int       opt = m->has_optimize ? m->optimize : 2;

        cfile_params_t cp = {
            .input_path      = full_input,
            .output_path     = full_output,
            .mode            = EmitTest,
            .debug_mode      = dbg,
            .opt_level       = opt,
            .target_triple   = Null,
            .extra_lib_paths = lib_count > 0 ? lib_ptrs : Null,
            .extra_lib_count = lib_count,
        };

        printf("  building test module '%s'...\n", m->name);
        fflush(stdout);

        if (compile_file(&cp) != Ok) {
            printf("  [FAIL] '%s'  (build failed)\n\n", m->name);
            fail++;
            continue;
        }

        /* ── run the test binary ── */
        printf("  running '%s'...\n", full_output);
        fflush(stdout);

#if defined(__APPLE__) || defined(__linux__)
        pid_t pid = fork();
        if (pid == 0) {
            /* child: exec the test binary */
            execl(full_output, full_output, (char *)Null);
            _exit(127);
        } else if (pid > 0) {
            int wstatus;
            waitpid(pid, &wstatus, 0);
            if (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0) {
                printf("  [PASS] '%s'\n\n", m->name);
                pass++;
            } else {
                int code = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : -1;
                printf("  [FAIL] '%s'  (exit %d)\n\n", m->name, code);
                fail++;
            }
        } else {
            printf("  [FAIL] '%s'  (fork failed)\n\n", m->name);
            fail++;
        }
#else
        /* Fallback for platforms without fork/exec: just report the binary. */
        printf("  [SKIP] '%s'  (auto-run not supported on this platform; binary at %s)\n\n",
               m->name, full_output);
#endif
    }

    printf("── project test results: %d/%d passed", pass, pass + fail);
    if (fail > 0) printf(", %d FAILED", fail);
    printf(" ──\n");

    return fail > 0 ? 1 : 0;
}

int main(int argc, char **argv) {
    init_bin_dir(argv[0]);

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
    emit_mode_t   mode        = EmitExe;
    editor_mode_t editor_mode = EditorModeNone;
    int           file_arg    = 1;

    /*
     * Module name from `stasha build <modname>`.  Empty string when not given.
     * A candidate is a non-flag argument after "build" that looks like an
     * identifier rather than a file path (no .sts suffix, no path separator).
     */
    static char selected_mod_name[64] = {0};

    if (strcmp(argv[1], "build") == 0) {
        mode = EmitExe; file_arg = 2;
        /* Peek at argv[2]: if it looks like a module name, consume it now. */
        if (argc >= 3 && argv[2][0] != '-') {
            const char *cand = argv[2];
            size_t clen = strlen(cand);
            boolean_t is_file = (clen > 4 && strcmp(cand + clen - 4, ".sts") == 0)
                             || (strchr(cand, '/')  != NULL)
                             || (strchr(cand, '\\') != NULL);
            if (!is_file) {
                strncpy(selected_mod_name, cand, sizeof(selected_mod_name) - 1);
                selected_mod_name[sizeof(selected_mod_name) - 1] = '\0';
                file_arg = 3; /* module name consumed */
            }
        }
    } else if (strcmp(argv[1], "lib") == 0) {
        mode = EmitLib;   file_arg = 2;
    } else if (strcmp(argv[1], "dylib") == 0) {
        mode = EmitDylib; file_arg = 2;
    } else if (strcmp(argv[1], "test") == 0) {
        mode = EmitTest;  file_arg = 2;
    } else if (strcmp(argv[1], "check") == 0) {
        editor_mode = EditorModeCheck;      file_arg = 2;
    } else if (strcmp(argv[1], "tokens") == 0) {
        editor_mode = EditorModeTokens;     file_arg = 2;
    } else if (strcmp(argv[1], "symbols") == 0) {
        editor_mode = EditorModeSymbols;    file_arg = 2;
    } else if (strcmp(argv[1], "definition") == 0) {
        editor_mode = EditorModeDefinition; file_arg = 2;
    }
    /* else: argv[1] is the file — default EmitExe */

    boolean_t  use_stdin    = False;
    const char *virtual_path = Null;
    usize_t    editor_line  = 0;
    usize_t    editor_col   = 0;
    boolean_t  has_editor_line = False;
    boolean_t  has_editor_col  = False;

    if (editor_mode != EditorModeNone) {
        log_set_stderr_enabled(False);
        diag_set_render_enabled(False);
    } else if (open_logger() != Ok) {
        return 1;
    }

    /* ── parse CLI flags ── */
    const char *target_triple     = Null;
    boolean_t   debug_mode        = False;
    int         optimization_level = 2;   /* LLVM default */
    char        lib_name_buf[300];
    const char *output_path       = Null;
    const char *cli_extra_libs[32];
    usize_t     cli_extra_lib_count = 0;

    const char *explicit_input_path =
        (file_arg >= 0 && file_arg < argc && argv[file_arg][0] != '-')
            ? argv[file_arg] : Null;

    int opts_start = (file_arg < 0 || file_arg >= argc) ? (int)argc : file_arg;
    for (int i = opts_start; i < argc; i++) {
        if (explicit_input_path == Null && argv[i][0] != '-') {
            explicit_input_path = argv[i];
        } else if (strcmp(argv[i], "--stdin") == 0) {
            use_stdin = True;
        } else if (strcmp(argv[i], "--path") == 0 && i + 1 < argc) {
            virtual_path = argv[++i];
        } else if (strcmp(argv[i], "--line") == 0 && i + 1 < argc) {
            editor_line = (usize_t)strtoul(argv[++i], NULL, 10);
            has_editor_line = True;
        } else if (strcmp(argv[i], "--col") == 0 && i + 1 < argc) {
            editor_col = (usize_t)strtoul(argv[++i], NULL, 10);
            has_editor_col = True;
        } else if (strncmp(argv[i], "-o=", 3) == 0) {
            const char *level = argv[i] + 3;
            if (level[0] >= '0' && level[0] <= '3' && level[1] == '\0')
                optimization_level = (int)(level[0] - '0');
            else { log_err("invalid optimization level '%s' (expected -o=0..3)", level); quit(Err); }
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_path = argv[++i];
        } else if (strcmp(argv[i], "--target") == 0 && i + 1 < argc) {
            target_triple = argv[++i];
        } else if (strcmp(argv[i], "-g") == 0) {
            debug_mode = True;
        } else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc && cli_extra_lib_count < 32) {
            cli_extra_libs[cli_extra_lib_count++] = argv[++i];
        } else if (strcmp(argv[i], "--strict") == 0) {
            diag_config_t cfg = diag_get_config();
            cfg.strict = True;
            diag_set_config(cfg);
        } else if (strcmp(argv[i], "--wall") == 0) {
            diag_config_t cfg = diag_get_config();
            cfg.enabled = WarnAll;
            diag_set_config(cfg);
        }
    }

    /* ── project file: load sts.sproj when no explicit file is given ── */
    static char     proj_input_buf[512]  = {0};
    static char     proj_output_buf[512] = {0};
    static sproj_t  sproj_cfg;
    sproj_module_t *sel_mod    = Null;  /* selected [module], if any */
    boolean_t       proj_mode  = False;

    /* Holds extra lib paths built from sproj ext_libs + -l flags (project mode). */
    static char proj_lib_paths_buf[128][1024];
    static char proj_sys_lib_buf[128][128];
    const char *proj_lib_ptrs[129];
    usize_t     proj_lib_count = 0;

    boolean_t want_proj = editor_mode == EditorModeNone
                       && !use_stdin
                       && (file_arg >= argc || selected_mod_name[0]);

    if (want_proj) {
        if (!parse_sproj("sts.sproj", &sproj_cfg)) {
            if (selected_mod_name[0])
                log_err("module '%s' given but no sts.sproj found in current directory",
                        selected_mod_name);
            else
                log_err("expected <file.sts> (or run from a project directory with sts.sproj)");
            quit(Err);
        }

        /* ── project auto-test: dispatch all [type = "test"] modules ── */
        if (mode == EmitTest && !selected_mod_name[0]) {
            int rc = run_project_tests(&sproj_cfg, ".");
            if (rc >= 0) quit(rc == 0 ? Ok : Err);
            /* rc == -1: no [test] modules defined; fall through to default test build */
        }

        /* ── resolve module selection ── */
        if (selected_mod_name[0]) {
            sel_mod = sproj_find_module(&sproj_cfg, selected_mod_name);
            if (!sel_mod) {
                log_err("module '%s' not found in sts.sproj", selected_mod_name);
                if (sproj_cfg.module_count > 0) {
                    log_err("defined modules:");
                    for (usize_t i = 0; i < sproj_cfg.module_count; i++)
                        log_err("  %s", sproj_cfg.modules[i].name);
                }
                quit(Err);
            }
        }

        strncpy(proj_input_buf, sproj_cfg.main_path, sizeof(proj_input_buf) - 1);

        if (sel_mod) {
            /* Apply module output path */
            if (sel_mod->has_output)
                strncpy(proj_output_buf, sel_mod->output, sizeof(proj_output_buf) - 1);
            else if (sproj_cfg.has_output)
                strncpy(proj_output_buf, sproj_cfg.output_path, sizeof(proj_output_buf) - 1);

            /* Module type overrides the emit mode */
            if (sel_mod->has_type) {
                switch (sel_mod->type) {
                    case SprojModLib:   mode = EmitLib;   break;
                    case SprojModDylib: mode = EmitDylib; break;
                    case SprojModTest:  mode = EmitTest;  break;
                    default:            mode = EmitExe;   break;
                }
            } else if (sproj_cfg.is_library && mode == EmitExe) {
                mode = EmitLib;
            }

            /* debug/optimize: module setting wins if CLI didn't override */
            if (sel_mod->has_debug    && !debug_mode)
                debug_mode = sel_mod->debug;
            if (sel_mod->has_optimize && optimization_level == 2)
                optimization_level = sel_mod->optimize;

            /* Collect ext_libs: root first, then module-specific */
            for (usize_t i = 0; i < sproj_cfg.ext_lib_count && proj_lib_count < 128; i++) {
                if (sproj_cfg.ext_libs[i].arc_path[0]) {
                    strncpy(proj_lib_paths_buf[proj_lib_count],
                            sproj_cfg.ext_libs[i].arc_path,
                            sizeof(proj_lib_paths_buf[proj_lib_count]) - 1);
                    proj_lib_ptrs[proj_lib_count] = proj_lib_paths_buf[proj_lib_count];
                    proj_lib_count++;
                }
            }
            for (usize_t i = 0; i < sproj_cfg.system_lib_count && proj_lib_count < 128; i++) {
                snprintf(proj_sys_lib_buf[proj_lib_count], sizeof(proj_sys_lib_buf[proj_lib_count]),
                         "-l%s", sproj_cfg.system_libs[i].name);
                proj_lib_ptrs[proj_lib_count] = proj_sys_lib_buf[proj_lib_count];
                proj_lib_count++;
            }
            for (usize_t i = 0; i < sel_mod->ext_lib_count && proj_lib_count < 128; i++) {
                if (sel_mod->ext_libs[i].arc_path[0]) {
                    strncpy(proj_lib_paths_buf[proj_lib_count],
                            sel_mod->ext_libs[i].arc_path,
                            sizeof(proj_lib_paths_buf[proj_lib_count]) - 1);
                    proj_lib_ptrs[proj_lib_count] = proj_lib_paths_buf[proj_lib_count];
                    proj_lib_count++;
                }
            }
            for (usize_t i = 0; i < sel_mod->system_lib_count && proj_lib_count < 128; i++) {
                snprintf(proj_sys_lib_buf[proj_lib_count], sizeof(proj_sys_lib_buf[proj_lib_count]),
                         "-l%s", sel_mod->system_libs[i].name);
                proj_lib_ptrs[proj_lib_count] = proj_sys_lib_buf[proj_lib_count];
                proj_lib_count++;
            }
        } else {
            /* Root-level settings only */
            if (sproj_cfg.has_output)
                strncpy(proj_output_buf, sproj_cfg.output_path, sizeof(proj_output_buf) - 1);
            if (sproj_cfg.is_library && mode == EmitExe)
                mode = EmitLib;

            for (usize_t i = 0; i < sproj_cfg.ext_lib_count && proj_lib_count < 128; i++) {
                if (sproj_cfg.ext_libs[i].arc_path[0]) {
                    strncpy(proj_lib_paths_buf[proj_lib_count],
                            sproj_cfg.ext_libs[i].arc_path,
                            sizeof(proj_lib_paths_buf[proj_lib_count]) - 1);
                    proj_lib_ptrs[proj_lib_count] = proj_lib_paths_buf[proj_lib_count];
                    proj_lib_count++;
                }
            }
            for (usize_t i = 0; i < sproj_cfg.system_lib_count && proj_lib_count < 128; i++) {
                snprintf(proj_sys_lib_buf[proj_lib_count], sizeof(proj_sys_lib_buf[proj_lib_count]),
                         "-l%s", sproj_cfg.system_libs[i].name);
                proj_lib_ptrs[proj_lib_count] = proj_sys_lib_buf[proj_lib_count];
                proj_lib_count++;
            }
        }

        /* Append CLI -l libraries */
        for (usize_t i = 0; i < cli_extra_lib_count && proj_lib_count < 128; i++)
            proj_lib_ptrs[proj_lib_count++] = cli_extra_libs[i];
        proj_lib_ptrs[proj_lib_count] = Null;

        log_msg("project: main='%s' module=%s", proj_input_buf,
                sel_mod ? sel_mod->name : "(root)");

        proj_mode = True;
    }

    /* ── resolve input path ── */
    const char *input_path = Null;
    if (use_stdin) {
        input_path = virtual_path ? virtual_path : "<stdin>";
    } else if (proj_mode) {
        input_path = proj_input_buf;
    } else {
        input_path = explicit_input_path;
    }

    if (!input_path) {
        log_err("expected <file.sts>");
        quit(Err);
    }

    if (editor_mode == EditorModeDefinition && (!has_editor_line || !has_editor_col)) {
        log_err("'definition' requires --line <n> and --col <n>");
        quit(Err);
    }

    /* ── default output path (CLI -o takes precedence) ── */
    if (!output_path) {
        if (proj_mode && proj_output_buf[0]) {
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
    }

    if (debug_mode && optimization_level != 0) {
        log_msg("optimization is automatically set to 0 when emitting debug symbols with '-g'.");
        optimization_level = 0;
    }

    /* ── editor modes — read source then dispatch, no code generation ── */
    char *source = use_stdin ? editor_read_stdin() : read_file(input_path);
    char *stdin_buffer = use_stdin ? source : Null;
    if (!source) {
        if (editor_mode != EditorModeNone) {
            diag_clear_captured();
            diag_begin_error("could not open file '%s'", input_path);
            diag_finish();
            editor_print_diagnostics_json();
        }
        quit(Err);
    }

    if (editor_mode == EditorModeTokens) {
        editor_print_tokens_json(source, input_path);
        compile_cleanup();
        if (stdin_buffer) editor_free_buffer(stdin_buffer);
        quit(Ok);
    }
    if (editor_mode == EditorModeSymbols) {
        result_t res = run_editor_symbols(source, input_path);
        compile_cleanup();
        if (stdin_buffer) editor_free_buffer(stdin_buffer);
        quit(res);
    }
    if (editor_mode == EditorModeCheck) {
        result_t res = run_editor_check(source, input_path);
        compile_cleanup();
        if (stdin_buffer) editor_free_buffer(stdin_buffer);
        quit(res);
    }
    if (editor_mode == EditorModeDefinition) {
        result_t res = run_editor_definition(source, input_path, editor_line, editor_col);
        compile_cleanup();
        if (stdin_buffer) editor_free_buffer(stdin_buffer);
        quit(res);
    }

    /* For compile modes: source was read above only so we could check for stdin.
     * compile_file() reads the source internally for file paths.
     * For stdin input we pass the buffer via source_override so it is not re-read. */
    char *stdin_compile_buf = Null;
    if (use_stdin) {
        stdin_compile_buf = source;
        /* Don't let compile_cleanup free it via source_heap. */
    } else {
        /* Discard the read_file() allocation; compile_file() will re-read. */
        compile_cleanup();
    }

    /* ── build extra libs list ── */
    const char **extra_lib_paths = Null;
    usize_t      extra_lib_count  = 0;
    if (proj_mode) {
        extra_lib_paths = proj_lib_count > 0 ? proj_lib_ptrs : Null;
        extra_lib_count = proj_lib_count;
    } else if (cli_extra_lib_count > 0) {
        extra_lib_paths = cli_extra_libs;
        extra_lib_count = cli_extra_lib_count;
    }

    /* ── compile ── */
    cfile_params_t cp = {
        .input_path      = input_path,
        .output_path     = output_path,
        .mode            = mode,
        .debug_mode      = debug_mode,
        .opt_level       = optimization_level,
        .target_triple   = target_triple,
        .extra_lib_paths = extra_lib_paths,
        .extra_lib_count = extra_lib_count,
        .source_override = stdin_compile_buf,
    };

    result_t res = compile_file(&cp);

    if (stdin_compile_buf) editor_free_buffer(stdin_compile_buf);
    quit(res);
}
