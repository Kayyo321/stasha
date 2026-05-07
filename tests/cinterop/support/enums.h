#ifndef CINTEROP_ENUMS_H
#define CINTEROP_ENUMS_H

#include <stdint.h>

/* Anonymous-typedef enum: cheader must adopt the typedef name as the
 * Stasha-visible enum type, and surface each variant as an int32 constant
 * with the right computed value (default zero, increment after explicit). */
typedef enum {
    Red,            /* 0 */
    Green = 5,      /* 5 */
    Blue            /* 6 */
} color_t;

/* Tagged enum (named); cheader exposes both `enum Mode` and `Mode` typedef. */
typedef enum Mode {
    ModeOff = 0,
    ModeOn  = 1,
    ModeAuto = 2
} Mode;

int32_t enum_brighten(int32_t c);

#endif
