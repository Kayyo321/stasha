#ifndef STASHA_FORMAT_SPEC_H
#define STASHA_FORMAT_SPEC_H

/* Compiler-only format metadata. No allocation, LLVM, locale, or printf calls.
 * Numeric fields name the current target's signed i32 C-int domain. The host
 * allocation domain is separately limited by both usize_t (unsigned long)
 * and size_t. This does not establish another target's snprintf ABI. */
#include <limits.h>
#include <stddef.h>
#include <stdint.h>

#define STASH_FORMAT_INT_MAX 2147483647UL
#if ULONG_MAX < STASH_FORMAT_INT_MAX
#error "format helpers require unsigned long to represent 2147483647"
#endif
#define STASH_FORMAT_HOST_MAX ((unsigned long)(SIZE_MAX < ULONG_MAX ? SIZE_MAX : ULONG_MAX))

typedef enum {
    STASH_FORMAT_OK,
    STASH_FORMAT_WIDTH_RANGE,
    STASH_FORMAT_PRECISION_RANGE,
    STASH_FORMAT_CAPACITY,
    STASH_FORMAT_OUTPUT_RANGE
} stash_format_status_t;

typedef struct {
    int left_align, force_sign, alt_form, zero_pad;
    unsigned long width, prec;
    int has_precision, precision_digits;
    char base;
    unsigned long consumed;
    stash_format_status_t status;
} stash_format_spec_t;

static inline int stash_format_add(unsigned long a, unsigned long b,
                                    unsigned long limit, unsigned long *out) {
    if (a > limit || b > limit - a) return 0;
    *out = a + b;
    return 1;
}

static inline int stash_format_mul(unsigned long a, unsigned long b,
                                    unsigned long limit, unsigned long *out) {
    if (a != 0 && b > limit / a) return 0;
    *out = a * b;
    return 1;
}

static inline int stash_format_decimal(const char *s, unsigned long len,
                                       unsigned long *pos, unsigned long *out) {
    unsigned long value = 0, i = *pos;
    while (i < len && s[i] >= '0' && s[i] <= '9') {
        unsigned long digit = (unsigned long)(s[i] - '0');
        if (value > (STASH_FORMAT_INT_MAX - digit) / 10) return 0;
        value = value * 10 + digit;
        i++;
    }
    *pos = i;
    *out = value;
    return 1;
}

/* Preserve inline's prefix interpretation, including bare '.' as precision 0.
 * Full consumption and a digit after '.' are separate standalone selection
 * requirements. A range failure invalidates every other field. */
static inline stash_format_spec_t stash_format_parse(const char *s, unsigned long len) {
    stash_format_spec_t sp = {0};
    unsigned long i = 0;
    while (i < len) {
        if      (s[i] == '<') sp.left_align = 1;
        else if (s[i] == '+') sp.force_sign = 1;
        else if (s[i] == '#') sp.alt_form = 1;
        else if (s[i] == '0') sp.zero_pad = 1;
        else break;
        i++;
    }
    if (!stash_format_decimal(s, len, &i, &sp.width)) {
        stash_format_spec_t invalid = {0};
        invalid.status = STASH_FORMAT_WIDTH_RANGE;
        return invalid;
    }
    if (i < len && s[i] == '.') {
        unsigned long start = ++i;
        sp.has_precision = 1;
        if (!stash_format_decimal(s, len, &i, &sp.prec)) {
            stash_format_spec_t invalid = {0};
            invalid.status = STASH_FORMAT_PRECISION_RANGE;
            return invalid;
        }
        sp.precision_digits = i != start;
    }
    if (i < len && (s[i] == 'x' || s[i] == 'X' || s[i] == 'b'
                   || s[i] == 'o' || s[i] == 'd')) sp.base = s[i++];
    sp.consumed = i;
    return sp;
}

static inline int stash_format_select_precision(const stash_format_spec_t *sp,
                                                unsigned long len) {
    return sp->status == STASH_FORMAT_OK && sp->has_precision
        && sp->precision_digits && sp->base == 0 && sp->consumed == len;
}

/* Keep standalone's historical sizing scanner distinct from inline flags. */
static inline stash_format_status_t stash_format_legacy_width(const char *s,
                                 unsigned long len, unsigned long *out) {
    unsigned long i = 0, width = 0, prec = 0;
    *out = 0;
    while (i < len && (s[i] == '+' || s[i] == '-' || s[i] == '#' || s[i] == ' ')) i++;
    if (i < len && s[i] == '0' && i + 1 < len && s[i + 1] >= '0' && s[i + 1] <= '9') i++;
    if (!stash_format_decimal(s, len, &i, &width)) return STASH_FORMAT_WIDTH_RANGE;
    if (i < len && s[i] == '.') {
        i++;
        if (!stash_format_decimal(s, len, &i, &prec)) return STASH_FORMAT_PRECISION_RANGE;
        if (!stash_format_add(prec, 20, STASH_FORMAT_HOST_MAX, &prec)) return STASH_FORMAT_OUTPUT_RANGE;
        if (prec > width) width = prec;
    }
    *out = width;
    return STASH_FORMAT_OK;
}

static inline unsigned long stash_format_digit_count(unsigned long value) {
    unsigned long n = 1;
    while (value >= 10) { value /= 10; n++; }
    return n;
}

static inline unsigned long stash_format_write_decimal(char *dst, unsigned long value) {
    unsigned long n = stash_format_digit_count(value), i = n;
    do { dst[--i] = (char)('0' + value % 10); value /= 10; } while (i);
    return n;
}

/* needed includes NUL. Query with (NULL, 0); insufficient capacity writes only
 * dst[0] = NUL when possible. '#' remains deliberately ignored for floats. */
static inline stash_format_status_t stash_format_float_conversion(
        const stash_format_spec_t *sp, char *dst, unsigned long capacity,
        unsigned long *needed) {
    *needed = 0;
    if (dst && capacity) dst[0] = '\0';
    if (sp->status != STASH_FORMAT_OK) return sp->status;
    if (sp->width > STASH_FORMAT_INT_MAX) return STASH_FORMAT_WIDTH_RANGE;
    if (sp->prec > STASH_FORMAT_INT_MAX) return STASH_FORMAT_PRECISION_RANGE;
    unsigned long n = 3 + !!sp->left_align + !!sp->force_sign
        + (sp->zero_pad && !sp->left_align);
    if (sp->width) n += stash_format_digit_count(sp->width);
    if (sp->has_precision) n += 1 + stash_format_digit_count(sp->prec);
    *needed = n;
    if (!dst && capacity == 0) return STASH_FORMAT_OK;
    if (!dst || capacity < n) return STASH_FORMAT_CAPACITY;
    unsigned long i = 0;
    dst[i++] = '%';
    if (sp->left_align) dst[i++] = '-';
    if (sp->force_sign) dst[i++] = '+';
    if (sp->zero_pad && !sp->left_align) dst[i++] = '0';
    if (sp->width) i += stash_format_write_decimal(dst + i, sp->width);
    if (sp->has_precision) {
        dst[i++] = '.';
        i += stash_format_write_decimal(dst + i, sp->prec);
    }
    dst[i++] = sp->has_precision ? 'f' : 'g';
    dst[i] = '\0';
    return STASH_FORMAT_OK;
}

/* Finite IEEE binary64 C-numeric-locale bound; compact nonfinites are qualified
 * separately. Includes a sign but not the final aggregate NUL. */
static inline stash_format_status_t stash_format_float_bound(
        const stash_format_spec_t *sp, unsigned long *out) {
    unsigned long natural = 310;
    *out = 0;
    if (sp->status != STASH_FORMAT_OK) return sp->status;
    if (sp->width > STASH_FORMAT_INT_MAX) return STASH_FORMAT_WIDTH_RANGE;
    if (sp->prec > STASH_FORMAT_INT_MAX) return STASH_FORMAT_PRECISION_RANGE;
    if (!sp->has_precision) return STASH_FORMAT_OUTPUT_RANGE;
    if (sp->prec && !stash_format_add(311, sp->prec, STASH_FORMAT_INT_MAX, &natural))
        return STASH_FORMAT_OUTPUT_RANGE;
    *out = sp->width > natural ? sp->width : natural;
    return STASH_FORMAT_OK;
}

static inline int stash_format_final_size(unsigned long bound, int on_heap,
                                          unsigned long *out) {
    unsigned long minimum = on_heap ? 512 : 16;
    unsigned long value = bound > minimum ? bound : minimum;
    if (value > STASH_FORMAT_INT_MAX || value > UINT_MAX || value > UINT64_MAX) return 0;
    *out = value;
    return 1;
}

/* Reserve the final NUL at each append; no would-have-written cursor updates. */
static inline int stash_format_append(char *dst, unsigned long capacity,
        unsigned long *length, const char *src, unsigned long count) {
    if (*length >= capacity || count > capacity - *length - 1) return 0;
    for (unsigned long i = 0; i < count; i++) dst[*length + i] = src[i];
    *length += count;
    dst[*length] = '\0';
    return 1;
}

#endif
