/* Self-contained regex engine — recursive backtracking, ERE subset.
   Public-domain implementation written for Stasha; no libc <regex.h>. */

#include "regex_rt.h"
#include <string.h>

/* ── Internal representation ──────────────────────────────────────────────
   We compile the pattern into a flat array of atoms.  Each atom has a
   "kind" + an inline payload.  Quantifiers are encoded as a flag on the
   *preceding* atom rather than as separate ops, which makes the matcher
   loop trivial. */

enum {
    AtomChar     = 1,  /* match payload.ch literally */
    AtomDot      = 2,  /* match any char except '\n' */
    AtomClass    = 3,  /* match if bitmap[c] is set */
    AtomEnd      = 4,  /* end-of-pattern sentinel */
};

enum {
    QuantNone = 0,
    QuantStar = 1,
    QuantPlus = 2,
    QuantOpt  = 3,
};

typedef struct {
    uint8_t  kind;
    uint8_t  quant;
    uint8_t  ch;        /* AtomChar payload */
    uint8_t  bmap[32];  /* AtomClass: 256-bit bitmap; bmap[c>>3] & (1<<(c&7)) */
} re_atom_t;

/* Maximum atoms in a compiled pattern.  Tunable; 256 covers our stdlib. */
#define RE_MAX_ATOMS 256

typedef struct {
    int       compiled;
    int       n_atoms;
    int       anchor_start;
    int       anchor_end;
    re_atom_t atoms[RE_MAX_ATOMS];
} re_ctx_t;

const size_t sts_regex_ctx_size = sizeof(re_ctx_t);

/* ── Compile ─────────────────────────────────────────────────────────────── */

static void bmap_set(uint8_t *b, unsigned char c) { b[c >> 3] |= (uint8_t)(1u << (c & 7)); }
static int  bmap_get(const uint8_t *b, unsigned char c) {
    return (b[c >> 3] >> (c & 7)) & 1;
}

static void class_shorthand(uint8_t *b, char esc) {
    switch (esc) {
        case 'd':
            for (int c = '0'; c <= '9'; c++) bmap_set(b, (unsigned char)c);
            break;
        case 'D':
            for (int c = 0; c < 256; c++)
                if (!(c >= '0' && c <= '9')) bmap_set(b, (unsigned char)c);
            break;
        case 'w':
            for (int c = '0'; c <= '9'; c++) bmap_set(b, (unsigned char)c);
            for (int c = 'a'; c <= 'z'; c++) bmap_set(b, (unsigned char)c);
            for (int c = 'A'; c <= 'Z'; c++) bmap_set(b, (unsigned char)c);
            bmap_set(b, '_');
            break;
        case 'W': {
            uint8_t tmp[32] = {0};
            class_shorthand(tmp, 'w');
            for (int c = 0; c < 256; c++) if (!bmap_get(tmp, (unsigned char)c)) bmap_set(b, (unsigned char)c);
            break;
        }
        case 's':
            bmap_set(b, ' '); bmap_set(b, '\t'); bmap_set(b, '\n');
            bmap_set(b, '\r'); bmap_set(b, '\f'); bmap_set(b, '\v');
            break;
        case 'S': {
            uint8_t tmp[32] = {0};
            class_shorthand(tmp, 's');
            for (int c = 0; c < 256; c++) if (!bmap_get(tmp, (unsigned char)c)) bmap_set(b, (unsigned char)c);
            break;
        }
        default:
            /* Unknown escape: treat as literal. */
            bmap_set(b, (unsigned char)esc);
            break;
    }
}

int sts_regex_compile(void *ctx_buf, const char *pattern, int flags) {
    (void)flags;
    re_ctx_t *ctx = (re_ctx_t *)ctx_buf;
    memset(ctx, 0, sizeof(*ctx));
    if (!pattern) return 1;

    const char *p = pattern;
    if (*p == '^') { ctx->anchor_start = 1; p++; }

    while (*p) {
        if (ctx->n_atoms >= RE_MAX_ATOMS) return 2;

        /* Detect trailing $ (only at end of pattern) */
        if (*p == '$' && p[1] == '\0') { ctx->anchor_end = 1; p++; break; }

        re_atom_t *a = &ctx->atoms[ctx->n_atoms++];
        a->kind = 0;
        a->quant = QuantNone;

        if (*p == '.') {
            a->kind = AtomDot;
            p++;
        } else if (*p == '\\') {
            p++;
            if (!*p) return 3;
            /* shorthand classes -> AtomClass; other escapes -> AtomChar */
            if (*p == 'd' || *p == 'D' || *p == 'w' || *p == 'W' ||
                *p == 's' || *p == 'S') {
                a->kind = AtomClass;
                class_shorthand(a->bmap, *p);
            } else {
                a->kind = AtomChar;
                a->ch = (uint8_t)*p;
            }
            p++;
        } else if (*p == '[') {
            a->kind = AtomClass;
            p++;
            int negate = 0;
            if (*p == '^') { negate = 1; p++; }
            uint8_t tmp[32] = {0};
            while (*p && *p != ']') {
                unsigned char lo;
                if (*p == '\\' && p[1]) { lo = (unsigned char)p[1]; p += 2; }
                else                    { lo = (unsigned char)*p;   p++;   }
                unsigned char hi = lo;
                if (*p == '-' && p[1] && p[1] != ']') {
                    p++;
                    if (*p == '\\' && p[1]) { hi = (unsigned char)p[1]; p += 2; }
                    else                    { hi = (unsigned char)*p;   p++;   }
                }
                if (hi < lo) { unsigned char t = lo; lo = hi; hi = t; }
                for (int c = lo; c <= hi; c++) bmap_set(tmp, (unsigned char)c);
            }
            if (*p != ']') return 4;
            p++;
            if (negate) {
                for (int c = 0; c < 256; c++) {
                    if (!bmap_get(tmp, (unsigned char)c)) bmap_set(a->bmap, (unsigned char)c);
                }
            } else {
                memcpy(a->bmap, tmp, sizeof(tmp));
            }
        } else {
            a->kind = AtomChar;
            a->ch = (uint8_t)*p;
            p++;
        }

        /* Quantifier? */
        if (*p == '*')      { a->quant = QuantStar; p++; }
        else if (*p == '+') { a->quant = QuantPlus; p++; }
        else if (*p == '?') { a->quant = QuantOpt;  p++; }
    }

    if (ctx->n_atoms >= RE_MAX_ATOMS) return 2;
    ctx->atoms[ctx->n_atoms].kind = AtomEnd;
    ctx->compiled = 1;
    return 0;
}

/* ── Match ──────────────────────────────────────────────────────────────── */

static int atom_match(const re_atom_t *a, unsigned char c) {
    switch (a->kind) {
        case AtomChar:  return a->ch == c;
        case AtomDot:   return c != '\n';
        case AtomClass: return bmap_get(a->bmap, c);
        default:        return 0;
    }
}

/* Returns 1 + chars-consumed on success, 0 on failure.  Encoded as
   ((consumed) | 0x10000) so a zero-length successful match (* with 0
   reps) is distinguishable from failure. */
static int match_here(const re_ctx_t *ctx, int ai, const char *text,
                      int ti, int *out_end);

static int match_quant(const re_ctx_t *ctx, int ai, const char *text,
                       int ti, int min, int max, int *out_end) {
    const re_atom_t *a = &ctx->atoms[ai];
    /* Greedy: consume as many as possible up to max, then back off. */
    int count = 0;
    int text_len = (int)strlen(text);
    while (count < max && ti + count < text_len &&
           atom_match(a, (unsigned char)text[ti + count])) {
        count++;
    }
    while (count >= min) {
        if (match_here(ctx, ai + 1, text, ti + count, out_end)) return 1;
        if (count == 0) break;
        count--;
    }
    return 0;
}

static int match_here(const re_ctx_t *ctx, int ai, const char *text,
                      int ti, int *out_end) {
    while (ctx->atoms[ai].kind != AtomEnd) {
        const re_atom_t *a = &ctx->atoms[ai];
        if (a->quant == QuantStar) {
            return match_quant(ctx, ai, text, ti, 0, 1 << 30, out_end);
        } else if (a->quant == QuantPlus) {
            return match_quant(ctx, ai, text, ti, 1, 1 << 30, out_end);
        } else if (a->quant == QuantOpt) {
            return match_quant(ctx, ai, text, ti, 0, 1, out_end);
        } else {
            if (!text[ti] || !atom_match(a, (unsigned char)text[ti])) return 0;
            ti++;
            ai++;
        }
    }
    if (ctx->anchor_end && text[ti] != '\0') return 0;
    *out_end = ti;
    return 1;
}

int sts_regex_test(const void *ctx_buf, const char *text) {
    int32_t s, e;
    return sts_regex_exec(ctx_buf, text, &s, &e);
}

int sts_regex_exec(const void *ctx_buf, const char *text,
                   int32_t *out_start, int32_t *out_end) {
    const re_ctx_t *ctx = (const re_ctx_t *)ctx_buf;
    if (!ctx->compiled || !text) return 0;
    int end = 0;

    if (ctx->anchor_start) {
        if (match_here(ctx, 0, text, 0, &end)) {
            *out_start = 0;
            *out_end   = end;
            return 1;
        }
        return 0;
    }

    int len = (int)strlen(text);
    for (int i = 0; i <= len; i++) {
        if (match_here(ctx, 0, text, i, &end)) {
            *out_start = i;
            *out_end   = end;
            return 1;
        }
    }
    return 0;
}
