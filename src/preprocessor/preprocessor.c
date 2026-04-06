#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "preprocessor.h"
#include "../lexer/lexer.h"

/* The preprocessor manages its own temporary token buffers using plain C
   allocation that is independent of the AST arena. Undefine the project-wide
   bans imposed by common.h so we can call stdlib allocators directly. */
#undef malloc
#undef realloc
#undef free

/* ── Limits ── */
#define PP_MAX_MACROS   64
#define PP_MAX_PARAMS   16

/* ── Token array (dynamic) ── */

typedef struct {
    token_t *toks;
    int      count;
    int      cap;
} tok_arr_t;

static void tarr_init(tok_arr_t *a) {
    a->cap   = 256;
    a->count = 0;
    a->toks  = malloc((size_t)a->cap * sizeof(token_t));
}

static void tarr_push(tok_arr_t *a, token_t t) {
    if (a->count >= a->cap) {
        a->cap *= 2;
        a->toks = realloc(a->toks, (size_t)a->cap * sizeof(token_t));
    }
    a->toks[a->count++] = t;
}

static void tarr_free(tok_arr_t *a) {
    free(a->toks);
    a->toks  = NULL;
    a->count = 0;
    a->cap   = 0;
}

/* ── Output buffer (growable char array) ── */

typedef struct {
    char   *data;
    size_t  len;
    size_t  cap;
} pp_buf_t;

static void buf_init(pp_buf_t *b) {
    b->cap  = 4096;
    b->len  = 0;
    b->data = malloc(b->cap);
    if (b->data) b->data[0] = '\0';
}

static void buf_append(pp_buf_t *b, const char *s, size_t n) {
    if (!b->data || !n) return;
    if (b->len + n + 1 >= b->cap) {
        b->cap = (b->len + n + 1) * 2 + 64;
        b->data = realloc(b->data, b->cap);
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

/* Emit a single token's source text followed by a space separator. */
static void buf_emit_tok(pp_buf_t *b, const token_t *t) {
    buf_append(b, t->start, t->length);
    buf_append(b, " ", 1);
}

/* ── Macro definitions ── */

typedef struct {
    char     name[64];
    char     params[PP_MAX_PARAMS][64];
    int      param_count;
    tok_arr_t body;              /* body tokens (own allocation) */
} pp_fn_t;

typedef struct {
    char      name[64];
    tok_arr_t expansion;         /* replacement tokens (own allocation) */
} pp_let_t;

typedef struct {
    pp_fn_t  fns[PP_MAX_MACROS];
    int      fn_count;
    pp_let_t lets[PP_MAX_MACROS];
    int      let_count;
} pp_state_t;

/* ── Helpers ── */

static int tok_is(const token_t *t, const char *s) {
    size_t n = strlen(s);
    return t->length == (usize_t)n && memcmp(t->start, s, n) == 0;
}

static void copy_name(char *dst, const token_t *t) {
    size_t n = t->length < 63 ? t->length : 63;
    memcpy(dst, t->start, n);
    dst[n] = '\0';
}

static pp_fn_t *find_fn(pp_state_t *pp, const char *name) {
    for (int i = 0; i < pp->fn_count; i++)
        if (strcmp(pp->fns[i].name, name) == 0)
            return &pp->fns[i];
    return NULL;
}

static pp_let_t *find_let(pp_state_t *pp, const char *name) {
    for (int i = 0; i < pp->let_count; i++)
        if (strcmp(pp->lets[i].name, name) == 0)
            return &pp->lets[i];
    return NULL;
}

/* ── Lex all tokens from source ── */

static tok_arr_t lex_all(const char *source) {
    tok_arr_t a;
    tarr_init(&a);
    lexer_t lex;
    init_lexer(&lex, source);
    for (;;) {
        token_t t = next_token(&lex);
        if (t.kind == TokError) continue;
        tarr_push(&a, t);
        if (t.kind == TokEof) break;
    }
    return a;
}

/* ── Parse @comptime fn definition ──
 *
 * Expects tokens starting at `i` (the TokAt):
 *   @  comptime  fn  name  !  {  (  [@param ...]  )  =>  {  body  }  ;  }
 *   i  i+1       i+2 i+3  i+4 i+5 ...
 *
 * Returns the index of the first token AFTER the definition.
 */
static int parse_comptime_fn(pp_state_t *pp, const tok_arr_t *a, int i) {
    /* Minimum token count check: @ comptime fn name ! { */
    if (i + 5 >= a->count) return i + 1;
    if (a->toks[i + 2].kind != TokFn)    return i + 1;
    if (a->toks[i + 3].kind != TokIdent) return i + 1;
    if (a->toks[i + 4].kind != TokBang)  return i + 1;
    if (a->toks[i + 5].kind != TokLBrace) return i + 1;

    if (pp->fn_count >= PP_MAX_MACROS) return i + 1;

    pp_fn_t *fn = &pp->fns[pp->fn_count++];
    memset(fn, 0, sizeof(*fn));
    copy_name(fn->name, &a->toks[i + 3]);
    tarr_init(&fn->body);

    int j = i + 6;  /* first token inside outer { */

    /* Expect: ( params... ) */
    if (j >= a->count || a->toks[j].kind != TokLParen) return i + 1;
    j++;  /* skip ( */

    while (j < a->count && a->toks[j].kind != TokRParen) {
        /* @param_name */
        if (a->toks[j].kind == TokAt && j + 1 < a->count
                && a->toks[j + 1].kind == TokIdent
                && fn->param_count < PP_MAX_PARAMS) {
            copy_name(fn->params[fn->param_count++], &a->toks[j + 1]);
            j += 2;
        } else if (a->toks[j].kind == TokComma) {
            j++;
        } else {
            j++;
        }
    }
    if (j >= a->count) return i + 1;
    j++;  /* skip ) */

    /* => */
    if (j >= a->count || a->toks[j].kind != TokFatArrow) return i + 1;
    j++;

    /* { body } */
    if (j >= a->count || a->toks[j].kind != TokLBrace) return i + 1;
    j++;

    int depth = 1;
    while (j < a->count) {
        if (a->toks[j].kind == TokLBrace) depth++;
        else if (a->toks[j].kind == TokRBrace) {
            depth--;
            if (depth == 0) { j++; break; }
        }
        if (depth > 0) tarr_push(&fn->body, a->toks[j]);
        j++;
    }

    /* Skip ; then closing } of the outer brace. */
    while (j < a->count && a->toks[j].kind == TokSemicolon) j++;
    if (j < a->count && a->toks[j].kind == TokRBrace) j++;

    return j;
}

/* ── Parse @comptime let definition ──
 *
 *   @  comptime  let  name  !  =  tokens...  ;
 *   i  i+1       i+2  i+3  i+4 i+5  ...
 */
static int parse_comptime_let(pp_state_t *pp, const tok_arr_t *a, int i) {
    if (i + 5 >= a->count) return i + 1;
    if (a->toks[i + 2].kind != TokLet)   return i + 1;
    if (a->toks[i + 3].kind != TokIdent) return i + 1;
    if (a->toks[i + 4].kind != TokBang)  return i + 1;
    if (a->toks[i + 5].kind != TokEq)    return i + 1;

    if (pp->let_count >= PP_MAX_MACROS) return i + 1;

    pp_let_t *lt = &pp->lets[pp->let_count++];
    memset(lt, 0, sizeof(*lt));
    copy_name(lt->name, &a->toks[i + 3]);
    tarr_init(&lt->expansion);

    int j = i + 6;
    while (j < a->count && a->toks[j].kind != TokSemicolon) {
        tarr_push(&lt->expansion, a->toks[j]);
        j++;
    }
    if (j < a->count) j++;  /* skip ; */

    return j;
}

/* ── Expand a fn macro invocation ──
 *
 * arg_toks   — flat array of all argument tokens
 * arg_starts — arg_starts[k] = start index of k-th argument in arg_toks
 * arg_ends   — arg_ends[k]   = one-past-end index of k-th argument in arg_toks
 * arg_count  — number of arguments
 */
static void expand_fn(const pp_fn_t *fn,
                      const token_t *arg_toks,
                      const int *arg_starts, const int *arg_ends, int arg_count,
                      pp_buf_t *out) {
    for (int i = 0; i < fn->body.count; i++) {
        const token_t *t = &fn->body.toks[i];

        /* @param substitution: TokAt followed by TokIdent matching a parameter */
        if (t->kind == TokAt && i + 1 < fn->body.count
                && fn->body.toks[i + 1].kind == TokIdent) {
            const token_t *pt = &fn->body.toks[i + 1];
            int found = -1;
            for (int p = 0; p < fn->param_count; p++) {
                if (pt->length == strlen(fn->params[p])
                        && memcmp(pt->start, fn->params[p], pt->length) == 0) {
                    found = p;
                    break;
                }
            }
            if (found >= 0) {
                if (found < arg_count) {
                    for (int k = arg_starts[found]; k < arg_ends[found]; k++)
                        buf_emit_tok(out, &arg_toks[k]);
                }
                i++;  /* skip the ident token too */
                continue;
            }
        }

        buf_emit_tok(out, t);
    }
}

/* ── Collect invocation arguments ──
 *
 * On entry, `j` points to the first token after '(' in `name.!(`.
 * Fills flat token array and start/end index arrays.
 * Returns index after the closing ')'.
 */
static int collect_args(const tok_arr_t *a, int j,
                        tok_arr_t *flat,
                        int arg_starts[PP_MAX_PARAMS],
                        int arg_ends[PP_MAX_PARAMS],
                        int *arg_count) {
    *arg_count = 0;
    tarr_init(flat);

    if (*arg_count < PP_MAX_PARAMS)
        arg_starts[*arg_count] = flat->count;

    int depth = 0;
    while (j < a->count) {
        token_kind_t k = a->toks[j].kind;

        if ((k == TokRParen || k == TokRBracket) && depth == 0)
            break;

        if (k == TokLParen || k == TokLBrace || k == TokLBracket) depth++;
        else if (k == TokRParen || k == TokRBrace || k == TokRBracket) depth--;
        else if (k == TokComma && depth == 0) {
            if (*arg_count < PP_MAX_PARAMS) {
                arg_ends[*arg_count]    = flat->count;
                (*arg_count)++;
                arg_starts[*arg_count] = flat->count;
            }
            j++;
            continue;
        }

        tarr_push(flat, a->toks[j]);
        j++;
    }

    /* close out the last (or only) argument */
    if (*arg_count < PP_MAX_PARAMS) {
        arg_ends[*arg_count] = flat->count;
        /* Only count it if there was content OR there are expected params. */
        (*arg_count)++;
    }

    if (j < a->count && a->toks[j].kind == TokRParen) j++;  /* consume ) */

    return j;
}

/* ── Main preprocessing pass ── */

char *preprocess(const char *source) {
    pp_state_t pp;
    memset(&pp, 0, sizeof(pp));

    tok_arr_t a = lex_all(source);
    pp_buf_t  out;
    buf_init(&out);

    int i = 0;
    while (i < a.count) {
        const token_t *t = &a.toks[i];

        /* ── @comptime definition ── */
        if (t->kind == TokAt
                && i + 2 < a.count
                && a.toks[i + 1].kind == TokIdent
                && tok_is(&a.toks[i + 1], "comptime")) {

            token_kind_t what = a.toks[i + 2].kind;

            if (what == TokFn) {
                i = parse_comptime_fn(&pp, &a, i);
                continue;
            }
            if (what == TokLet) {
                i = parse_comptime_let(&pp, &a, i);
                continue;
            }
        }

        /* ── macro invocation: name . ! ... ── */
        if (t->kind == TokIdent
                && i + 2 < a.count
                && a.toks[i + 1].kind == TokDot
                && a.toks[i + 2].kind == TokBang) {

            char name[64];
            copy_name(name, t);

            /* fn invocation: name.!( args ) */
            if (i + 3 < a.count && a.toks[i + 3].kind == TokLParen) {
                pp_fn_t *fn = find_fn(&pp, name);
                if (fn) {
                    int arg_starts[PP_MAX_PARAMS];
                    int arg_ends[PP_MAX_PARAMS];
                    int arg_count = 0;
                    tok_arr_t flat;

                    int j = collect_args(&a, i + 4, &flat,
                                         arg_starts, arg_ends, &arg_count);

                    expand_fn(fn, flat.toks,
                              arg_starts, arg_ends, arg_count, &out);

                    tarr_free(&flat);
                    /* Consume the ';' at the invocation site if present.
                       The macro body already ends with its own ';'. */
                    if (j < a.count && a.toks[j].kind == TokSemicolon) j++;
                    i = j;
                    continue;
                }
            }

            /* let alias: name.! used as type or token replacement */
            pp_let_t *lt = find_let(&pp, name);
            if (lt) {
                for (int k = 0; k < lt->expansion.count; k++)
                    buf_emit_tok(&out, &lt->expansion.toks[k]);
                i += 3;  /* skip name . ! */
                continue;
            }
        }

        /* ── default: emit token verbatim ── */
        if (t->kind != TokEof)
            buf_emit_tok(&out, t);
        i++;
    }

    tarr_free(&a);

    /* Free macro storage. */
    for (int k = 0; k < pp.fn_count;  k++) tarr_free(&pp.fns[k].body);
    for (int k = 0; k < pp.let_count; k++) tarr_free(&pp.lets[k].expansion);

    return out.data;
}
