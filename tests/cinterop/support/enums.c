#include "enums.h"

int32_t enum_brighten(int32_t c) {
    /* Map Red -> 100, Green -> 200, Blue -> 300; everything else -> -1. */
    if (c == Red)   return 100;
    if (c == Green) return 200;
    if (c == Blue)  return 300;
    return -1;
}
