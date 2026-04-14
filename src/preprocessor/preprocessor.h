#ifndef PreprocessorH
#define PreprocessorH

/*
 * Stasha token-based preprocessor.
 *
 * Pipeline:
 *   Source text → Lexer → [Preprocessor] → pp_stream_t → Parser → AST
 *
 * The preprocessor operates on tokens, never on raw strings.
 * Every token in the output stream carries its original source location
 * (file, line, col) so that errors always point to the right place,
 * even inside macro expansions or across imported files.
 *
 * Macro syntax:
 *   int macro fn  name! { ([@param ...]) => { body }; }
 *   ext macro fn  name! { ([@param ...]) => { body }; }
 *   int macro let name! = tokens... ;
 *   ext macro let name! = tokens... ;
 *
 * int → visible only within the defining file.
 * ext → exported; available to files that import this one.
 *
 * Invocation syntax (unchanged from old preprocessor):
 *   name.!(arg1, arg2)   — fn macro call
 *   name.!               — let macro token substitution
 */

#include "../lexer/lexer.h"

/* ── Limits ─────────────────────────────────────────────────────── */

#define PP_MAX_PARAMS       16
#define PP_MAX_EXPAND_DEPTH 32

/* ── Expansion chain ─────────────────────────────────────────────── */

/*
 * One node in the macro call stack recorded at expansion time.
 * When the parser (or codegen) emits an error on a token that came from
 * a macro body, it walks this chain and prints:
 *   = note: expanded from macro 'X' at file:line:col
 *
 * Multiple tokens produced by a single macro expansion share the SAME
 * pp_expansion_t pointer.  To allow pp_stream_free() to release all nodes
 * without double-freeing them, every node is also linked into the owning
 * stream's exp_pool_head singly-linked list via pool_next.
 */
typedef struct pp_expansion_t pp_expansion_t;
struct pp_expansion_t {
    const char     *macro_name;  /* name of the macro that was expanded       */
    const char     *call_file;   /* file where the invocation appeared        */
    usize_t         call_line;   /* line of the invocation                    */
    usize_t         call_col;    /* column of the invocation                  */
    pp_expansion_t *parent;      /* enclosing expansion, or NULL              */
    pp_expansion_t *pool_next;   /* next node in stream's exp_pool_head list  */
};

/* ── Preprocessed token ──────────────────────────────────────────── */

typedef struct {
    token_t         tok;        /* kind, text, file/line/col = definition-site origin */
    pp_expansion_t *expansion;  /* NULL for literal (non-expanded) tokens             */
} pp_token_t;

/* ── Macro visibility ────────────────────────────────────────────── */

typedef enum {
    MacroVis_Internal = 0,  /* int macro — private to the defining file */
    MacroVis_External = 1,  /* ext macro — exported for importers       */
} macro_vis_t;

/* ── Stored token inside a macro body / let expansion ───────────── */

typedef struct {
    token_kind_t  kind;
    const char   *text;      /* points into the defining file's source buffer */
    usize_t       text_len;
    usize_t       line;      /* position inside the defining file             */
    usize_t       col;
    const char   *file;      /* the file that contains this definition        */
} macro_tok_t;

/* ── fn macro: name!(@p1, @p2) => { body } ──────────────────────── */
/*
 * Variadic parameter syntax: (...@args) or (@p1, ...@args)
 *
 * If has_variadic is set, variadic_name holds the pack name ("args" above).
 * Named params before the pack occupy params[0..param_count-1]; the pack
 * collects all remaining call-site arguments starting at index param_count.
 *
 * Inside the body, @foreach iterates over the pack:
 *
 *   @foreach item : args { ... @item ... }
 *
 * The loop is expanded inline during stage-1 parameter substitution.
 */
typedef struct {
    char         name[64];
    macro_vis_t  vis;
    const char  *defined_file;
    int          param_count;            /* named (non-variadic) params   */
    char         params[PP_MAX_PARAMS][64];
    int          has_variadic;           /* 1 if ...@name pack present    */
    char         variadic_name[64];      /* name of the variadic pack     */
    macro_tok_t *body;
    int          body_count;
    int          body_cap;
} macro_fn_t;

/* ── let macro: name! = token-sequence ; ────────────────────────── */

typedef struct {
    char         name[64];
    macro_vis_t  vis;
    const char  *defined_file;
    macro_tok_t *tokens;
    int          tok_count;
    int          tok_cap;
} macro_let_t;

/* ── Macro set: all macros visible during processing of one file ─── */

typedef struct {
    macro_fn_t  *fns;   int fn_count,  fn_cap;
    macro_let_t *lets;  int let_count, let_cap;
} pp_macro_set_t;

/* ── Output stream ───────────────────────────────────────────────── */

typedef struct {
    pp_token_t *tokens;
    int         count;
    int         cap;
    /*
     * Intern pool: owns a copy of the text for tokens that originated
     * inside a macro body.  Tokens not from expansions keep their
     * `.start` pointing into the original source buffer.
     */
    char       *text_pool;
    int         text_used;
    int         text_cap;
    /*
     * Expansion node pool: all pp_expansion_t objects allocated for this
     * stream, linked via pool_next.  pp_stream_free() iterates this list
     * to free every node exactly once (tokens sharing an expansion node
     * are never double-freed this way).
     */
    pp_expansion_t *exp_pool_head;
    /*
     * Exported macro set: only ext macros from this file.
     * Passed as an element of `imported_sets` when preprocessing
     * a file that imports this one.
     */
    pp_macro_set_t *exports;
} pp_stream_t;

/* ── Public API ──────────────────────────────────────────────────── */

/*
 * pp_process — preprocess one source file.
 *
 * source        : null-terminated source text.
 * file_path     : path used in diagnostics and token `.file` fields.
 * imported_sets : ext-macro sets from files this file has imported
 *                 (pass NULL / 0 if there are no imports with macros).
 * import_count  : number of entries in imported_sets.
 *
 * Returns a heap-allocated pp_stream_t.  The caller must call
 * pp_stream_free() when done.
 * Returns NULL on fatal allocation failure.
 *
 * Side effects: calls diag_register_source(file_path, source) so that
 * the diagnostic renderer can print source snippets for this file.
 */
pp_stream_t *pp_process(const char *source, const char *file_path,
                         pp_macro_set_t **imported_sets, int import_count);

/*
 * pp_get_exports — return the ext-macro set of a processed stream.
 * The returned pointer is owned by the stream and freed by pp_stream_free.
 */
pp_macro_set_t *pp_get_exports(pp_stream_t *stream);

/*
 * pp_stream_free — release all memory owned by a stream, including
 * expansion chains and the intern pool.
 */
void pp_stream_free(pp_stream_t *stream);

/*
 * pp_macro_set_free — release a standalone macro set.
 */
void pp_macro_set_free(pp_macro_set_t *set);

/*
 * pp_format_expansion — write a human-readable expansion chain into buf.
 * Each level produces one line:
 *   "  expanded from macro 'X' at file:line:col\n"
 * buf is always NUL-terminated.
 */
void pp_format_expansion(const pp_expansion_t *chain, char *buf, int bufsz);

#endif /* PreprocessorH */
