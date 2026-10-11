/* Qualification-only ordinary values. No forged storage, locale/rounding
 * mutation, division by zero, signalling NaN, or formatting oracle. */
#include <float.h>
#include <fenv.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <stddef.h>
#include <string.h>
int fp_environment_ok(void) {
    const char *numeric_locale = setlocale(LC_NUMERIC, NULL); /* query only */
    return numeric_locale != NULL
        && (strcmp(numeric_locale, "C") == 0 || strcmp(numeric_locale, "POSIX") == 0)
        && sizeof(int) == 4 && sizeof(unsigned long) == 8 && sizeof(size_t) == 8
        && CHAR_BIT == 8 && FLT_RADIX == 2 && FLT_MANT_DIG == 24
        && DBL_MANT_DIG == 53 && FLT_MAX_EXP == 128 && DBL_MAX_EXP == 1024
        && fegetround() == FE_TONEAREST
        && strcmp(localeconv()->decimal_point, ".") == 0;
}
double fp_max64(void) { return DBL_MAX; }
float fp_max32(void) { return FLT_MAX; }
double fp_sub64(void) { return DBL_TRUE_MIN; }
float fp_sub32(void) { return FLT_TRUE_MIN; }
double fp_negzero64(void) { return -0.0; }
float fp_negzero32(void) { return -0.0f; }
double fp_nan64(void) { return (double)NAN; }
float fp_nan32(void) { return NAN; }
double fp_inf64(void) { return (double)INFINITY; }
float fp_inf32(void) { return INFINITY; }
