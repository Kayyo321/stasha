/* ── diagnostic.c ────────────────────────────────────────────────────────────
   Stasha compiler diagnostic rendering.

   Produces Rust-style output:

     error: undefined variable 'foo'
       --> main.sts:15:9
        |
     13 |     stack i32 y = 5;
     14 |     stack i32 z = 3;
     15 |     stack i32 w = foo + z;
        |                   ^^^ not found in this scope
        |
        = note: variables must be declared before use
        = help: did you mean 'for'?
   ─────────────────────────────────────────────────────────────────────────── */

/* This file is #included into common.c — do not include common.h/diagnostic.h
   here as they are already in scope.  System headers below are idempotent. */
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>

/* ── Global context ── */

static const char *g_filename = "<unknown>";
static const char *g_source   = Null;
static boolean_t   g_render_enabled = True;

/* In-progress diagnostic being built with diag_begin_* / diag_span / ... */
static diagnostic_t g_pending;
static boolean_t    g_has_pending = False;

/* ── Multi-file source registry ── */

#define DIAG_MAX_REGISTERED_SOURCES 64

typedef struct {
    const char *filename;
    const char *source;
} diag_source_entry_t;

static diag_source_entry_t g_source_registry[DIAG_MAX_REGISTERED_SOURCES];
static int                 g_source_registry_count = 0;
static captured_diag_t     g_captured_diags[DIAG_MAX_CAPTURED];
static usize_t             g_captured_diag_count = 0;

void diag_register_source(const char *filename, const char *source) {
    if (!filename || !source) return;
    /* Update existing entry if already registered. */
    for (int i = 0; i < g_source_registry_count; i++) {
        if (strcmp(g_source_registry[i].filename, filename) == 0) {
            g_source_registry[i].source = source;
            return;
        }
    }
    if (g_source_registry_count < DIAG_MAX_REGISTERED_SOURCES) {
        g_source_registry[g_source_registry_count].filename = filename;
        g_source_registry[g_source_registry_count].source   = source;
        g_source_registry_count++;
    }
}

void diag_set_render_enabled(boolean_t enabled) {
    g_render_enabled = enabled;
}

void diag_clear_captured(void) {
    g_captured_diag_count = 0;
}

usize_t diag_get_captured_count(void) {
    return g_captured_diag_count;
}

const captured_diag_t *diag_get_captured(usize_t index) {
    if (index >= g_captured_diag_count) return Null;
    return &g_captured_diags[index];
}

static const char *lookup_registered_source(const char *filename) {
    if (!filename) return Null;
    for (int i = 0; i < g_source_registry_count; i++) {
        if (strcmp(g_source_registry[i].filename, filename) == 0)
            return g_source_registry[i].source;
    }
    return Null;
}

void diag_set_file(const char *filename) {
    g_filename = filename ? filename : "<unknown>";
    /* Auto-switch the active source text for snippet rendering. */
    const char *registered = lookup_registered_source(g_filename);
    if (registered) g_source = registered;
}

void diag_set_source(const char *source) {
    g_source = source;
}

const char *diag_get_file(void) {
    return g_filename;
}

/* ── Levenshtein distance ──────────────────────────────────────────────────── */

usize_t levenshtein(const char *a, const char *b) {
    usize_t la = strlen(a);
    usize_t lb = strlen(b);

    /* Use two rows to avoid O(n*m) memory. Limit to 64 chars each to stay
       stack-friendly. */
    if (la > 64 || lb > 64) return (usize_t)-1;

    usize_t prev[65], curr[65];
    for (usize_t j = 0; j <= lb; j++) prev[j] = j;

    for (usize_t i = 1; i <= la; i++) {
        curr[0] = i;
        for (usize_t j = 1; j <= lb; j++) {
            usize_t cost = (a[i-1] == b[j-1]) ? 0 : 1;
            usize_t del   = prev[j]   + 1;
            usize_t ins   = curr[j-1] + 1;
            usize_t sub   = prev[j-1] + cost;
            usize_t m = del < ins ? del : ins;
            curr[j] = m < sub ? m : sub;
        }
        for (usize_t j = 0; j <= lb; j++) prev[j] = curr[j];
    }
    return prev[lb];
}

/* ── Source navigation helpers ─────────────────────────────────────────────── */

/*
 * find_line_start — return a pointer to the first character of line `line`
 * (1-based) within `src`, or NULL if not found.
 */
static const char *find_line_start(const char *src, usize_t line) {
    if (!src || line == 0) return Null;
    usize_t cur = 1;
    const char *p = src;
    while (cur < line) {
        p = strchr(p, '\n');
        if (!p) return Null;
        p++;
        cur++;
    }
    return p;
}

/*
 * line_length — number of bytes in the line starting at `p`, not including
 * the newline.
 */
static usize_t line_length(const char *p) {
    if (!p) return 0;
    const char *end = strchr(p, '\n');
    return end ? (usize_t)(end - p) : (usize_t)strlen(p);
}

/* ── ANSI colour helpers ─────────────────────────────────────────────────── */

/* We emit colour only when writing to a real terminal. */
#ifdef _WIN32
#  define USE_COLOR 0
#else
#  include <unistd.h>
#  define USE_COLOR (isatty(STDERR_FILENO))
#endif

static const char *col_reset(void)  { return USE_COLOR ? "\033[0m"  : ""; }
static const char *col_bold(void)   { return USE_COLOR ? "\033[1m"  : ""; }
static const char *col_red(void)    { return USE_COLOR ? "\033[31m" : ""; }
static const char *col_yellow(void) { return USE_COLOR ? "\033[33m" : ""; }
static const char *col_blue(void)   { return USE_COLOR ? "\033[34m" : ""; }
static const char *col_cyan(void)   { return USE_COLOR ? "\033[36m" : ""; }

/* ── Rendering ─────────────────────────────────────────────────────────────── */

/*
 * digit_count — number of decimal digits in n (minimum 1).
 */
static int digit_count(usize_t n) {
    if (n == 0) return 1;
    int d = 0;
    while (n > 0) { d++; n /= 10; }
    return d;
}

/*
 * render_source_window — print 0-2 context lines before `primary_line`, then
 * the primary line itself, then the underline with labels.
 *
 * gutter_w: width to reserve for line numbers (so they align).
 */
static void render_source_window(const diagnostic_t *d, int gutter_w) {
    if (!g_source || d->label_count == 0) return;

    /* Find the primary label (first one marked primary). */
    const diag_label_t *prim = Null;
    for (usize_t i = 0; i < d->label_count; i++) {
        if (d->labels[i].primary) { prim = &d->labels[i]; break; }
    }
    if (!prim) prim = &d->labels[0];

    usize_t primary_line = prim->loc.line;
    if (primary_line == 0) return;   /* unknown location */

    /* Determine how many context lines to show (up to 2 before). */
    usize_t ctx_start = primary_line > 2 ? primary_line - 2 : 1;

    /* Print the empty gutter separator. */
    fprintf(stderr, "%s%*s |%s\n", col_blue(), gutter_w, "", col_reset());

    /* Print context lines (before the primary line). */
    for (usize_t ln = ctx_start; ln <= primary_line; ln++) {
        const char *ls = find_line_start(g_source, ln);
        if (!ls) continue;
        usize_t len = line_length(ls);

        /* Truncate very long lines for display. */
        char linebuf[256];
        usize_t display_len = len < (sizeof(linebuf) - 1) ? len : sizeof(linebuf) - 1;
        memcpy(linebuf, ls, display_len);
        linebuf[display_len] = '\0';

        fprintf(stderr, "%s%*lu |%s %s\n",
                col_blue(), gutter_w, (unsigned long)ln, col_reset(), linebuf);

        /* After printing the primary line, render any labels pointing at it. */
        if (ln == primary_line) {
            for (usize_t i = 0; i < d->label_count; i++) {
                const diag_label_t *lbl = &d->labels[i];
                if (lbl->loc.line != ln) continue;
                if (lbl->loc.col == 0) continue;

                /* col is 1-based; the gutter adds "N | " before the source. */
                usize_t col0 = lbl->loc.col > 0 ? lbl->loc.col - 1 : 0;
                usize_t span = lbl->loc.len > 0 ? lbl->loc.len : 1;
                if (span > 80) span = 80;

                /* Build the underline string. */
                char under[256];
                usize_t ui = 0;
                for (usize_t s = 0; s < col0 && ui < sizeof(under) - 2; s++)
                    under[ui++] = ' ';
                char under_char = lbl->primary ? '^' : '-';
                for (usize_t s = 0; s < span && ui < sizeof(under) - 2; s++)
                    under[ui++] = under_char;
                under[ui] = '\0';

                const char *marker_col = lbl->primary
                    ? (d->level == DiagError ? col_red() : col_yellow())
                    : col_blue();

                if (lbl->text[0]) {
                    fprintf(stderr, "%s%*s |%s %s%s %s%s\n",
                            col_blue(), gutter_w, "", col_reset(),
                            marker_col, under, lbl->text, col_reset());
                } else {
                    fprintf(stderr, "%s%*s |%s %s%s%s\n",
                            col_blue(), gutter_w, "", col_reset(),
                            marker_col, under, col_reset());
                }
            }

            /* Also check for secondary labels on other lines and print them. */
        }
    }

    /* Print secondary labels that are on lines OTHER than primary_line. */
    for (usize_t i = 0; i < d->label_count; i++) {
        const diag_label_t *lbl = &d->labels[i];
        if (lbl->primary) continue;
        if (lbl->loc.line == primary_line) continue;
        if (lbl->loc.line == 0) continue;

        const char *ls = find_line_start(g_source, lbl->loc.line);
        if (!ls) continue;
        usize_t len = line_length(ls);
        char linebuf[256];
        usize_t display_len = len < (sizeof(linebuf) - 1) ? len : sizeof(linebuf) - 1;
        memcpy(linebuf, ls, display_len);
        linebuf[display_len] = '\0';

        fprintf(stderr, "%s%*lu |%s %s\n",
                col_blue(), gutter_w, (unsigned long)lbl->loc.line, col_reset(), linebuf);

        if (lbl->loc.col > 0) {
            usize_t col0 = lbl->loc.col - 1;
            usize_t span = lbl->loc.len > 0 ? lbl->loc.len : 1;
            char under[256];
            usize_t ui = 0;
            for (usize_t s = 0; s < col0 && ui < sizeof(under) - 2; s++) under[ui++] = ' ';
            for (usize_t s = 0; s < span && ui < sizeof(under) - 2; s++) under[ui++] = '-';
            under[ui] = '\0';
            if (lbl->text[0]) {
                fprintf(stderr, "%s%*s |%s %s%s %s%s\n",
                        col_blue(), gutter_w, "", col_reset(),
                        col_blue(), under, lbl->text, col_reset());
            } else {
                fprintf(stderr, "%s%*s |%s %s%s%s\n",
                        col_blue(), gutter_w, "", col_reset(),
                        col_blue(), under, col_reset());
            }
        }
    }

    /* Trailing empty gutter line. */
    fprintf(stderr, "%s%*s |%s\n", col_blue(), gutter_w, "", col_reset());
}

static void render_diagnostic(const diagnostic_t *d) {
    /* ── Header line: "error: message" ── */
    const char *level_str;
    const char *level_col;
    switch (d->level) {
        case DiagError:   level_str = "error";   level_col = col_red();    break;
        case DiagWarning: level_str = "warning"; level_col = col_yellow(); break;
        case DiagNote:    level_str = "note";    level_col = col_cyan();   break;
        case DiagHelp:    level_str = "help";    level_col = col_cyan();   break;
        default:          level_str = "error";   level_col = col_red();    break;
    }

    fprintf(stderr, "%s%s%s%s: %s%s%s\n",
            col_bold(), level_col, level_str, col_reset(),
            col_bold(), d->message, col_reset());

    /* ── Location: "  --> file:line:col" ── */
    src_loc_t loc = NO_LOC;
    for (usize_t i = 0; i < d->label_count; i++) {
        if (d->labels[i].primary || i == 0) {
            loc = d->labels[i].loc;
            if (d->labels[i].primary) break;
        }
    }

    if (loc.line > 0) {
        /* Compute gutter width from the highest line number we'll print. */
        usize_t max_line = loc.line;
        for (usize_t i = 0; i < d->label_count; i++)
            if (d->labels[i].loc.line > max_line)
                max_line = d->labels[i].loc.line;
        int gutter_w = digit_count(max_line);
        if (gutter_w < 1) gutter_w = 1;

        if (loc.col > 0) {
            fprintf(stderr, "%s%*s --> %s%s:%lu:%lu\n",
                    col_blue(), gutter_w, "", col_reset(),
                    g_filename, (unsigned long)loc.line, (unsigned long)loc.col);
        } else {
            fprintf(stderr, "%s%*s --> %s%s:%lu\n",
                    col_blue(), gutter_w, "", col_reset(),
                    g_filename, (unsigned long)loc.line);
        }

        /* ── Source snippet ── */
        render_source_window(d, gutter_w);
    } else {
        fprintf(stderr, "\n");
    }

    /* ── Notes and help lines ── */
    for (usize_t i = 0; i < d->note_count; i++) {
        const char *kind_str = (d->note_kinds[i] == DiagHelp) ? "help" : "note";
        fprintf(stderr, "   %s=%s %s%s:%s %s\n",
                col_blue(), col_reset(),
                col_bold(), kind_str, col_reset(),
                d->notes[i]);
    }

    if (d->note_count > 0)
        fprintf(stderr, "\n");

    fflush(stderr);
}

/* ── Builder API ─────────────────────────────────────────────────────────── */

static void begin_diag(diag_level_t level, const char *fmt, va_list ap) {
    memset(&g_pending, 0, sizeof(g_pending));
    g_pending.level = level;
    vsnprintf(g_pending.message, sizeof(g_pending.message), fmt, ap);
    g_has_pending = True;
}

void diag_begin_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    begin_diag(DiagError, fmt, ap);
    va_end(ap);
}

void diag_begin_warning(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    begin_diag(DiagWarning, fmt, ap);
    va_end(ap);
}

void diag_span(src_loc_t loc, boolean_t primary, const char *fmt, ...) {
    if (!g_has_pending) return;
    if (g_pending.label_count >= DIAG_MAX_LABELS) return;

    diag_label_t *lbl = &g_pending.labels[g_pending.label_count++];
    lbl->loc     = loc;
    lbl->primary = primary;

    if (fmt && fmt[0]) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(lbl->text, sizeof(lbl->text), fmt, ap);
        va_end(ap);
    } else {
        lbl->text[0] = '\0';
    }
}

void diag_note(const char *fmt, ...) {
    if (!g_has_pending) return;
    if (g_pending.note_count >= DIAG_MAX_NOTES) return;
    usize_t idx = g_pending.note_count++;
    g_pending.note_kinds[idx] = DiagNote;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_pending.notes[idx], DIAG_NOTE_SIZE, fmt, ap);
    va_end(ap);
}

void diag_help(const char *fmt, ...) {
    if (!g_has_pending) return;
    if (g_pending.note_count >= DIAG_MAX_NOTES) return;
    usize_t idx = g_pending.note_count++;
    g_pending.note_kinds[idx] = DiagHelp;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_pending.notes[idx], DIAG_NOTE_SIZE, fmt, ap);
    va_end(ap);
}

void diag_finish(void) {
    if (!g_has_pending) return;
    if (g_captured_diag_count < DIAG_MAX_CAPTURED) {
        captured_diag_t *slot = &g_captured_diags[g_captured_diag_count++];
        memset(slot, 0, sizeof(*slot));
        slot->diag = g_pending;
        snprintf(slot->filename, sizeof(slot->filename), "%s", g_filename ? g_filename : "<unknown>");
    }

    if (g_render_enabled)
        render_diagnostic(&g_pending);

    /* Update global counters used by the rest of the compiler. */
    if (g_pending.level == DiagError)   error_cnt++;
    if (g_pending.level == DiagWarning) warn_cnt++;

    /* Also write a plain one-liner to the log file if open. */
    if (log_file) {
        const char *level_str =
            (g_pending.level == DiagError)   ? "(EE)" :
            (g_pending.level == DiagWarning) ? "(!!)" : "(--)";
        src_loc_t loc = NO_LOC;
        for (usize_t i = 0; i < g_pending.label_count; i++) {
            if (g_pending.labels[i].primary || i == 0) {
                loc = g_pending.labels[i].loc;
                if (g_pending.labels[i].primary) break;
            }
        }
        if (loc.line > 0)
            fprintf(log_file, "%s %s:%lu:%lu: %s\n",
                    level_str, g_filename,
                    (unsigned long)loc.line, (unsigned long)loc.col,
                    g_pending.message);
        else
            fprintf(log_file, "%s %s\n", level_str, g_pending.message);
        fflush(log_file);
    }

    memset(&g_pending, 0, sizeof(g_pending));
    g_has_pending = False;
}

/* ── Shorthand ─────────────────────────────────────────────────────────────── */

void diag_error_at(src_loc_t loc, usize_t span_len,
                   const char *label, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    begin_diag(DiagError, fmt, ap);
    va_end(ap);
    src_loc_t sloc = loc;
    sloc.len = span_len;
    diag_span(sloc, True, "%s", label ? label : "");
    diag_finish();
}

void diag_warning_at(src_loc_t loc, usize_t span_len,
                     const char *label, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    begin_diag(DiagWarning, fmt, ap);
    va_end(ap);
    src_loc_t sloc = loc;
    sloc.len = span_len;
    diag_span(sloc, True, "%s", label ? label : "");
    diag_finish();
}
