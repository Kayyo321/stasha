/* Fixed pure helper tests: no generated spec is ever executed by printf.
 * Huge numeric endpoints remain parsing/arithmetic inputs only. */
#include <stdio.h>
#include <string.h>
#include "../../src/codegen/format_spec.h"

static unsigned checks;
#define CHECK(c) do { checks++; if (!(c)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); return 1; \
} } while (0)

static int parse_and_build(void) {
    static const struct {
        const char *text, *expected;
        unsigned long width, prec, consumed;
        int has_precision, digits, selected;
        char base;
    } cases[] = {
        { ".0", "%.0f", 0, 0, 2, 1, 1, 1, 0 },
        { ".4", "%.4f", 0, 4, 2, 1, 1, 1, 0 },
        { "+.4", "%+.4f", 0, 4, 3, 1, 1, 1, 0 },
        { "08.2", "%08.2f", 8, 2, 4, 1, 1, 1, 0 },
        { "<8.2", "%-8.2f", 8, 2, 4, 1, 1, 1, 0 },
        { "<+08.2", "%-+8.2f", 8, 2, 6, 1, 1, 1, 0 },
        { "#.0", "%.0f", 0, 0, 3, 1, 1, 1, 0 },
        { "++##<<0008.002", "%-+8.2f", 8, 2, 14, 1, 1, 1, 0 },
        { "0008.02", "%08.2f", 8, 2, 7, 1, 1, 1, 0 },
        { "", "%g", 0, 0, 0, 0, 0, 0, 0 },
        { "+", "%+g", 0, 0, 1, 0, 0, 0, 0 },
        { "8", "%8g", 8, 0, 1, 0, 0, 0, 0 },
        { "<+08", "%-+8g", 8, 0, 4, 0, 0, 0, 0 },
        { ".", "%.0f", 0, 0, 1, 1, 0, 0, 0 },
        { ".2x", "%.2f", 0, 2, 3, 1, 1, 0, 'x' },
        { ".2b", "%.2f", 0, 2, 3, 1, 1, 0, 'b' },
        { ".2f", "%.2f", 0, 2, 2, 1, 1, 0, 0 },
        { ".2junk", "%.2f", 0, 2, 2, 1, 1, 0, 0 },
        { ".2 ", "%.2f", 0, 2, 2, 1, 1, 0, 0 },
        { "-.2", "%g", 0, 0, 0, 0, 0, 0, 0 },
        { " .2", "%g", 0, 0, 0, 0, 0, 0, 0 },
        { ".-2", "%.0f", 0, 0, 1, 1, 0, 0, 0 },
        { "x", "%g", 0, 0, 1, 0, 0, 0, 'x' },
        { "+02147483647.2147483647", "%+02147483647.2147483647f",
            2147483647UL, 2147483647UL, 23, 1, 1, 1, 0 },
        { "<+02147483647.2147483647", "%-+2147483647.2147483647f",
            2147483647UL, 2147483647UL, 24, 1, 1, 1, 0 }
    };
    for (size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
        const unsigned long len = (unsigned long)strlen(cases[k].text);
        stash_format_spec_t sp = stash_format_parse(cases[k].text, len);
        CHECK(sp.status == STASH_FORMAT_OK);
        CHECK(sp.width == cases[k].width && sp.prec == cases[k].prec);
        CHECK(sp.consumed == cases[k].consumed);
        CHECK(sp.has_precision == cases[k].has_precision);
        CHECK(sp.precision_digits == cases[k].digits && sp.base == cases[k].base);
        CHECK(stash_format_select_precision(&sp, len) == cases[k].selected);
        unsigned long needed = 999;
        CHECK(stash_format_float_conversion(&sp, NULL, 0, &needed) == STASH_FORMAT_OK);
        CHECK(needed == strlen(cases[k].expected) + 1);
        unsigned char box[40];
        memset(box, 0xa5, sizeof(box));
        CHECK(stash_format_float_conversion(&sp, (char *)box + 1, needed, &needed) == STASH_FORMAT_OK);
        CHECK(strcmp((char *)box + 1, cases[k].expected) == 0);
        CHECK(box[0] == 0xa5 && box[needed + 1] == 0xa5);
        unsigned long exact = needed;
        memset(box, 0xa5, sizeof(box));
        CHECK(stash_format_float_conversion(&sp, (char *)box + 1, exact - 1, &needed) == STASH_FORMAT_CAPACITY);
        CHECK(needed == exact && box[1] == 0 && box[0] == 0xa5);
        for (size_t j = 2; j < sizeof(box); j++) CHECK(box[j] == 0xa5);
        memset(box, 0xa5, sizeof(box));
        CHECK(stash_format_float_conversion(&sp, (char *)box + 1, 0, &needed) == STASH_FORMAT_CAPACITY);
        for (size_t j = 0; j < sizeof(box); j++) CHECK(box[j] == 0xa5);
        CHECK(stash_format_float_conversion(&sp, NULL, exact, &needed) == STASH_FORMAT_CAPACITY);
        CHECK(stash_format_float_conversion(&sp, (char *)box + 1, sizeof(box) - 2, &needed) == STASH_FORMAT_OK);
        CHECK(strcmp((char *)box + 1, cases[k].expected) == 0 && box[39] == 0xa5);
    }
    return 0;
}

static int numeric_ranges(void) {
    static const struct { const char *text; stash_format_status_t status; } bad[] = {
        { "2147483648.2", STASH_FORMAT_WIDTH_RANGE },
        { "0002147483648.2", STASH_FORMAT_WIDTH_RANGE },
        { ".2147483648", STASH_FORMAT_PRECISION_RANGE },
        { ".0002147483648", STASH_FORMAT_PRECISION_RANGE },
        { ".2147483648x", STASH_FORMAT_PRECISION_RANGE },
        { ".2147483648q", STASH_FORMAT_PRECISION_RANGE },
        { "18446744073709551615.2", STASH_FORMAT_WIDTH_RANGE },
        { ".18446744073709551615", STASH_FORMAT_PRECISION_RANGE },
        { "18446744073709551616.2", STASH_FORMAT_WIDTH_RANGE },
        { ".18446744073709551616z", STASH_FORMAT_PRECISION_RANGE }
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        stash_format_spec_t sp = stash_format_parse(bad[i].text, (unsigned long)strlen(bad[i].text));
        CHECK(sp.status == bad[i].status);
        CHECK(sp.width == 0 && sp.prec == 0 && !sp.has_precision && sp.consumed == 0);
        CHECK(!stash_format_select_precision(&sp, (unsigned long)strlen(bad[i].text)));
        unsigned long needed = 999, bound = 999;
        unsigned char box[] = {0xa5, 0xa5, 0xa5, 0xa5};
        CHECK(stash_format_float_conversion(&sp, (char *)box + 1, 2, &needed) == bad[i].status);
        CHECK(needed == 0 && box[0] == 0xa5 && box[1] == 0 && box[2] == 0xa5 && box[3] == 0xa5);
        CHECK(stash_format_float_bound(&sp, &bound) == bad[i].status && bound == 0);
    }
    stash_format_spec_t sp = stash_format_parse("0002147483647.0002147483647", 27);
    CHECK(sp.status == STASH_FORMAT_OK && sp.width == 2147483647UL && sp.prec == 2147483647UL);
    char output[32]; unsigned long needed;
    sp.width = STASH_FORMAT_INT_MAX + 1;
    CHECK(stash_format_float_conversion(&sp, output, sizeof(output), &needed) == STASH_FORMAT_WIDTH_RANGE);
    sp.width = 0; sp.prec = STASH_FORMAT_INT_MAX + 1;
    CHECK(stash_format_float_conversion(&sp, output, sizeof(output), &needed) == STASH_FORMAT_PRECISION_RANGE);
    unsigned long value = 77, pos = 0;
    CHECK(!stash_format_decimal("2147483648", 10, &pos, &value));
    CHECK(pos == 0 && value == 77);
    return 0;
}

static int legacy_ranges(void) {
    static const struct { const char *text; unsigned long width; stash_format_status_t status; } cases[] = {
        { "", 0, STASH_FORMAT_OK }, { "<8.2", 0, STASH_FORMAT_OK },
        { "08.2", 22, STASH_FORMAT_OK }, { "- 08.2", 22, STASH_FORMAT_OK },
        { ".", 20, STASH_FORMAT_OK }, { "2147483647", 2147483647UL, STASH_FORMAT_OK },
        { ".2147483647", 2147483667UL, STASH_FORMAT_OK },
        { "- 2147483648.2", 0, STASH_FORMAT_WIDTH_RANGE },
        { " .2147483648q", 0, STASH_FORMAT_PRECISION_RANGE },
        { "+#-08.2147483648x", 0, STASH_FORMAT_PRECISION_RANGE }
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        unsigned long width = 777;
        CHECK(stash_format_legacy_width(cases[i].text, (unsigned long)strlen(cases[i].text), &width) == cases[i].status);
        CHECK(width == cases[i].width);
    }
    return 0;
}

static int bounds_and_sizes(void) {
    static const struct { unsigned long width, precision, bound; } cases[] = {
        {0, 0, 310}, {0, 4, 315}, {314, 4, 315}, {315, 4, 315}, {316, 4, 316},
        {0, 2147483335UL, 2147483646UL}, {0, 2147483336UL, 2147483647UL},
        {2147483646UL, 0, 2147483646UL}, {2147483647UL, 0, 2147483647UL}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        stash_format_spec_t sp = {0}; sp.has_precision = 1; sp.width = cases[i].width; sp.prec = cases[i].precision;
        unsigned long bound = 0, aggregate = 0;
        CHECK(stash_format_float_bound(&sp, &bound) == STASH_FORMAT_OK);
        CHECK(bound == cases[i].bound);
        CHECK(stash_format_add(1, bound, STASH_FORMAT_INT_MAX, &aggregate) == (bound < STASH_FORMAT_INT_MAX));
        if (bound < STASH_FORMAT_INT_MAX) CHECK(aggregate == bound + 1);
    }
    stash_format_spec_t sp = {0}; sp.has_precision = 1; sp.prec = 2147483337UL;
    unsigned long value = 77;
    CHECK(stash_format_float_bound(&sp, &value) == STASH_FORMAT_OUTPUT_RANGE && value == 0);
    sp.prec = 0; sp.has_precision = 0;
    CHECK(stash_format_float_bound(&sp, &value) == STASH_FORMAT_OUTPUT_RANGE && value == 0);
    unsigned long total = 1;
    CHECK(stash_format_add(total, 7, STASH_FORMAT_INT_MAX, &total));
    CHECK(stash_format_add(total, 310, STASH_FORMAT_INT_MAX, &total));
    CHECK(stash_format_add(total, 315, STASH_FORMAT_INT_MAX, &total));
    CHECK(total == 633);
    CHECK(stash_format_final_size(1, 0, &value) && value == 16);
    CHECK(stash_format_final_size(1, 1, &value) && value == 512);
    CHECK(stash_format_final_size(633, 0, &value) && value == 633);
    CHECK(stash_format_final_size(STASH_FORMAT_INT_MAX, 1, &value) && value == STASH_FORMAT_INT_MAX);
    CHECK(!stash_format_final_size(STASH_FORMAT_INT_MAX + 1, 0, &value));
    value = 77;
    CHECK(stash_format_add(15, 1, 16, &value) && value == 16);
    CHECK(!stash_format_add(16, 1, 16, &value) && value == 16);
    CHECK(!stash_format_add(17, 0, 16, &value));
    CHECK(stash_format_mul(4, 4, 16, &value) && value == 16);
    CHECK(!stash_format_mul(4, 5, 16, &value) && value == 16);
    CHECK(stash_format_mul(ULONG_MAX, 0, 16, &value) && value == 0);
    CHECK(stash_format_mul(0, ULONG_MAX, 16, &value) && value == 0);
    CHECK(stash_format_add(STASH_FORMAT_HOST_MAX - 1, 1, STASH_FORMAT_HOST_MAX, &value));
    CHECK(!stash_format_add(STASH_FORMAT_HOST_MAX, 1, STASH_FORMAT_HOST_MAX, &value));
    unsigned long flen = (STASH_FORMAT_HOST_MAX - 64) / 4;
    CHECK(stash_format_mul(flen, 4, STASH_FORMAT_HOST_MAX, &value));
    CHECK(stash_format_add(value, 64, STASH_FORMAT_HOST_MAX, &value));
    CHECK(stash_format_mul(flen + 1, 4, STASH_FORMAT_HOST_MAX, &value));
    CHECK(!stash_format_add(value, 64, STASH_FORMAT_HOST_MAX, &value));
    CHECK(stash_format_mul(STASH_FORMAT_HOST_MAX / sizeof(void *), sizeof(void *), STASH_FORMAT_HOST_MAX, &value));
    CHECK(!stash_format_mul(STASH_FORMAT_HOST_MAX / sizeof(void *) + 1, sizeof(void *), STASH_FORMAT_HOST_MAX, &value));
    CHECK(stash_format_add(UINT_MAX - 3UL, 3, UINT_MAX, &value) && value == UINT_MAX);
    CHECK(!stash_format_add(UINT_MAX - 2UL, 3, UINT_MAX, &value));
    return 0;
}

static int append_capacity(void) {
    unsigned char box[8]; memset(box, 0xa5, sizeof(box));
    unsigned long length = 0;
    CHECK(stash_format_append((char *)box + 1, 6, &length, "%%", 2));
    CHECK(stash_format_append((char *)box + 1, 6, &length, "abc", 3));
    CHECK(length == 5 && memcmp(box + 1, "%%abc\0", 6) == 0);
    CHECK(!stash_format_append((char *)box + 1, 6, &length, "x", 1));
    CHECK(length == 5 && box[0] == 0xa5 && box[7] == 0xa5);
    CHECK(stash_format_append((char *)box + 1, 6, &length, "", 0));
    length = ULONG_MAX;
    CHECK(!stash_format_append((char *)box + 1, 6, &length, "x", 1));
    length = 0;
    CHECK(!stash_format_append((char *)box + 1, 0, &length, "", 0));
    return 0;
}

int main(void) {
    if (parse_and_build() || numeric_ranges() || legacy_ranges() || bounds_and_sizes() || append_capacity()) return 1;
    printf("PASS: %u fixed format helper checks\n", checks);
    return 0;
}
