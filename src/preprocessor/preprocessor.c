/*
 * preprocessor.c — Stasha token-based preprocessor.
 *
 * Architecture:
 *   Pass 1: lex all tokens from the source file, tagging each with file_path.
 *   Pass 2: scan the token array for macro definitions (int/ext macro fn/let).
 *   Pass 3: walk the token array, expand macro invocations, and build the
 *           output pp_stream_t.  Macro-expanded tokens carry:
 *             .tok.file/line/col  = origin inside the macro body definition
 *             .expansion          = linked list of call-site locations
 *
 * Source location is NEVER destroyed.  Errors from the parser or codegen
 * always point to the right file and line, even across macro expansions.
 *
 * Allocations: the preprocessor manages its own memory independently of
 * the AST arena, using plain stdlib allocators (common.h's ban is lifted
 * below).
 */
#include "preprocessor.h"
#include "../lexer/lexer.h"
#include "../common/common.h"

/*
 * Lift the project-wide stdlib allocator ban BEFORE including <stdlib.h>.
 * common.h defines `free(...)` as a macro; if that's still active when
 * stdlib.h is parsed, the system's `void free(void *)` declaration gets
 * textually substituted into garbage.  Undefining first lets the real
 * declarations through.
 */
#undef malloc
#undef realloc
#undef free

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ─────────────────────────────────────────────────────────────────────────────
   Raw token array (temporary; used only during preprocessing)
   ───────────────────────────────────────────────────────────────────────── */

typedef struct {
    token_t *toks;
    int      count;
    int      cap;
} raw_arr_t;

static void ra_init(raw_arr_t *a) {
    a->cap   = 256;
    a->count = 0;
    a->toks  = malloc((size_t)a->cap * sizeof(token_t));
}

static void ra_push(raw_arr_t *a, token_t t) {
    if (a->count >= a->cap) {
        a->cap *= 2;
        a->toks = realloc(a->toks, (size_t)a->cap * sizeof(token_t));
    }
    a->toks[a->count++] = t;
}

static void ra_free(raw_arr_t *a) {
    free(a->toks);
    a->toks  = NULL;
    a->count = 0;
    a->cap   = 0;
}

/* ─────────────────────────────────────────────────────────────────────────────
   Text intern pool (owns text for macro-expanded tokens)
   ───────────────────────────────────────────────────────────────────────── */

static void pool_init(pp_stream_t *s) {
    s->text_cap  = 4096;
    s->text_used = 0;
    s->text_pool = malloc((size_t)s->text_cap);
}

/* Intern `len` bytes from `text` into the pool.
   Returns a pointer into the pool (NUL-terminated). */
static const char *pool_intern(pp_stream_t *s, const char *text, usize_t len) {
    if (s->text_used + (int)len + 1 > s->text_cap) {
        s->text_cap = (s->text_used + (int)len + 1) * 2 + 256;
        s->text_pool = realloc(s->text_pool, (size_t)s->text_cap);
    }
    char *dest = s->text_pool + s->text_used;
    memcpy(dest, text, (size_t)len);
    dest[len] = '\0';
    s->text_used += (int)len + 1;
    return dest;
}

/* ─────────────────────────────────────────────────────────────────────────────
   Output stream operations
   ───────────────────────────────────────────────────────────────────────── */

static void stream_push_raw(pp_stream_t *s, pp_token_t t) {
    if (s->count >= s->cap) {
        s->cap = s->cap ? s->cap * 2 : 256;
        s->tokens = realloc(s->tokens, (size_t)s->cap * sizeof(pp_token_t));
    }
    s->tokens[s->count++] = t;
}

/*
 * emit_token — push one token to the output stream.
 *
 * If `expansion` is non-NULL (the token came from a macro body), its text
 * is interned into the pool so the output stream owns a stable copy,
 * independent of the original source buffer's lifetime.
 *
 * If `expansion` is NULL (a literal source token), `.start` is left as-is
 * (it points into the original source, which outlives the parse phase).
 */
static void emit_token(pp_stream_t *out, token_t tok, pp_expansion_t *expansion) {
    pp_token_t pt;
    pt.tok       = tok;
    pt.expansion = expansion;
    if (expansion && tok.start && tok.length > 0)
        pt.tok.start = pool_intern(out, tok.start, tok.length);
    stream_push_raw(out, pt);
}

/* ─────────────────────────────────────────────────────────────────────────────
   Macro set operations
   ───────────────────────────────────────────────────────────────────────── */

pp_macro_set_t *pp_macro_set_new(void) {
    pp_macro_set_t *s = malloc(sizeof(pp_macro_set_t));
    memset(s, 0, sizeof(*s));
    s->fn_cap  = 16;
    s->let_cap = 16;
    s->fns  = malloc((size_t)s->fn_cap  * sizeof(macro_fn_t));
    s->lets = malloc((size_t)s->let_cap * sizeof(macro_let_t));
    return s;
}

static macro_fn_t *ms_add_fn(pp_macro_set_t *s) {
    if (s->fn_count >= s->fn_cap) {
        s->fn_cap = s->fn_cap ? s->fn_cap * 2 : 16;
        s->fns = realloc(s->fns, (size_t)s->fn_cap * sizeof(macro_fn_t));
    }
    macro_fn_t *fn = &s->fns[s->fn_count++];
    memset(fn, 0, sizeof(*fn));
    return fn;
}

static macro_let_t *ms_add_let(pp_macro_set_t *s) {
    if (s->let_count >= s->let_cap) {
        s->let_cap = s->let_cap ? s->let_cap * 2 : 16;
        s->lets = realloc(s->lets, (size_t)s->let_cap * sizeof(macro_let_t));
    }
    macro_let_t *lt = &s->lets[s->let_count++];
    memset(lt, 0, sizeof(*lt));
    return lt;
}

void pp_macro_set_free(pp_macro_set_t *set) {
    if (!set) return;
    for (int i = 0; i < set->fn_count;  i++) free(set->fns[i].body);
    for (int i = 0; i < set->let_count; i++) free(set->lets[i].tokens);
    free(set->fns);
    free(set->lets);
    free(set);
}

/* ─────────────────────────────────────────────────────────────────────────────
   Macro lookup helpers
   ───────────────────────────────────────────────────────────────────────── */

static macro_fn_t *find_fn_in_set(pp_macro_set_t *s, const char *name) {
    for (int i = 0; i < s->fn_count; i++)
        if (strcmp(s->fns[i].name, name) == 0)
            return &s->fns[i];
    return NULL;
}

static macro_let_t *find_let_in_set(pp_macro_set_t *s, const char *name) {
    for (int i = 0; i < s->let_count; i++)
        if (strcmp(s->lets[i].name, name) == 0)
            return &s->lets[i];
    return NULL;
}

/*
 * Lookup a fn macro: check the local set first, then ext macros from imports.
 */
static macro_fn_t *lookup_fn(const char *name,
                               pp_macro_set_t  *local,
                               pp_macro_set_t **imports, int import_count) {
    macro_fn_t *f = find_fn_in_set(local, name);
    if (f) return f;
    for (int i = 0; i < import_count; i++) {
        f = find_fn_in_set(imports[i], name);
        if (f && f->vis == MacroVis_External) return f;
    }
    return NULL;
}

static macro_let_t *lookup_let(const char *name,
                                 pp_macro_set_t  *local,
                                 pp_macro_set_t **imports, int import_count) {
    macro_let_t *l = find_let_in_set(local, name);
    if (l) return l;
    for (int i = 0; i < import_count; i++) {
        l = find_let_in_set(imports[i], name);
        if (l && l->vis == MacroVis_External) return l;
    }
    return NULL;
}

/* ─────────────────────────────────────────────────────────────────────────────
   Helpers
   ───────────────────────────────────────────────────────────────────────── */

static void copy_tok_name(char *dst, size_t dsz, const token_t *t) {
    size_t n = (size_t)t->length < dsz - 1 ? (size_t)t->length : dsz - 1;
    memcpy(dst, t->start, n);
    dst[n] = '\0';
}

static macro_tok_t make_macro_tok(const token_t *t, const char *file_path) {
    macro_tok_t mt;
    mt.kind     = t->kind;
    mt.text     = t->start;
    mt.text_len = t->length;
    mt.line     = t->line;
    mt.col      = t->col;
    mt.file     = file_path;
    return mt;
}

static void mfn_push_body(macro_fn_t *fn, macro_tok_t mt) {
    if (fn->body_count >= fn->body_cap) {
        fn->body_cap = fn->body_cap ? fn->body_cap * 2 : 32;
        fn->body = realloc(fn->body, (size_t)fn->body_cap * sizeof(macro_tok_t));
    }
    fn->body[fn->body_count++] = mt;
}

static void mlt_push_tok(macro_let_t *lt, macro_tok_t mt) {
    if (lt->tok_count >= lt->tok_cap) {
        lt->tok_cap = lt->tok_cap ? lt->tok_cap * 2 : 16;
        lt->tokens = realloc(lt->tokens, (size_t)lt->tok_cap * sizeof(macro_tok_t));
    }
    lt->tokens[lt->tok_count++] = mt;
}

/* ─────────────────────────────────────────────────────────────────────────────
   Expansion chain allocation
   ───────────────────────────────────────────────────────────────────────── */

/*
 * new_expansion — allocate a new expansion node and link it into the
 * stream's exp_pool_head list so pp_stream_free() can release it exactly once.
 */
static pp_expansion_t *new_expansion(pp_stream_t    *out,
                                      const char     *macro_name,
                                      const char     *call_file,
                                      usize_t         call_line,
                                      usize_t         call_col,
                                      pp_expansion_t *parent) {
    pp_expansion_t *e = malloc(sizeof(pp_expansion_t));
    e->macro_name = macro_name;
    e->call_file  = call_file ? call_file : "<unknown>";
    e->call_line  = call_line;
    e->call_col   = call_col;
    e->parent     = parent;
    /* Register in the stream's pool so we can free it exactly once. */
    e->pool_next      = out->exp_pool_head;
    out->exp_pool_head = e;
    return e;
}

/* ─────────────────────────────────────────────────────────────────────────────
   Pass 1: scan raw token array for macro definitions
   ───────────────────────────────────────────────────────────────────────── */

/*
 * scan_fn_macro_body — shared body parser used by both scan_fn_macro and
 * scan_bare_fn_macro.
 *
 * Entry: a->toks[j] is the first token INSIDE the outer '{' (i.e. the '(')
 * Fills fn->params / fn->has_variadic / fn->variadic_name / fn->body.
 * Returns: index of first token after the closing outer '}'.
 */
static int scan_fn_macro_body(macro_fn_t *fn, const raw_arr_t *a, int j,
                               const char *file_path) {
    /* ( [@param ...] ) */
    if (j >= a->count || a->toks[j].kind != TokLParen) return j + 1;
    j++;

    while (j < a->count && a->toks[j].kind != TokRParen) {
        /* Variadic pack: ...@name */
        if (a->toks[j].kind == TokDotDotDot
                && j + 2 < a->count
                && a->toks[j+1].kind == TokAt
                && a->toks[j+2].kind == TokIdent) {
            fn->has_variadic = 1;
            copy_tok_name(fn->variadic_name, 64, &a->toks[j+2]);
            j += 3; /* skip ... @ name */
        } else if (a->toks[j].kind == TokAt
                && j + 1 < a->count
                && a->toks[j+1].kind == TokIdent
                && fn->param_count < PP_MAX_PARAMS) {
            copy_tok_name(fn->params[fn->param_count++], 64, &a->toks[j+1]);
            j += 2;
        } else if (a->toks[j].kind == TokComma) {
            j++;
        } else {
            j++;
        }
    }
    if (j >= a->count) return j + 1;
    j++; /* skip ) */

    /* => */
    if (j >= a->count || a->toks[j].kind != TokFatArrow) return j + 1;
    j++;

    /* { body } */
    if (j >= a->count || a->toks[j].kind != TokLBrace) return j + 1;
    j++;

    int depth = 1;
    while (j < a->count) {
        if (a->toks[j].kind == TokLBrace)       depth++;
        else if (a->toks[j].kind == TokRBrace) {
            if (--depth == 0) { j++; break; }
        }
        if (depth > 0)
            mfn_push_body(fn, make_macro_tok(&a->toks[j], file_path));
        j++;
    }

    /* skip optional ';' then closing '}' of the outer brace */
    while (j < a->count && a->toks[j].kind == TokSemicolon) j++;
    if (j < a->count && a->toks[j].kind == TokRBrace) j++;

    return j;
}

/*
 * scan_fn_macro — parse:
 *   fn name ! { ( [@param ...] ) => { body } ; }
 *
 * Entry:  a->toks[i] == TokFn
 * Returns: index of first token after the closing '}'
 */
static int scan_fn_macro(pp_macro_set_t *ms, const raw_arr_t *a, int i,
                          macro_vis_t vis, const char *file_path) {
    /* fn name ! { */
    if (i + 3 >= a->count)                    return i + 1;
    if (a->toks[i].kind     != TokFn)         return i + 1;
    if (a->toks[i+1].kind   != TokIdent)      return i + 1;
    if (a->toks[i+2].kind   != TokBang)       return i + 1;
    if (a->toks[i+3].kind   != TokLBrace)     return i + 1;

    macro_fn_t *fn = ms_add_fn(ms);
    fn->vis          = vis;
    fn->defined_file = file_path;
    copy_tok_name(fn->name, sizeof(fn->name), &a->toks[i+1]);

    int j = i + 4;  /* first token inside outer { */

    return scan_fn_macro_body(fn, a, j, file_path);
}

/*
 * scan_bare_fn_macro — parse:
 *   name ! { ( [@param ...] ) => { body } }
 *
 * Entry:  a->toks[i] == TokIdent (macro name, no leading 'fn' keyword)
 * Returns: index of first token after the closing '}'
 */
static int scan_bare_fn_macro(pp_macro_set_t *ms, const raw_arr_t *a, int i,
                               macro_vis_t vis, const char *file_path) {
    /* name ! { */
    if (i + 2 >= a->count)                  return i + 1;
    if (a->toks[i].kind   != TokIdent)      return i + 1;
    if (a->toks[i+1].kind != TokBang)       return i + 1;
    if (a->toks[i+2].kind != TokLBrace)     return i + 1;

    macro_fn_t *fn = ms_add_fn(ms);
    fn->vis          = vis;
    fn->defined_file = file_path;
    copy_tok_name(fn->name, sizeof(fn->name), &a->toks[i]);

    int j = i + 3;  /* first token inside outer { */
    return scan_fn_macro_body(fn, a, j, file_path);
}

/*
 * scan_let_macro — parse:
 *   let name ! = tokens... ;
 *
 * Entry:  a->toks[i] == TokLet
 */
static int scan_let_macro(pp_macro_set_t *ms, const raw_arr_t *a, int i,
                           macro_vis_t vis, const char *file_path) {
    if (i + 3 >= a->count)                return i + 1;
    if (a->toks[i].kind   != TokLet)      return i + 1;
    if (a->toks[i+1].kind != TokIdent)    return i + 1;
    if (a->toks[i+2].kind != TokBang)     return i + 1;
    if (a->toks[i+3].kind != TokEq)       return i + 1;

    macro_let_t *lt = ms_add_let(ms);
    lt->vis          = vis;
    lt->defined_file = file_path;
    copy_tok_name(lt->name, sizeof(lt->name), &a->toks[i+1]);

    int j = i + 4;
    while (j < a->count && a->toks[j].kind != TokSemicolon)
        mlt_push_tok(lt, make_macro_tok(&a->toks[j++], file_path));
    if (j < a->count) j++; /* skip ; */

    return j;
}

/*
 * scan_macro_defs — pass 1.
 *
 * Walks the raw token array, finds all:
 *   [int|ext] macro fn  name! { ... }
 *   [int|ext] macro let name! = ... ;
 *
 * and registers them in the macro set `ms`.
 */
static void scan_macro_defs(pp_macro_set_t *ms, const raw_arr_t *a,
                             const char *file_path) {
    for (int i = 0; i < a->count; ) {
        token_kind_t k = a->toks[i].kind;

        /* [int|ext] macro fn|let */
        if ((k == TokInt || k == TokExt)
                && i + 1 < a->count
                && a->toks[i+1].kind == TokMacro) {

            macro_vis_t vis = (k == TokExt) ? MacroVis_External : MacroVis_Internal;
            i += 2; /* skip int/ext + macro */

            if (i < a->count) {
                if (a->toks[i].kind == TokFn) {
                    i = scan_fn_macro(ms, a, i, vis, file_path);
                    continue;
                }
                if (a->toks[i].kind == TokLet) {
                    i = scan_let_macro(ms, a, i, vis, file_path);
                    continue;
                }
                /* Bare form: macro name! { ... } (no fn/let keyword) */
                if (a->toks[i].kind == TokIdent
                        && i + 1 < a->count && a->toks[i+1].kind == TokBang
                        && i + 2 < a->count && a->toks[i+2].kind == TokLBrace) {
                    i = scan_bare_fn_macro(ms, a, i, vis, file_path);
                    continue;
                }
            }
            continue;
        }
        i++;
    }
}

/* ─────────────────────────────────────────────────────────────────────────────
   Pass 2: expand tokens into output stream
   ───────────────────────────────────────────────────────────────────────── */

/* Recursion guard: track which macros are currently being expanded. */
static const char *g_expanding[PP_MAX_EXPAND_DEPTH];
static int         g_expand_depth = 0;

static int is_expanding(const char *name) {
    for (int i = 0; i < g_expand_depth; i++)
        if (strcmp(g_expanding[i], name) == 0) return 1;
    return 0;
}

/* Argument set: comma-separated groups of tokens from a call site. */
typedef struct {
    token_t *toks;
    int      total;
    int      cap;
    int      starts[PP_MAX_PARAMS]; /* start index of each argument */
    int      ends[PP_MAX_PARAMS];   /* one-past-end index            */
    int      count;                 /* number of arguments           */
} arg_set_t;

static void as_init(arg_set_t *as) {
    as->cap   = 64;
    as->total = 0;
    as->count = 0;
    as->toks  = malloc((size_t)as->cap * sizeof(token_t));
}

static void as_push(arg_set_t *as, token_t t) {
    if (as->total >= as->cap) {
        as->cap *= 2;
        as->toks = realloc(as->toks, (size_t)as->cap * sizeof(token_t));
    }
    as->toks[as->total++] = t;
}

static void as_free(arg_set_t *as) {
    free(as->toks);
    as->toks  = NULL;
    as->total = as->cap = as->count = 0;
}

/*
 * collect_args — collect tokens from a function-macro call site.
 *
 * Entry: a->toks[j] is the first token AFTER the opening '('.
 * Returns the index of the first token after the closing ')'.
 *
 * Builds the arg_set_t with one group per comma-separated argument.
 * Nested parentheses / brackets / braces are respected.
 */
static int collect_args(const raw_arr_t *a, int j, arg_set_t *as) {
    as_init(as);
    as->count    = 0;
    as->starts[0] = 0;

    int depth = 0;
    while (j < a->count) {
        token_kind_t k = a->toks[j].kind;
        if (k == TokRParen && depth == 0) break;

        if (k == TokLParen || k == TokLBrace || k == TokLBracket)  depth++;
        else if (k == TokRBrace || k == TokRBracket)                depth--;
        else if (k == TokRParen)                                     depth--;
        else if (k == TokComma && depth == 0) {
            as->ends[as->count]    = as->total;
            as->count++;
            as->starts[as->count] = as->total;
            j++;
            continue;
        }

        as_push(as, a->toks[j]);
        j++;
    }

    as->ends[as->count] = as->total;
    as->count++;
    if (j < a->count && a->toks[j].kind == TokRParen) j++;
    return j;
}

/* Forward declaration (expand_fn_macro and expand_stream are mutually recursive). */
static void expand_stream(const raw_arr_t *src, int start, int end,
                           pp_macro_set_t  *local,
                           pp_macro_set_t **imports, int import_count,
                           pp_stream_t     *out,
                           pp_expansion_t  *parent_exp);

/*
 * macro_tok_name_eq — return 1 if macro_tok_t text equals `s`.
 */
static int macro_tok_name_eq(const macro_tok_t *mt, const char *s) {
    usize_t slen = (usize_t)strlen(s);
    return mt->text_len == slen && memcmp(mt->text, s, (size_t)slen) == 0;
}

/*
 * copy_macro_tok_name — copy macro_tok_t text into dst (NUL-terminated).
 */
static void copy_macro_tok_name(char *dst, size_t dsz, const macro_tok_t *mt) {
    size_t n = mt->text_len < dsz - 1 ? mt->text_len : dsz - 1;
    memcpy(dst, mt->text, n);
    dst[n] = '\0';
}

/*
 * macro_tok_to_token — convert a stored macro body token to a token_t.
 */
static token_t macro_tok_to_token(const macro_tok_t *mt) {
    token_t tok;
    tok.kind   = mt->kind;
    tok.start  = mt->text;
    tok.length = mt->text_len;
    tok.line   = mt->line;
    tok.col    = mt->col;
    tok.file   = mt->file;
    return tok;
}

/*
 * expand_fn_macro — substitute parameters and recursively expand a fn macro body.
 *
 * Stage 1: walk body tokens.
 *   - @foreach loop_var : variadic_name { body } → unroll over variadic args.
 *   - @param_name → splice corresponding named argument tokens.
 *   - @variadic_name → splice ALL variadic args (comma-separated).
 *   - Everything else passes through verbatim.
 *   Builds a temporary raw_arr_t of substituted tokens.
 *
 * Stage 2: call expand_stream on the substituted array so nested macro
 *          invocations inside the body are also expanded.
 */
static void expand_fn_macro(macro_fn_t    *fn,
                              arg_set_t     *args,
                              pp_macro_set_t *local,
                              pp_macro_set_t **imports, int import_count,
                              pp_stream_t    *out,
                              pp_expansion_t *call_exp) {
    /* Stage 1: parameter substitution. */
    raw_arr_t substituted;
    ra_init(&substituted);

    int i = 0;
    while (i < fn->body_count) {
        macro_tok_t *mt = &fn->body[i];

        /* ── @foreach loop_var : variadic_name { body } ── */
        if (mt->kind == TokAt
                && i + 5 < fn->body_count
                && fn->body[i+1].kind == TokIdent
                && macro_tok_name_eq(&fn->body[i+1], "foreach")
                && fn->body[i+2].kind == TokIdent   /* loop variable  */
                && fn->body[i+3].kind == TokColon
                && fn->body[i+4].kind == TokIdent   /* variadic name  */
                && fn->body[i+5].kind == TokLBrace
                && fn->has_variadic) {

            char loop_var[64], var_name[64];
            copy_macro_tok_name(loop_var,  sizeof(loop_var),  &fn->body[i+2]);
            copy_macro_tok_name(var_name,  sizeof(var_name),  &fn->body[i+4]);

            if (strcmp(var_name, fn->variadic_name) == 0) {
                /* Find the matching closing brace for the loop body. */
                int body_start = i + 6;
                int depth = 1;
                int j = body_start;
                while (j < fn->body_count && depth > 0) {
                    if      (fn->body[j].kind == TokLBrace)  depth++;
                    else if (fn->body[j].kind == TokRBrace)  depth--;
                    if (depth > 0) j++;
                    else break;
                }
                int body_end = j; /* index of the closing '}' */

                /* Emit the loop body once per variadic argument. */
                int var_start = fn->param_count; /* first variadic arg index */
                for (int vi = var_start; vi < args->count; vi++) {
                    for (int bi = body_start; bi < body_end; ) {
                        macro_tok_t *bmt = &fn->body[bi];

                        /*
                         * Substitute the loop variable when used as a plain
                         * identifier (the `@` prefix is only in the header).
                         * Also accept `@loop_var` for symmetry with named params.
                         */
                        if (bmt->kind == TokAt
                                && bi + 1 < body_end
                                && fn->body[bi+1].kind == TokIdent) {
                            char bname[64];
                            copy_macro_tok_name(bname, sizeof(bname), &fn->body[bi+1]);
                            if (strcmp(bname, loop_var) == 0) {
                                for (int k = args->starts[vi]; k < args->ends[vi]; k++)
                                    ra_push(&substituted, args->toks[k]);
                                bi += 2; /* skip @ + ident */
                                continue;
                            }
                        }
                        if (bmt->kind == TokIdent) {
                            char bname[64];
                            copy_macro_tok_name(bname, sizeof(bname), bmt);
                            if (strcmp(bname, loop_var) == 0) {
                                for (int k = args->starts[vi]; k < args->ends[vi]; k++)
                                    ra_push(&substituted, args->toks[k]);
                                bi++;
                                continue;
                            }
                        }

                        ra_push(&substituted, macro_tok_to_token(bmt));
                        bi++;
                    }
                }

                i = body_end + 1; /* skip past the closing '}' */
                continue;
            }
        }

        /* ── @param_name substitution ── */
        if (mt->kind == TokAt
                && i + 1 < fn->body_count
                && fn->body[i+1].kind == TokIdent) {

            char pname[64];
            usize_t plen = fn->body[i+1].text_len < 63
                         ? fn->body[i+1].text_len : 63;
            memcpy(pname, fn->body[i+1].text, (size_t)plen);
            pname[plen] = '\0';

            /* Named parameter substitution. */
            int pidx = -1;
            for (int p = 0; p < fn->param_count; p++)
                if (strcmp(fn->params[p], pname) == 0) { pidx = p; break; }

            if (pidx >= 0 && pidx < args->count) {
                for (int k = args->starts[pidx]; k < args->ends[pidx]; k++)
                    ra_push(&substituted, args->toks[k]);
                i += 2;
                continue;
            }

            /* Variadic pack substitution: @args → all variadic args, comma-joined. */
            if (fn->has_variadic && strcmp(fn->variadic_name, pname) == 0) {
                for (int vi = fn->param_count; vi < args->count; vi++) {
                    if (vi > fn->param_count) {
                        token_t comma;
                        comma.kind   = TokComma;
                        comma.start  = ",";
                        comma.length = 1;
                        comma.line   = mt->line;
                        comma.col    = mt->col;
                        comma.file   = mt->file;
                        ra_push(&substituted, comma);
                    }
                    for (int k = args->starts[vi]; k < args->ends[vi]; k++)
                        ra_push(&substituted, args->toks[k]);
                }
                i += 2;
                continue;
            }
        }

        /* ── Regular body token ── */
        ra_push(&substituted, macro_tok_to_token(mt));
        i++;
    }

    /* Stage 2: recursively expand the substituted body. */
    expand_stream(&substituted, 0, substituted.count,
                  local, imports, import_count, out, call_exp);

    ra_free(&substituted);
}

/*
 * skip_macro_def — skip over a complete macro definition in the token stream.
 *
 * Entry: a->toks[i] == TokFn or TokLet (the token after `[int|ext] macro`).
 * Returns: index of first token after the definition.
 */
/*
 * skip_brace_block — consume a '{'-delimited block, returning the index
 * after the matching '}'.  Entry: i points at the '{'.
 */
static int skip_brace_block(const raw_arr_t *a, int i) {
    if (i >= a->count || a->toks[i].kind != TokLBrace) return i + 1;
    i++; /* consume '{' */
    int depth = 1;
    while (i < a->count && depth > 0) {
        if      (a->toks[i].kind == TokLBrace)  depth++;
        else if (a->toks[i].kind == TokRBrace)  depth--;
        i++;
    }
    return i;
}

static int skip_macro_def(const raw_arr_t *a, int i) {
    if (i >= a->count) return i;

    if (a->toks[i].kind == TokFn) {
        /* fn name ! { ... } */
        i += 3; /* skip fn name ! — now at the outer '{' */
        return skip_brace_block(a, i);
    }

    if (a->toks[i].kind == TokLet) {
        /* let name ! = ... ; */
        i += 4; /* skip let name ! = */
        while (i < a->count && a->toks[i].kind != TokSemicolon) i++;
        if (i < a->count) i++; /* skip ; */
        return i;
    }

    if (a->toks[i].kind == TokIdent
            && i + 1 < a->count && a->toks[i+1].kind == TokBang
            && i + 2 < a->count && a->toks[i+2].kind == TokLBrace) {
        /* Bare form: name ! { ... } (no leading fn/let keyword) */
        i += 2; /* skip name ! — now at the outer '{' */
        return skip_brace_block(a, i);
    }

    return i + 1;
}

/*
 * expand_stream — main expansion pass (pass 2).
 *
 * Walks src[start..end), emits non-macro tokens verbatim, and expands
 * macro invocations into `out`.  `parent_exp` is the expansion chain
 * inherited from an enclosing expansion (NULL at the top level).
 */
static void expand_stream(const raw_arr_t *src, int start, int end,
                           pp_macro_set_t  *local,
                           pp_macro_set_t **imports, int import_count,
                           pp_stream_t     *out,
                           pp_expansion_t  *parent_exp) {
    int i = start;
    while (i < end) {
        const token_t *t = &src->toks[i];
        token_kind_t   k = t->kind;

        /* ── Skip macro definitions (already registered in pass 1) ── */
        if ((k == TokInt || k == TokExt)
                && i + 1 < end
                && src->toks[i+1].kind == TokMacro) {
            i = skip_macro_def(src, i + 2);
            continue;
        }

        /* ── namespaced fn macro: qualifier . name . ! ( args ) ── */
        if (k == TokIdent
                && i + 5 < end
                && src->toks[i+1].kind == TokDot
                && src->toks[i+2].kind == TokIdent
                && src->toks[i+3].kind == TokDot
                && src->toks[i+4].kind == TokBang
                && src->toks[i+5].kind == TokLParen) {

            char name[64];
            copy_tok_name(name, sizeof(name), &src->toks[i+2]);

            macro_fn_t *fn = lookup_fn(name, local, imports, import_count);
            if (fn && !is_expanding(fn->name)) {
                pp_expansion_t *call_exp = new_expansion(
                    out, fn->name, t->file, t->line, t->col, parent_exp);

                arg_set_t args;
                int j = collect_args(src, i + 6, &args);

                if (g_expand_depth < PP_MAX_EXPAND_DEPTH)
                    g_expanding[g_expand_depth++] = fn->name;

                expand_fn_macro(fn, &args, local, imports, import_count,
                                out, call_exp);

                if (g_expand_depth > 0) g_expand_depth--;
                as_free(&args);

                if (j < end && src->toks[j].kind == TokSemicolon) j++;
                i = j;
                continue;
            }
        }

        /* ── namespaced let macro: qualifier . name . ! ── */
        if (k == TokIdent
                && i + 4 < end
                && src->toks[i+1].kind == TokDot
                && src->toks[i+2].kind == TokIdent
                && src->toks[i+3].kind == TokDot
                && src->toks[i+4].kind == TokBang
                && (i + 5 >= end || src->toks[i+5].kind != TokLParen)) {

            char name[64];
            copy_tok_name(name, sizeof(name), &src->toks[i+2]);

            macro_let_t *lt = lookup_let(name, local, imports, import_count);
            if (lt && !is_expanding(lt->name)) {
                pp_expansion_t *call_exp = new_expansion(
                    out, lt->name, t->file, t->line, t->col, parent_exp);

                if (g_expand_depth < PP_MAX_EXPAND_DEPTH)
                    g_expanding[g_expand_depth++] = lt->name;

                for (int bi = 0; bi < lt->tok_count; bi++) {
                    macro_tok_t *mt = &lt->tokens[bi];
                    emit_token(out, macro_tok_to_token(mt), call_exp);
                }

                if (g_expand_depth > 0) g_expand_depth--;
                i += 5; /* skip qualifier . name . ! */
                continue;
            }
        }

        /* ── fn macro invocation: name . ! ( args ) ── */
        if (k == TokIdent
                && i + 3 < end
                && src->toks[i+1].kind == TokDot
                && src->toks[i+2].kind == TokBang
                && src->toks[i+3].kind == TokLParen) {

            char name[64];
            copy_tok_name(name, sizeof(name), t);

            macro_fn_t *fn = lookup_fn(name, local, imports, import_count);
            if (fn && !is_expanding(fn->name)) {
                /* Record where this invocation appears (the call site). */
                pp_expansion_t *call_exp = new_expansion(
                    out, fn->name,
                    t->file, t->line, t->col,
                    parent_exp
                );

                /* Collect arguments from the call site. */
                arg_set_t args;
                int j = collect_args(src, i + 4, &args);

                /* Push recursion guard. */
                if (g_expand_depth < PP_MAX_EXPAND_DEPTH)
                    g_expanding[g_expand_depth++] = fn->name;

                expand_fn_macro(fn, &args, local, imports, import_count,
                                out, call_exp);

                if (g_expand_depth > 0) g_expand_depth--;

                as_free(&args);

                /* Consume a trailing ';' at the call site, if present. */
                if (j < end && src->toks[j].kind == TokSemicolon) j++;
                i = j;
                continue;
            }
        }

        /* ── let macro invocation: name . ! ── */
        if (k == TokIdent
                && i + 2 < end
                && src->toks[i+1].kind == TokDot
                && src->toks[i+2].kind == TokBang) {

            char name[64];
            copy_tok_name(name, sizeof(name), t);

            macro_let_t *lt = lookup_let(name, local, imports, import_count);
            if (lt && !is_expanding(lt->name)) {
                pp_expansion_t *call_exp = new_expansion(
                    out, lt->name,
                    t->file, t->line, t->col,
                    parent_exp
                );

                if (g_expand_depth < PP_MAX_EXPAND_DEPTH)
                    g_expanding[g_expand_depth++] = lt->name;

                /* Emit each body token with the expansion chain attached. */
                for (int bi = 0; bi < lt->tok_count; bi++) {
                    macro_tok_t *mt = &lt->tokens[bi];
                    token_t tok;
                    tok.kind   = mt->kind;
                    tok.start  = mt->text;
                    tok.length = mt->text_len;
                    tok.line   = mt->line;
                    tok.col    = mt->col;
                    tok.file   = mt->file;
                    emit_token(out, tok, call_exp);
                }

                if (g_expand_depth > 0) g_expand_depth--;

                i += 3; /* skip name . ! */
                continue;
            }
        }

        /* ── Default: emit token verbatim ── */
        if (k != TokEof)
            emit_token(out, *t, parent_exp);
        i++;
    }
}

/* ─────────────────────────────────────────────────────────────────────────────
   Public API
   ───────────────────────────────────────────────────────────────────────── */

pp_stream_t *pp_process(const char *source, const char *file_path,
                         pp_macro_set_t **imported_sets, int import_count) {
    if (!source) return NULL;

    /* Register this file with the diagnostic system so source snippets work. */
    diag_register_source(file_path, source);

    /* ── Lex all tokens, tagging each with file_path ── */
    raw_arr_t raw;
    ra_init(&raw);
    {
        lexer_t lex;
        init_lexer(&lex, source);
        for (;;) {
            token_t t = next_token(&lex);
            t.file = file_path;
            /* Keep lex errors in the stream; the parser will report them. */
            ra_push(&raw, t);
            if (t.kind == TokEof) break;
        }
    }

    /* ── Allocate output stream ── */
    pp_stream_t *stream = malloc(sizeof(pp_stream_t));
    memset(stream, 0, sizeof(*stream));
    stream->cap    = 256;
    stream->tokens = malloc((size_t)stream->cap * sizeof(pp_token_t));
    pool_init(stream);

    /* ── Pass 1: collect macro definitions ── */
    pp_macro_set_t *local_ms = pp_macro_set_new();
    scan_macro_defs(local_ms, &raw, file_path);

    /* ── Pass 2: expand macro invocations → pp_stream_t ── */
    g_expand_depth = 0;
    expand_stream(&raw, 0, raw.count,
                  local_ms, imported_sets, import_count,
                  stream, NULL);

    /* Emit EOF. */
    {
        pp_token_t eof;
        memset(&eof, 0, sizeof(eof));
        eof.tok.kind = TokEof;
        eof.tok.file = file_path;
        stream_push_raw(stream, eof);
    }

    ra_free(&raw);

    /* Compute export set: only ext macros from this file. */
    pp_macro_set_t *exports = pp_macro_set_new();
    for (int i = 0; i < local_ms->fn_count; i++)
        if (local_ms->fns[i].vis == MacroVis_External)
            *ms_add_fn(exports) = local_ms->fns[i];
    for (int i = 0; i < local_ms->let_count; i++)
        if (local_ms->lets[i].vis == MacroVis_External)
            *ms_add_let(exports) = local_ms->lets[i];

    /* The full local set is no longer needed (its body ptrs point into source). */
    free(local_ms->fns);
    free(local_ms->lets);
    free(local_ms);

    stream->exports = exports;
    return stream;
}

pp_macro_set_t *pp_get_exports(pp_stream_t *stream) {
    return stream ? stream->exports : NULL;
}

void pp_stream_free(pp_stream_t *stream) {
    if (!stream) return;

    /*
     * Free all expansion nodes via the pool list (each node appears exactly
     * once, even though many tokens may share the same expansion pointer).
     */
    pp_expansion_t *e = stream->exp_pool_head;
    while (e) {
        pp_expansion_t *next = e->pool_next;
        free(e);
        e = next;
    }

    free(stream->tokens);
    free(stream->text_pool);
    pp_macro_set_free(stream->exports);
    free(stream);
}

void pp_format_expansion(const pp_expansion_t *chain, char *buf, int bufsz) {
    int pos = 0;
    while (chain && pos < bufsz - 1) {
        int n = snprintf(buf + pos, (size_t)(bufsz - pos),
                         "  expanded from macro '%s' at %s:%lu:%lu\n",
                         chain->macro_name,
                         chain->call_file,
                         (unsigned long)chain->call_line,
                         (unsigned long)chain->call_col);
        if (n <= 0) break;
        pos += n;
        chain = chain->parent;
    }
    if (bufsz > 0) buf[pos < bufsz ? pos : bufsz - 1] = '\0';
}
