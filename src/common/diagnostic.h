#ifndef DiagnosticH
#define DiagnosticH

/* NOTE: common.h includes this file after defining its own types, so usize_t,
   boolean_t, Null, etc. are always available here.  Do NOT include common.h
   from this header (it would be circular). */
#include <stdarg.h>

/* ─────────────────────────────────────────────────────────────────────────────
   Stasha Diagnostic System
   Rust-style errors and warnings with source snippets, labels, notes, and
   "did you mean" suggestions.

   Typical usage:

     diag_begin_error("undefined variable '%s'", name);
     diag_span(SRC_LOC(line, col, len), True, "not found in this scope");
     diag_note("variables must be declared before use");
     diag_finish();
   ───────────────────────────────────────────────────────────────────────── */

/* ── Severity levels ── */

typedef enum {
    DiagError,
    DiagWarning,
    DiagNote,
    DiagHelp,
} diag_level_t;

/* ── Source location ── */

typedef struct {
    usize_t line;   /* 1-based line number  (0 = unknown) */
    usize_t col;    /* 1-based column number (0 = unknown) */
    usize_t len;    /* span length in bytes  (0 = point)   */
} src_loc_t;

#define SRC_LOC(l, c, n)    ((src_loc_t){(l), (c), (n)})
#define NO_LOC              ((src_loc_t){0, 0, 0})

/* ── Limits ── */

#define DIAG_MAX_LABELS  6
#define DIAG_MAX_NOTES   8
#define DIAG_MSG_SIZE    512
#define DIAG_LABEL_SIZE  256
#define DIAG_NOTE_SIZE   256
#define DIAG_PATH_SIZE   512
#define DIAG_MAX_CAPTURED 256

/* ── A label pointing at a source span ── */

typedef struct {
    src_loc_t   loc;
    char        text[DIAG_LABEL_SIZE];
    boolean_t   primary;   /* True → ^^^, False → --- */
} diag_label_t;

/* ── A full diagnostic (built incrementally, then rendered by diag_finish) ── */

typedef struct {
    diag_level_t    level;
    char            message[DIAG_MSG_SIZE];
    diag_label_t    labels[DIAG_MAX_LABELS];
    usize_t         label_count;
    char            notes[DIAG_MAX_NOTES][DIAG_NOTE_SIZE];
    diag_level_t    note_kinds[DIAG_MAX_NOTES]; /* DiagNote or DiagHelp */
    usize_t         note_count;
} diagnostic_t;

typedef struct {
    diagnostic_t diag;
    char         filename[DIAG_PATH_SIZE];
} captured_diag_t;

/* ── Global context (set before each parse/codegen pass) ── */

void diag_set_file(const char *filename);
void diag_set_source(const char *source);
const char *diag_get_file(void);

/*
 * Register a (file → source text) mapping so that the diagnostic renderer
 * can show code snippets for any file, not just the main translation unit.
 * Called by the preprocessor for every file it processes.
 * The `source` pointer must outlive all diagnostics for that file.
 */
void diag_register_source(const char *filename, const char *source);
void diag_set_render_enabled(boolean_t enabled);
void diag_clear_captured(void);
usize_t diag_get_captured_count(void);
const captured_diag_t *diag_get_captured(usize_t index);

/* ── Builder API ── */

/* Start accumulating a new diagnostic.  Calling diag_begin_* discards any
   previously-started but not yet finished diagnostic. */
void diag_begin_error(const char *fmt, ...);
void diag_begin_warning(const char *fmt, ...);

/* Add a labeled source span.  primary=True → rendered as ^^^, False → ---. */
void diag_span(src_loc_t loc, boolean_t primary, const char *fmt, ...);

/* Append a note or help line ("= note: ..." / "= help: ..."). */
void diag_note(const char *fmt, ...);
void diag_help(const char *fmt, ...);

/* Render and print the pending diagnostic; update global error/warning counts.
   After this call, no diagnostic is pending. */
void diag_finish(void);

/* ── Shorthand: build + finish in one call (single-label diagnostics) ── */

void diag_error_at(src_loc_t loc, usize_t span_len,
                   const char *label, const char *fmt, ...);
void diag_warning_at(src_loc_t loc, usize_t span_len,
                     const char *label, const char *fmt, ...);

/* ── Levenshtein distance (for "did you mean" suggestions) ── */

usize_t levenshtein(const char *a, const char *b);

/* ── Convenience macros ── */

/* Build src_loc_t from a token_t (requires col field added to token_t) */
#define DIAG_TOK(tok)   SRC_LOC((tok).line, (tok).col, (tok).length)

/* Build src_loc_t from a node_t (col may be 0 for older AST nodes) */
#define DIAG_NODE(n)    SRC_LOC((n)->line, (n)->col, 0)

/* Emit a simple error at a node with one label message */
#define DIAG_ERR(node, msg_fmt, ...) do { \
    diag_begin_error(msg_fmt, ##__VA_ARGS__); \
    diag_span(DIAG_NODE(node), True, msg_fmt, ##__VA_ARGS__); \
    diag_finish(); \
} while (0)

#endif /* DiagnosticH */
