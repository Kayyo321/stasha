#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "cheader.h"

typedef struct {
    const char *path;
    char *source;
    usize_t pos;
    usize_t len;
} ch_scan_t;

typedef struct {
    heap_t heap;
    char **items;
    usize_t count;
    usize_t cap;
} ch_str_list_t;

typedef struct {
    int kind;
    char *text;
} ch_tok_t;

enum {
    CHTokEof = 0,
    CHTokIdent,
    CHTokNumber,
    CHTokString,
    CHTokLParen,
    CHTokRParen,
    CHTokLBrace,
    CHTokRBrace,
    CHTokLBracket,
    CHTokRBracket,
    CHTokSemicolon,
    CHTokComma,
    CHTokStar,
    CHTokAssign,
    CHTokColon,
    CHTokEllipsis,
} ;

typedef struct {
    const char *path;
    ch_tok_t *toks;
    usize_t count;
    usize_t pos;
    heap_t heap;
} ch_tok_stream_t;

typedef struct {
    cheader_result_t *out;
    ch_str_list_t seen_files;
    ch_str_list_t type_names;
    ch_str_list_t search_dirs;
    boolean_t top_level_found;
} ch_ctx_t;

static const char *ch_default_dirs[] = {
    "/usr/include",
    "/usr/local/include",
    "/opt/homebrew/include",
#if defined(__APPLE__)
    "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include",
#endif
    NULL,
};

static cheader_result_t *ch_active_result = NULL;

/* True while scanning the cheader path the user explicitly wrote.
 * Toggled to False around #include recursion so transitively-pulled
 * declarations (system headers, support helpers) can be distinguished
 * from the user's own definitions when issuing FFI-fatal errors. */
static boolean_t ch_at_top_file = True;

static void ch_track_heap(heap_t h) {
    if (!ch_active_result || !h.pointer) return;
    if (ch_active_result->owned_heap_count >= ch_active_result->owned_heap_cap) {
        usize_t new_cap = ch_active_result->owned_heap_cap < 32 ? 32 : ch_active_result->owned_heap_cap * 2;
        if (ch_active_result->owned_heaps_heap.pointer == Null)
            ch_active_result->owned_heaps_heap = allocate(new_cap, sizeof(heap_t));
        else
            ch_active_result->owned_heaps_heap =
                reallocate(ch_active_result->owned_heaps_heap, new_cap * sizeof(heap_t));
        ch_active_result->owned_heaps = ch_active_result->owned_heaps_heap.pointer;
        ch_active_result->owned_heap_cap = new_cap;
    }
    ch_active_result->owned_heaps[ch_active_result->owned_heap_count++] = h;
}

static char *ch_strdup(const char *s, usize_t len) {
    heap_t h = allocate(len + 1, sizeof(char));
    ch_track_heap(h);
    char *out = h.pointer;
    memcpy(out, s, len);
    out[len] = '\0';
    return out;
}

static void ch_str_list_push(ch_str_list_t *list, const char *s) {
    if (!s || !s[0]) return;
    for (usize_t i = 0; i < list->count; i++)
        if (strcmp(list->items[i], s) == 0) return;
    if (list->count >= list->cap) {
        usize_t new_cap = list->cap < 8 ? 8 : list->cap * 2;
        if (list->heap.pointer == Null)
            list->heap = allocate(new_cap, sizeof(char *));
        else
            list->heap = reallocate(list->heap, new_cap * sizeof(char *));
        list->items = list->heap.pointer;
        list->cap = new_cap;
    }
    list->items[list->count++] = ch_strdup(s, strlen(s));
}

static boolean_t ch_str_list_has(ch_str_list_t *list, const char *s) {
    for (usize_t i = 0; i < list->count; i++)
        if (strcmp(list->items[i], s) == 0) return True;
    return False;
}

static char *ch_read_file(const char *path, heap_t *out_heap) {
    FILE *f = fopen(path, "rb");
    if (!f) return Null;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    heap_t h = allocate((usize_t)sz + 1, sizeof(char));
    char *buf = h.pointer;
    fread(buf, 1, (size_t)sz, f);
    buf[sz] = '\0';
    fclose(f);
    if (out_heap) *out_heap = h;
    return buf;
}

static void ch_dirname(const char *path, char *out, usize_t out_cap) {
    strncpy(out, path, out_cap - 1);
    out[out_cap - 1] = '\0';
    char *sep = strrchr(out, '/');
    if (!sep) sep = strrchr(out, '\\');
    if (sep) *sep = '\0';
    else { out[0] = '.'; out[1] = '\0'; }
}

static void ch_join_path(const char *dir, const char *name, char *out, usize_t out_cap) {
    if (!dir || !dir[0]) snprintf(out, out_cap, "%s", name);
    else snprintf(out, out_cap, "%s/%s", dir, name);
}

static void ch_add_search_dirs(ch_ctx_t *ctx, const char *input_path, const char *extra_dirs) {
    char base_dir[1024];
    ch_dirname(input_path, base_dir, sizeof(base_dir));
    ch_str_list_push(&ctx->search_dirs, base_dir);

    if (extra_dirs && extra_dirs[0]) {
        const char *p = extra_dirs;
        while (*p) {
            const char *start = p;
            while (*p && *p != ':') p++;
            if (p > start) {
                char buf[1024];
                usize_t len = (usize_t)(p - start);
                if (len >= sizeof(buf)) len = sizeof(buf) - 1;
                memcpy(buf, start, len);
                buf[len] = '\0';
                ch_str_list_push(&ctx->search_dirs, buf);
            }
            if (*p == ':') p++;
        }
    }

    for (usize_t i = 0; ch_default_dirs[i]; i++)
        ch_str_list_push(&ctx->search_dirs, ch_default_dirs[i]);
}

static boolean_t ch_resolve_header(ch_ctx_t *ctx, const char *cur_file,
                                   const char *header, boolean_t quoted,
                                   char *out, usize_t out_cap) {
    if (!header || !header[0]) return False;
    if (header[0] == '/' || (header[0] && header[1] == ':')) {
        FILE *f = fopen(header, "rb");
        if (!f) return False;
        fclose(f);
        snprintf(out, out_cap, "%s", header);
        return True;
    }

    if (quoted && cur_file) {
        char cur_dir[1024];
        char cand[2048];
        ch_dirname(cur_file, cur_dir, sizeof(cur_dir));
        ch_join_path(cur_dir, header, cand, sizeof(cand));
        FILE *f = fopen(cand, "rb");
        if (f) {
            fclose(f);
            snprintf(out, out_cap, "%s", cand);
            return True;
        }
    }

    for (usize_t i = 0; i < ctx->search_dirs.count; i++) {
        char cand[2048];
        ch_join_path(ctx->search_dirs.items[i], header, cand, sizeof(cand));
        FILE *f = fopen(cand, "rb");
        if (f) {
            fclose(f);
            snprintf(out, out_cap, "%s", cand);
            return True;
        }
    }
    return False;
}

static long ch_parse_int_literal(const char *text, boolean_t *ok) {
    char buf[128];
    usize_t len = strlen(text);
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, text, len);
    buf[len] = '\0';

    char *s = buf;
    while (*s == ' ' || *s == '\t' || *s == '(') s++;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ')' || end[-1] == ' ' || end[-1] == '\t'))
        *--end = '\0';
    while (end > s && (end[-1] == 'u' || end[-1] == 'U'
            || end[-1] == 'l' || end[-1] == 'L'))
        *--end = '\0';

    if (*s == '\0') { *ok = False; return 0; }
    char *parse_end = Null;
    long v = strtol(s, &parse_end, 0);
    while (parse_end && (*parse_end == ' ' || *parse_end == '\t')) parse_end++;
    *ok = (parse_end && *parse_end == '\0');
    return v;
}

static boolean_t ch_result_has_fn(cheader_result_t *r, const char *name) {
    for (usize_t i = 0; i < r->fn_count; i++)
        if (strcmp(r->fns[i].name, name) == 0) return True;
    return False;
}

static boolean_t ch_result_has_struct(cheader_result_t *r, const char *name) {
    for (usize_t i = 0; i < r->struct_count; i++)
        if (strcmp(r->structs[i].name, name) == 0) return True;
    return False;
}

static boolean_t ch_result_has_typedef(cheader_result_t *r, const char *name) {
    for (usize_t i = 0; i < r->tdef_count; i++)
        if (strcmp(r->tdefs[i].name, name) == 0) return True;
    return False;
}

static boolean_t ch_result_has_enum(cheader_result_t *r, const char *name) {
    for (usize_t i = 0; i < r->enum_count; i++)
        if (strcmp(r->enums[i].name, name) == 0) return True;
    return False;
    }

static boolean_t ch_result_has_const(cheader_result_t *r, const char *name) {
    for (usize_t i = 0; i < r->const_count; i++)
        if (strcmp(r->consts[i].name, name) == 0) return True;
    return False;
}

static boolean_t ch_result_has_global(cheader_result_t *r, const char *name) {
    for (usize_t i = 0; i < r->global_count; i++)
        if (strcmp(r->globals[i].name, name) == 0) return True;
    return False;
}

static void ch_push_fn(cheader_result_t *r, c_fn_t fn) {
    if (!fn.name || ch_result_has_fn(r, fn.name)) return;
    fn.from_user_header = ch_at_top_file;
    if (r->fn_count == 0) {
        r->fns_heap = allocate(8, sizeof(c_fn_t));
        r->fns = r->fns_heap.pointer;
    } else if ((r->fn_count & (r->fn_count - 1)) == 0) {
        usize_t cap = r->fn_count < 8 ? 8 : r->fn_count;
        r->fns_heap = reallocate(r->fns_heap, (cap * 2) * sizeof(c_fn_t));
        r->fns = r->fns_heap.pointer;
    }
    r->fns[r->fn_count++] = fn;
}

static void ch_push_struct(cheader_result_t *r, c_struct_t st) {
    if (!st.name || ch_result_has_struct(r, st.name)) return;
    st.from_user_header = ch_at_top_file;
    if (r->struct_count == 0) {
        r->structs_heap = allocate(8, sizeof(c_struct_t));
        r->structs = r->structs_heap.pointer;
    } else if ((r->struct_count & (r->struct_count - 1)) == 0) {
        usize_t cap = r->struct_count < 8 ? 8 : r->struct_count;
        r->structs_heap = reallocate(r->structs_heap, (cap * 2) * sizeof(c_struct_t));
        r->structs = r->structs_heap.pointer;
    }
    r->structs[r->struct_count++] = st;
}

static void ch_push_typedef(cheader_result_t *r, c_typedef_t td) {
    if (!td.name || ch_result_has_typedef(r, td.name)) return;
    td.from_user_header = ch_at_top_file;
    if (r->tdef_count == 0) {
        r->tdefs_heap = allocate(8, sizeof(c_typedef_t));
        r->tdefs = r->tdefs_heap.pointer;
    } else if ((r->tdef_count & (r->tdef_count - 1)) == 0) {
        usize_t cap = r->tdef_count < 8 ? 8 : r->tdef_count;
        r->tdefs_heap = reallocate(r->tdefs_heap, (cap * 2) * sizeof(c_typedef_t));
        r->tdefs = r->tdefs_heap.pointer;
    }
    r->tdefs[r->tdef_count++] = td;
}

static void ch_push_enum(cheader_result_t *r, c_enum_t en) {
    if (!en.name || ch_result_has_enum(r, en.name)) return;
    if (r->enum_count == 0) {
        r->enums_heap = allocate(8, sizeof(c_enum_t));
        r->enums = r->enums_heap.pointer;
    } else if ((r->enum_count & (r->enum_count - 1)) == 0) {
        usize_t cap = r->enum_count < 8 ? 8 : r->enum_count;
        r->enums_heap = reallocate(r->enums_heap, (cap * 2) * sizeof(c_enum_t));
        r->enums = r->enums_heap.pointer;
    }
    r->enums[r->enum_count++] = en;
}

static void ch_push_const(cheader_result_t *r, c_const_t cn) {
    if (!cn.name || ch_result_has_const(r, cn.name)) return;
    if (r->const_count == 0) {
        r->consts_heap = allocate(8, sizeof(c_const_t));
        r->consts = r->consts_heap.pointer;
    } else if ((r->const_count & (r->const_count - 1)) == 0) {
        usize_t cap = r->const_count < 8 ? 8 : r->const_count;
        r->consts_heap = reallocate(r->consts_heap, (cap * 2) * sizeof(c_const_t));
        r->consts = r->consts_heap.pointer;
    }
    r->consts[r->const_count++] = cn;
}

static void ch_push_global(cheader_result_t *r, c_global_t g) {
    if (!g.name || ch_result_has_global(r, g.name)) return;
    g.from_user_header = ch_at_top_file;
    if (r->global_count == 0) {
        r->globals_heap = allocate(8, sizeof(c_global_t));
        r->globals = r->globals_heap.pointer;
    } else if ((r->global_count & (r->global_count - 1)) == 0) {
        usize_t cap = r->global_count < 8 ? 8 : r->global_count;
        r->globals_heap = reallocate(r->globals_heap, (cap * 2) * sizeof(c_global_t));
        r->globals = r->globals_heap.pointer;
    }
    r->globals[r->global_count++] = g;
}

static c_type_t ch_type_make(c_type_kind_t kind) {
    c_type_t t;
    memset(&t, 0, sizeof(t));
    t.kind = kind;
    return t;
}

static c_type_t ch_type_clone(c_type_t *src) {
    c_type_t out = *src;
    if (src->name) out.name = ch_strdup(src->name, strlen(src->name));
    if (src->elem) {
        heap_t h = allocate(1, sizeof(c_type_t));
        ch_track_heap(h);
        out.elem = h.pointer;
        *out.elem = ch_type_clone(src->elem);
    }
    return out;
}

static void ch_type_free(c_type_t *t) {
    if (!t) return;
    if (t->elem) ch_type_free(t->elem);
}

static void ch_skip_ws(ch_scan_t *s) {
    while (s->pos < s->len) {
        char c = s->source[s->pos];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            s->pos++;
            continue;
        }
        if (c == '/' && s->pos + 1 < s->len && s->source[s->pos + 1] == '/') {
            s->pos += 2;
            while (s->pos < s->len && s->source[s->pos] != '\n') s->pos++;
            continue;
        }
        if (c == '/' && s->pos + 1 < s->len && s->source[s->pos + 1] == '*') {
            s->pos += 2;
            while (s->pos + 1 < s->len) {
                if (s->source[s->pos] == '*' && s->source[s->pos + 1] == '/') {
                    s->pos += 2;
                    break;
                }
                s->pos++;
            }
            continue;
        }
        break;
    }
}

static ch_tok_t ch_make_tok(int kind, const char *s, usize_t len) {
    ch_tok_t tok;
    tok.kind = kind;
    tok.text = ch_strdup(s, len);
    return tok;
}

static void ch_tok_push(ch_tok_stream_t *ts, ch_tok_t tok) {
    if (ts->count == 0) ts->heap = NullHeap;
    if (ts->count == 0) {
        ts->heap = allocate(256, sizeof(ch_tok_t));
        ts->toks = ts->heap.pointer;
    } else if ((ts->count & (ts->count - 1)) == 0) {
        usize_t cap = ts->count < 256 ? 256 : ts->count;
        ts->heap = reallocate(ts->heap, (cap * 2) * sizeof(ch_tok_t));
        ts->toks = ts->heap.pointer;
    }
    ts->toks[ts->count++] = tok;
}

static void ch_tokenize_code(const char *path, char *code, ch_tok_stream_t *out) {
    ch_scan_t s = {path, code, 0, strlen(code)};
    out->path = path;
    while (1) {
        ch_skip_ws(&s);
        if (s.pos >= s.len) break;
        char c = s.source[s.pos];
        if (isalpha((unsigned char)c) || c == '_') {
            usize_t start = s.pos++;
            while (s.pos < s.len && (isalnum((unsigned char)s.source[s.pos]) || s.source[s.pos] == '_'))
                s.pos++;
            usize_t len = s.pos - start;
            if (len == 13 && memcmp(s.source + start, "__attribute__", 13) == 0) {
                ch_skip_ws(&s);
                if (s.pos < s.len && s.source[s.pos] == '(') {
                    int depth = 0;
                    do {
                        if (s.source[s.pos] == '(') depth++;
                        else if (s.source[s.pos] == ')') depth--;
                        s.pos++;
                    } while (s.pos < s.len && depth > 0);
                }
                continue;
            }
            ch_tok_push(out, ch_make_tok(CHTokIdent, s.source + start, len));
            continue;
        }
        if (isdigit((unsigned char)c) || (c == '-' && s.pos + 1 < s.len && isdigit((unsigned char)s.source[s.pos + 1]))) {
            usize_t start = s.pos++;
            while (s.pos < s.len && (isalnum((unsigned char)s.source[s.pos]) || s.source[s.pos] == 'x'
                    || s.source[s.pos] == 'X' || s.source[s.pos] == '+' || s.source[s.pos] == '-'))
                s.pos++;
            ch_tok_push(out, ch_make_tok(CHTokNumber, s.source + start, s.pos - start));
            continue;
        }
        if (c == '"') {
            usize_t start = s.pos++;
            while (s.pos < s.len && s.source[s.pos] != '"') {
                if (s.source[s.pos] == '\\' && s.pos + 1 < s.len) s.pos += 2;
                else s.pos++;
            }
            if (s.pos < s.len) s.pos++;
            ch_tok_push(out, ch_make_tok(CHTokString, s.source + start, s.pos - start));
            continue;
        }
        if (c == '.' && s.pos + 2 < s.len && s.source[s.pos + 1] == '.' && s.source[s.pos + 2] == '.') {
            ch_tok_push(out, ch_make_tok(CHTokEllipsis, s.source + s.pos, 3));
            s.pos += 3;
            continue;
        }
        switch (c) {
            case '(': ch_tok_push(out, ch_make_tok(CHTokLParen, "(", 1)); break;
            case ')': ch_tok_push(out, ch_make_tok(CHTokRParen, ")", 1)); break;
            case '{': ch_tok_push(out, ch_make_tok(CHTokLBrace, "{", 1)); break;
            case '}': ch_tok_push(out, ch_make_tok(CHTokRBrace, "}", 1)); break;
            case '[': ch_tok_push(out, ch_make_tok(CHTokLBracket, "[", 1)); break;
            case ']': ch_tok_push(out, ch_make_tok(CHTokRBracket, "]", 1)); break;
            case ';': ch_tok_push(out, ch_make_tok(CHTokSemicolon, ";", 1)); break;
            case ',': ch_tok_push(out, ch_make_tok(CHTokComma, ",", 1)); break;
            case '*': ch_tok_push(out, ch_make_tok(CHTokStar, "*", 1)); break;
            case '=': ch_tok_push(out, ch_make_tok(CHTokAssign, "=", 1)); break;
            case ':': ch_tok_push(out, ch_make_tok(CHTokColon, ":", 1)); break;
            default: s.pos++; continue;
        }
        s.pos++;
    }
    ch_tok_push(out, ch_make_tok(CHTokEof, "", 0));
}

static ch_tok_t *ch_peek(ch_tok_stream_t *ts) {
    if (ts->pos >= ts->count) return &ts->toks[ts->count - 1];
    return &ts->toks[ts->pos];
}

static ch_tok_t *ch_prev(ch_tok_stream_t *ts) {
    if (ts->pos == 0) return &ts->toks[0];
    return &ts->toks[ts->pos - 1];
}

static boolean_t ch_match(ch_tok_stream_t *ts, int kind) {
    if (ch_peek(ts)->kind != kind) return False;
    ts->pos++;
    return True;
}

static boolean_t ch_is_ident(ch_tok_stream_t *ts, const char *s) {
    ch_tok_t *t = ch_peek(ts);
    return t->kind == CHTokIdent && strcmp(t->text, s) == 0;
}

static boolean_t ch_is_type_name(ch_ctx_t *ctx, const char *s) {
    static const char *builtin[] = {
        "void", "char", "short", "int", "long", "float", "double",
        "signed", "unsigned", "const", "volatile", "struct", "union",
        "enum", "size_t", "ssize_t", "ptrdiff_t", "uintptr_t", "int8_t",
        "uint8_t", "int16_t", "uint16_t", "int32_t", "uint32_t", "int64_t",
        "uint64_t", NULL
    };
    for (usize_t i = 0; builtin[i]; i++)
        if (strcmp(builtin[i], s) == 0) return True;
    return ch_str_list_has(&ctx->type_names, s);
}

static void ch_skip_balanced(ch_tok_stream_t *ts, int open_kind, int close_kind) {
    if (!ch_match(ts, open_kind)) return;
    int depth = 1;
    while (depth > 0 && ch_peek(ts)->kind != CHTokEof) {
        if (ch_match(ts, open_kind)) depth++;
        else if (ch_match(ts, close_kind)) depth--;
        else ts->pos++;
    }
}

static c_type_t ch_parse_declspec(ch_ctx_t *ctx, ch_tok_stream_t *ts,
                                  boolean_t *is_typedef, boolean_t *is_const,
                                  char **pending_inline_name,
                                  c_struct_t *inline_struct_out,
                                  c_enum_t *inline_enum_out);

static c_type_t ch_parse_type_name(ch_ctx_t *ctx, ch_tok_stream_t *ts) {
    boolean_t is_typedef = False, is_const = False;
    char *inline_name = Null;
    c_struct_t st; memset(&st, 0, sizeof(st));
    c_enum_t en; memset(&en, 0, sizeof(en));
    c_type_t t = ch_parse_declspec(ctx, ts, &is_typedef, &is_const,
                                   &inline_name, &st, &en);
    if (inline_name) {
        if (st.name) ch_push_struct(ctx->out, st);
        if (en.name) ch_push_enum(ctx->out, en);
    }
    return t;
}

static void ch_parse_enum_body(ch_tok_stream_t *ts, c_enum_t *out) {
    long next_val = 0;
    if (!ch_match(ts, CHTokLBrace)) return;
    while (ch_peek(ts)->kind != CHTokRBrace && ch_peek(ts)->kind != CHTokEof) {
        if (ch_peek(ts)->kind != CHTokIdent) { ts->pos++; continue; }
        c_enum_variant_t var;
        memset(&var, 0, sizeof(var));
        var.name = ch_strdup(ch_peek(ts)->text, strlen(ch_peek(ts)->text));
        ts->pos++;
        var.value = next_val;
        if (ch_match(ts, CHTokAssign)) {
            if (ch_peek(ts)->kind == CHTokNumber) {
                boolean_t ok = False;
                var.value = ch_parse_int_literal(ch_peek(ts)->text, &ok);
                if (ok) next_val = var.value;
                ts->pos++;
            } else {
                while (ch_peek(ts)->kind != CHTokComma && ch_peek(ts)->kind != CHTokRBrace
                        && ch_peek(ts)->kind != CHTokEof)
                    ts->pos++;
            }
        }
        if (out->variant_count == 0) {
            out->variants_heap = allocate(8, sizeof(c_enum_variant_t));
            out->variants = out->variants_heap.pointer;
        } else if ((out->variant_count & (out->variant_count - 1)) == 0) {
            usize_t cap = out->variant_count < 8 ? 8 : out->variant_count;
            out->variants_heap = reallocate(out->variants_heap, (cap * 2) * sizeof(c_enum_variant_t));
            out->variants = out->variants_heap.pointer;
        }
        out->variants[out->variant_count++] = var;
        next_val = var.value + 1;
        ch_match(ts, CHTokComma);
    }
    ch_match(ts, CHTokRBrace);
}

static c_field_t ch_parse_field(ch_ctx_t *ctx, ch_tok_stream_t *ts);

static void ch_parse_struct_body(ch_ctx_t *ctx, ch_tok_stream_t *ts, c_struct_t *out) {
    if (!ch_match(ts, CHTokLBrace)) return;
    while (ch_peek(ts)->kind != CHTokRBrace && ch_peek(ts)->kind != CHTokEof) {
        c_field_t f = ch_parse_field(ctx, ts);
        if (f.name) {
            if (out->field_count == 0) {
                out->fields_heap = allocate(8, sizeof(c_field_t));
                out->fields = out->fields_heap.pointer;
            } else if ((out->field_count & (out->field_count - 1)) == 0) {
                usize_t cap = out->field_count < 8 ? 8 : out->field_count;
                out->fields_heap = reallocate(out->fields_heap, (cap * 2) * sizeof(c_field_t));
                out->fields = out->fields_heap.pointer;
            }
            out->fields[out->field_count++] = f;
        } else {
            while (ch_peek(ts)->kind != CHTokSemicolon && ch_peek(ts)->kind != CHTokRBrace
                    && ch_peek(ts)->kind != CHTokEof)
                ts->pos++;
            ch_match(ts, CHTokSemicolon);
        }
    }
    ch_match(ts, CHTokRBrace);
}

static c_type_t ch_parse_declspec(ch_ctx_t *ctx, ch_tok_stream_t *ts,
                                  boolean_t *is_typedef, boolean_t *is_const,
                                  char **pending_inline_name,
                                  c_struct_t *inline_struct_out,
                                  c_enum_t *inline_enum_out) {
    c_type_t t = ch_type_make(CTypeUnsupported);
    boolean_t seen_unsigned = False, seen_signed = False;
    int long_count = 0;
    *is_typedef = False;
    *is_const = False;
    *pending_inline_name = Null;
    memset(inline_struct_out, 0, sizeof(*inline_struct_out));
    memset(inline_enum_out, 0, sizeof(*inline_enum_out));

    while (ch_peek(ts)->kind == CHTokIdent) {
        const char *s = ch_peek(ts)->text;
        if (strcmp(s, "typedef") == 0) { *is_typedef = True; ts->pos++; continue; }
        if (strcmp(s, "extern") == 0 || strcmp(s, "static") == 0
                || strcmp(s, "inline") == 0 || strcmp(s, "__inline__") == 0) {
            ts->pos++;
            continue;
        }
        if (strcmp(s, "const") == 0) { *is_const = True; ts->pos++; continue; }
        if (strcmp(s, "volatile") == 0 || strcmp(s, "register") == 0
                || strcmp(s, "restrict") == 0 || strcmp(s, "__restrict") == 0
                || strcmp(s, "__restrict__") == 0
                || strcmp(s, "_Atomic") == 0 || strcmp(s, "_Nullable") == 0
                || strcmp(s, "_Nonnull") == 0 || strcmp(s, "_Null_unspecified") == 0) {
            ts->pos++; continue;
        }
        if (strcmp(s, "_Bool") == 0 || strcmp(s, "bool") == 0) {
            t.kind = CTypeBool; ts->pos++; continue;
        }
        if (strcmp(s, "unsigned") == 0) { seen_unsigned = True; ts->pos++; continue; }
        if (strcmp(s, "signed") == 0) { seen_signed = True; ts->pos++; continue; }
        if (strcmp(s, "long") == 0) { long_count++; ts->pos++; continue; }
        if (strcmp(s, "short") == 0) { t.kind = seen_unsigned ? CTypeUShort : CTypeShort; ts->pos++; continue; }
        if (strcmp(s, "char") == 0) {
            t.kind = seen_unsigned ? CTypeUChar : (seen_signed ? CTypeSChar : CTypeChar);
            ts->pos++;
            continue;
        }
        if (strcmp(s, "int") == 0) {
            if (long_count >= 2) t.kind = seen_unsigned ? CTypeULongLong : CTypeLongLong;
            else if (long_count == 1) t.kind = seen_unsigned ? CTypeULong : CTypeLong;
            else t.kind = seen_unsigned ? CTypeUInt : CTypeInt;
            ts->pos++;
            continue;
        }
        if (strcmp(s, "float") == 0) { t.kind = CTypeFloat; ts->pos++; continue; }
        if (strcmp(s, "double") == 0) {
            /* `long double` is an 80-/128-bit ABI type that Stasha can't
             * lower to LLVM IR portably yet — flag it distinctly so the
             * mapping layer in main.c can emit a precise error instead
             * of silently degrading to `double`. */
            t.kind = (long_count > 0) ? CTypeLongDouble : CTypeDouble;
            ts->pos++;
            continue;
        }
        if (strcmp(s, "void") == 0) { t.kind = CTypeVoid; ts->pos++; continue; }
        if (strcmp(s, "struct") == 0 || strcmp(s, "union") == 0) {
            boolean_t is_union = strcmp(s, "union") == 0;
            ts->pos++;
            char *tag = Null;
            if (ch_peek(ts)->kind == CHTokIdent) {
                tag = ch_strdup(ch_peek(ts)->text, strlen(ch_peek(ts)->text));
                ts->pos++;
            }
            if (ch_peek(ts)->kind == CHTokLBrace) {
                c_struct_t st;
                memset(&st, 0, sizeof(st));
                st.name = tag ? ch_strdup(tag, strlen(tag)) : Null;
                st.is_union = is_union;
                ch_parse_struct_body(ctx, ts, &st);
                *inline_struct_out = st;
                *pending_inline_name = tag;
            }
            t.kind = is_union ? CTypeUnionRef : CTypeStructRef;
            t.name = tag;
            break;
        }
        if (strcmp(s, "enum") == 0) {
            ts->pos++;
            char *tag = Null;
            if (ch_peek(ts)->kind == CHTokIdent) {
                tag = ch_strdup(ch_peek(ts)->text, strlen(ch_peek(ts)->text));
                ts->pos++;
            }
            if (ch_peek(ts)->kind == CHTokLBrace) {
                c_enum_t en;
                memset(&en, 0, sizeof(en));
                en.name = tag ? ch_strdup(tag, strlen(tag)) : Null;
                ch_parse_enum_body(ts, &en);
                *inline_enum_out = en;
                *pending_inline_name = tag;
            }
            t.kind = CTypeEnumRef;
            t.name = tag;
            break;
        }
        if (ch_is_type_name(ctx, s)) {
            t.kind = CTypeTypedefRef;
            t.name = ch_strdup(s, strlen(s));
            ts->pos++;
            break;
        }
        break;
    }

    if (t.kind == CTypeUnsupported && long_count > 0)
        t.kind = seen_unsigned ? CTypeULong : CTypeLong;
    t.is_const = *is_const;
    return t;
}

typedef struct {
    char *name;
    c_type_t type;
    c_param_t *params;
    usize_t param_count;
    boolean_t is_fn;
    boolean_t is_variadic;
    heap_t params_heap;
} ch_decl_t;

static ch_decl_t ch_parse_declarator(ch_ctx_t *ctx, ch_tok_stream_t *ts, c_type_t base) {
    ch_decl_t d;
    memset(&d, 0, sizeof(d));
    d.type = ch_type_clone(&base);

    while (ch_match(ts, CHTokStar)) {
        c_type_t ptr = ch_type_make(CTypePointer);
        heap_t h = allocate(1, sizeof(c_type_t));
        ch_track_heap(h);
        ptr.elem = h.pointer;
        *ptr.elem = d.type;
        /* Consume zero-or-more pointer-level qualifiers: const, volatile,
         * and the various restrict spellings.  Only `const` affects the
         * Stasha pointer permission; the rest are accepted and dropped. */
        while (ch_peek(ts)->kind == CHTokIdent) {
            const char *q = ch_peek(ts)->text;
            if (strcmp(q, "const") == 0) {
                ptr.is_const = True;
                ts->pos++;
                continue;
            }
            if (strcmp(q, "volatile") == 0
                    || strcmp(q, "restrict") == 0
                    || strcmp(q, "__restrict") == 0
                    || strcmp(q, "__restrict__") == 0
                    || strcmp(q, "_Atomic") == 0
                    || strcmp(q, "_Nullable") == 0
                    || strcmp(q, "_Nonnull") == 0
                    || strcmp(q, "_Null_unspecified") == 0) {
                ts->pos++;
                continue;
            }
            break;
        }
        d.type = ptr;
    }

    if (ch_match(ts, CHTokLParen)) {
        if (ch_match(ts, CHTokStar)) {
            d.type.kind = CTypeUnsupported;
            while (!ch_match(ts, CHTokRParen) && ch_peek(ts)->kind != CHTokEof) ts->pos++;
            return d;
        }
        ts->pos--;
    }

    if (ch_peek(ts)->kind == CHTokIdent) {
        d.name = ch_strdup(ch_peek(ts)->text, strlen(ch_peek(ts)->text));
        ts->pos++;
    }

    while (1) {
        if (ch_match(ts, CHTokLBracket)) {
            long len = 0;
            if (ch_peek(ts)->kind == CHTokNumber) {
                boolean_t ok = False;
                len = ch_parse_int_literal(ch_peek(ts)->text, &ok);
                ts->pos++;
            }
            ch_match(ts, CHTokRBracket);
            c_type_t arr = ch_type_make(CTypeArray);
            arr.array_len = len;
            heap_t h = allocate(1, sizeof(c_type_t));
            ch_track_heap(h);
            arr.elem = h.pointer;
            *arr.elem = d.type;
            d.type = arr;
            continue;
        }
        if (ch_match(ts, CHTokLParen)) {
            d.is_fn = True;
            /* `(void)` means zero params.  But `(void *p, …)` is a real
             * pointer-to-void parameter — only treat the bare `void` form
             * as the empty-params sentinel. */
            if (ch_peek(ts)->kind == CHTokIdent && strcmp(ch_peek(ts)->text, "void") == 0
                    && ts->pos + 1 < ts->count
                    && ts->toks[ts->pos + 1].kind == CHTokRParen) {
                ts->pos++;
                ch_match(ts, CHTokRParen);
                break;
            }
            while (ch_peek(ts)->kind != CHTokRParen && ch_peek(ts)->kind != CHTokEof) {
                if (ch_match(ts, CHTokEllipsis)) {
                    d.is_variadic = True;
                    break;
                }
                c_type_t pbase = ch_parse_type_name(ctx, ts);
                ch_decl_t pd = ch_parse_declarator(ctx, ts, pbase);
                if (pd.name || pd.type.kind != CTypeUnsupported) {
                    if (d.param_count == 0) {
                        d.params_heap = allocate(8, sizeof(c_param_t));
                        d.params = d.params_heap.pointer;
                    } else if ((d.param_count & (d.param_count - 1)) == 0) {
                        usize_t cap = d.param_count < 8 ? 8 : d.param_count;
                        d.params_heap = reallocate(d.params_heap, (cap * 2) * sizeof(c_param_t));
                        d.params = d.params_heap.pointer;
                    }
                    d.params[d.param_count].name = pd.name ? pd.name : ch_strdup("arg", 3);
                    d.params[d.param_count].type = pd.type;
                    d.param_count++;
                }
                if (!ch_match(ts, CHTokComma)) break;
            }
            ch_match(ts, CHTokRParen);
            continue;
        }
        break;
    }
    return d;
}

static c_field_t ch_parse_field(ch_ctx_t *ctx, ch_tok_stream_t *ts) {
    c_field_t f;
    memset(&f, 0, sizeof(f));
    boolean_t is_typedef = False, is_const = False;
    char *inline_name = Null;
    c_struct_t st; memset(&st, 0, sizeof(st));
    c_enum_t en; memset(&en, 0, sizeof(en));
    c_type_t base = ch_parse_declspec(ctx, ts, &is_typedef, &is_const, &inline_name, &st, &en);
    ch_decl_t d = ch_parse_declarator(ctx, ts, base);
    if (d.name) f.name = d.name;
    f.type = d.type;
    if (f.type.kind == CTypeArray && f.type.elem) f.array_len = f.type.array_len;
    /* Bitfield detection: `int flags : 3;` — record the colon and width
     * but don't treat as a normal field.  main.c later refuses any struct
     * containing bitfields, surfacing a precise error rather than silently
     * mis-laying out the struct. */
    if (ch_peek(ts)->kind == CHTokColon) {
        ts->pos++;
        f.is_bitfield = True;
        if (ch_peek(ts)->kind == CHTokNumber) {
            boolean_t ok = False;
            f.bit_width = ch_parse_int_literal(ch_peek(ts)->text, &ok);
            ts->pos++;
        }
    }
    ch_match(ts, CHTokSemicolon);
    if (st.name) ch_push_struct(ctx->out, st);
    if (en.name) ch_push_enum(ctx->out, en);
    return f;
}

static void ch_parse_top_decl(ch_ctx_t *ctx, ch_tok_stream_t *ts) {
    if (ch_is_ident(ts, "extern")) {
        ts->pos++;
        if (ch_peek(ts)->kind == CHTokString) {
            ts->pos++;
            if (ch_peek(ts)->kind == CHTokLBrace) {
                ch_match(ts, CHTokLBrace);
                return;
            }
        }
        if (ts->pos > 0) ts->pos--;
    }

    boolean_t is_typedef = False, is_const = False;
    char *inline_name = Null;
    c_struct_t st; memset(&st, 0, sizeof(st));
    c_enum_t en; memset(&en, 0, sizeof(en));
    c_type_t base = ch_parse_declspec(ctx, ts, &is_typedef, &is_const, &inline_name, &st, &en);

    if (ch_match(ts, CHTokSemicolon)) {
        if (st.name) ch_push_struct(ctx->out, st);
        if (en.name) ch_push_enum(ctx->out, en);
        return;
    }

    ch_decl_t decl = ch_parse_declarator(ctx, ts, base);

    if (decl.is_fn) {
        if (ch_peek(ts)->kind == CHTokLBrace) {
            ch_skip_balanced(ts, CHTokLBrace, CHTokRBrace);
        } else {
            ch_match(ts, CHTokSemicolon);
        }
        if (decl.name) {
            c_fn_t fn;
            memset(&fn, 0, sizeof(fn));
            fn.name = decl.name;
            fn.ret = decl.type;
            fn.params = decl.params;
            fn.param_count = decl.param_count;
            fn.is_variadic = decl.is_variadic;
            fn.params_heap = decl.params_heap;
            ch_push_fn(ctx->out, fn);
        }
    } else if (is_typedef && decl.name) {
        /* Adopt the typedef name as the struct/enum name *only* when the
         * underlying tag is anonymous AND the declarator didn't add any
         * indirection (no pointer/array layers).  Otherwise patterns like
         *   typedef struct OpaqueFoo *FooRef;   (explicit tag, pointer)
         *   typedef struct { … } *FooRef;       (anon, but pointer)
         * would lose the pointer and silently turn FooRef into a struct
         * type with empty body — corrupting opaque-handle FFI. */
        boolean_t has_tag      = (base.name != Null);
        boolean_t has_pointer  = (decl.type.kind == CTypePointer || decl.type.kind == CTypeArray);
        if (!st.name && !has_tag && !has_pointer
                && (base.kind == CTypeStructRef || base.kind == CTypeUnionRef)) {
            /* anonymous typedef struct/union { ... } name — adopt the typedef name */
            st.name = ch_strdup(decl.name, strlen(decl.name));
            ch_push_struct(ctx->out, st);
            base.name = ch_strdup(decl.name, strlen(decl.name));
            decl.type = base;
        } else if (!en.name && !has_tag && !has_pointer && base.kind == CTypeEnumRef) {
            /* anonymous typedef enum { ... } name — adopt the typedef name */
            en.name = ch_strdup(decl.name, strlen(decl.name));
            ch_push_enum(ctx->out, en);
            base.name = ch_strdup(decl.name, strlen(decl.name));
            decl.type = base;
        } else {
            if (st.name) ch_push_struct(ctx->out, st);
            if (en.name) ch_push_enum(ctx->out, en);
        }
        c_typedef_t td;
        memset(&td, 0, sizeof(td));
        td.name = decl.name;
        td.actual = decl.type;
        ch_push_typedef(ctx->out, td);
        ch_str_list_push(&ctx->type_names, td.name);
        while (ch_match(ts, CHTokComma)) {
            ch_decl_t extra = ch_parse_declarator(ctx, ts, base);
            if (extra.name) {
                c_typedef_t td2;
                memset(&td2, 0, sizeof(td2));
                td2.name = extra.name;
                td2.actual = extra.type;
                ch_push_typedef(ctx->out, td2);
                ch_str_list_push(&ctx->type_names, td2.name);
            }
        }
        ch_match(ts, CHTokSemicolon);
    } else {
        /* Top-level non-fn, non-typedef → treat as a global variable
         * declaration when the declarator produced a name + type that
         * map_c_type can lower.  Skip past any initializer expression
         * to the terminating semicolon. */
        if (decl.name && decl.type.kind != CTypeUnsupported) {
            c_global_t g;
            memset(&g, 0, sizeof(g));
            g.name = decl.name;
            g.type = decl.type;
            if (g.type.kind == CTypeArray) g.array_len = g.type.array_len;
            ch_push_global(ctx->out, g);
            /* Same line may declare multiple comma-separated names. */
            while (ch_peek(ts)->kind == CHTokComma) {
                ts->pos++;
                ch_decl_t extra = ch_parse_declarator(ctx, ts, base);
                if (extra.name && extra.type.kind != CTypeUnsupported) {
                    c_global_t g2;
                    memset(&g2, 0, sizeof(g2));
                    g2.name = extra.name;
                    g2.type = extra.type;
                    if (g2.type.kind == CTypeArray) g2.array_len = g2.type.array_len;
                    ch_push_global(ctx->out, g2);
                }
            }
        }
        while (ch_peek(ts)->kind != CHTokSemicolon && ch_peek(ts)->kind != CHTokEof) {
            if (ch_peek(ts)->kind == CHTokLBrace) ch_skip_balanced(ts, CHTokLBrace, CHTokRBrace);
            else ts->pos++;
        }
        ch_match(ts, CHTokSemicolon);
        if (st.name) ch_push_struct(ctx->out, st);
        if (en.name) ch_push_enum(ctx->out, en);
    }
}

static void ch_extract_define(cheader_result_t *out, const char *line) {
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '#') return;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "define", 6) != 0 || !isspace((unsigned char)p[6])) return;
    p += 6;
    while (*p == ' ' || *p == '\t') p++;
    const char *name_start = p;
    if (!isalpha((unsigned char)*p) && *p != '_') return;
    while (isalnum((unsigned char)*p) || *p == '_') p++;
    if (*p == '(') return;
    char *name = ch_strdup(name_start, (usize_t)(p - name_start));
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0') return;
    /* Strip a trailing line- or block-comment from the value: system
     * headers routinely append a description comment after the macro
     * body, and a comment leftover would cause ch_parse_int_literal to
     * refuse the value because parse_end is not at end-of-string. */
    char value_buf[256];
    usize_t vlen = 0;
    while (*p && vlen + 1 < sizeof(value_buf)) {
        if (*p == '/' && (p[1] == '/' || p[1] == '*')) break;
        value_buf[vlen++] = *p++;
    }
    value_buf[vlen] = '\0';
    /* Trim trailing whitespace on the captured slice. */
    while (vlen > 0 && (value_buf[vlen - 1] == ' '
                        || value_buf[vlen - 1] == '\t'
                        || value_buf[vlen - 1] == '\r')) {
        value_buf[--vlen] = '\0';
    }
    if (vlen == 0) return;
    boolean_t ok = False;
    long value = ch_parse_int_literal(value_buf, &ok);
    if (!ok) return;
    c_const_t cn;
    cn.name = name;
    cn.value = value;
    ch_push_const(out, cn);
}

static void ch_append_line(char **buf, heap_t *heap, usize_t *len, const char *line) {
    usize_t add = strlen(line);
    usize_t need = *len + add + 2;
    if (heap->pointer == Null) {
        *heap = allocate(need < 256 ? 256 : need, sizeof(char));
        *buf = heap->pointer;
        (*buf)[0] = '\0';
    } else if (need > heap->size) {
        usize_t new_size = heap->size * 2;
        if (new_size < need) new_size = need;
        *heap = reallocate(*heap, new_size);
        *buf = heap->pointer;
    }
    memcpy(*buf + *len, line, add);
    *len += add;
    (*buf)[(*len)++] = '\n';
    (*buf)[*len] = '\0';
}

static void ch_process_file(ch_ctx_t *ctx, const char *path);

static void ch_scan_source(ch_ctx_t *ctx, const char *path, char *source) {
    char *code = Null;
    heap_t code_heap = NullHeap;
    usize_t code_len = 0;

    char *p = source;
    while (*p) {
        char *line_start = p;
        while (*p && *p != '\n') p++;
        usize_t line_len = (usize_t)(p - line_start);
        char line[4096];
        if (line_len >= sizeof(line)) line_len = sizeof(line) - 1;
        memcpy(line, line_start, line_len);
        line[line_len] = '\0';
        if (*p == '\n') p++;

        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '#') {
            if (strncmp(s + 1, "include", 7) == 0 && isspace((unsigned char)s[8])) {
                char *q = s + 8;
                while (*q == ' ' || *q == '\t') q++;
                if (*q == '"' || *q == '<') {
                    boolean_t quoted = (*q == '"');
                    char endc = quoted ? '"' : '>';
                    q++;
                    char *end = strchr(q, endc);
                    if (end) {
                        char hdr[1024];
                        usize_t hlen = (usize_t)(end - q);
                        if (hlen >= sizeof(hdr)) hlen = sizeof(hdr) - 1;
                        memcpy(hdr, q, hlen);
                        hdr[hlen] = '\0';
                        char resolved[2048];
                        if (ch_resolve_header(ctx, path, hdr, quoted, resolved, sizeof(resolved))) {
                            boolean_t saved_top = ch_at_top_file;
                            ch_at_top_file = False;
                            ch_process_file(ctx, resolved);
                            ch_at_top_file = saved_top;
                        }
                    }
                }
            } else {
                ch_extract_define(ctx->out, s);
            }
            continue;
        }
        ch_append_line(&code, &code_heap, &code_len, line);
    }

    if (code) {
        ch_tok_stream_t ts;
        memset(&ts, 0, sizeof(ts));
        ch_tokenize_code(path, code, &ts);
        while (ch_peek(&ts)->kind != CHTokEof) {
            if (ch_match(&ts, CHTokRBrace)) continue;
            ch_parse_top_decl(ctx, &ts);
        }
        if (ts.heap.pointer) deallocate(ts.heap);
        deallocate(code_heap);
    }
}

static void ch_process_file(ch_ctx_t *ctx, const char *path) {
    if (ch_str_list_has(&ctx->seen_files, path)) return;
    ch_str_list_push(&ctx->seen_files, path);
    heap_t src_heap = NullHeap;
    char *src = ch_read_file(path, &src_heap);
    if (!src) return;
    ch_scan_source(ctx, path, src);
    deallocate(src_heap);
}

static boolean_t parse_cheader_file(const char *header_path,
                                    const char *search_dirs,
                                    const char *input_path,
                                    cheader_result_t *out,
                                    char *resolved_path_out,
                                    usize_t resolved_path_cap) {
    memset(out, 0, sizeof(*out));
    ch_active_result = out;
    ch_at_top_file = True;

    ch_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.out = out;
    ch_add_search_dirs(&ctx, input_path, search_dirs);

    /* Built-in typedef-like names that often appear before stdint.h is parsed. */
    ch_str_list_push(&ctx.type_names, "size_t");
    ch_str_list_push(&ctx.type_names, "ssize_t");
    ch_str_list_push(&ctx.type_names, "ptrdiff_t");
    ch_str_list_push(&ctx.type_names, "uintptr_t");
    ch_str_list_push(&ctx.type_names, "int8_t");
    ch_str_list_push(&ctx.type_names, "uint8_t");
    ch_str_list_push(&ctx.type_names, "int16_t");
    ch_str_list_push(&ctx.type_names, "uint16_t");
    ch_str_list_push(&ctx.type_names, "int32_t");
    ch_str_list_push(&ctx.type_names, "uint32_t");
    ch_str_list_push(&ctx.type_names, "int64_t");
    ch_str_list_push(&ctx.type_names, "uint64_t");

    char resolved[2048];
    if (!ch_resolve_header(&ctx, input_path, header_path,
                           header_path[0] != '<', resolved, sizeof(resolved)))
        return False;

    if (resolved_path_out && resolved_path_cap > 0)
        snprintf(resolved_path_out, resolved_path_cap, "%s", resolved);
    ch_process_file(&ctx, resolved);
    if (ctx.seen_files.heap.pointer) deallocate(ctx.seen_files.heap);
    if (ctx.type_names.heap.pointer) deallocate(ctx.type_names.heap);
    if (ctx.search_dirs.heap.pointer) deallocate(ctx.search_dirs.heap);
    ch_active_result = Null;
    return True;
}

static void free_cheader_result(cheader_result_t *result) {
    if (!result) return;
    for (usize_t i = 0; i < result->fn_count; i++) {
        c_fn_t *fn = &result->fns[i];
        if (fn->params_heap.pointer)
            deallocate(fn->params_heap);
        ch_type_free(&fn->ret);
    }
    for (usize_t i = 0; i < result->struct_count; i++) {
        c_struct_t *st = &result->structs[i];
        for (usize_t j = 0; j < st->field_count; j++)
            ch_type_free(&st->fields[j].type);
        if (st->fields_heap.pointer)
            deallocate(st->fields_heap);
    }
    for (usize_t i = 0; i < result->tdef_count; i++)
        ch_type_free(&result->tdefs[i].actual);
    for (usize_t i = 0; i < result->enum_count; i++)
        if (result->enums[i].variants_heap.pointer)
            deallocate(result->enums[i].variants_heap);
    for (usize_t i = 0; i < result->global_count; i++)
        ch_type_free(&result->globals[i].type);
    if (result->fns_heap.pointer) deallocate(result->fns_heap);
    if (result->structs_heap.pointer) deallocate(result->structs_heap);
    if (result->tdefs_heap.pointer) deallocate(result->tdefs_heap);
    if (result->enums_heap.pointer) deallocate(result->enums_heap);
    if (result->consts_heap.pointer) deallocate(result->consts_heap);
    if (result->globals_heap.pointer) deallocate(result->globals_heap);
    for (usize_t i = 0; i < result->owned_heap_count; i++)
        if (result->owned_heaps[i].pointer)
            deallocate(result->owned_heaps[i]);
    if (result->owned_heaps_heap.pointer)
        deallocate(result->owned_heaps_heap);
    memset(result, 0, sizeof(*result));
}
