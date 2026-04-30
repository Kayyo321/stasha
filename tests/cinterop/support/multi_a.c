#include <stdint.h>

int32_t multi_a_value(void) { return 100; }
int32_t multi_b_value(void); /* defined in multi_b.c */

int32_t multi_combined(void) {
    return multi_a_value() + multi_b_value();
}
