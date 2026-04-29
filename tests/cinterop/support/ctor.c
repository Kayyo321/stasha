#include <stdint.h>

int32_t ctor_canary = 0;

__attribute__((constructor))
static void cinterop_ctor_init(void) {
    ctor_canary = 0xC0DECA1E;
}

int32_t ctor_get_canary(void) { return ctor_canary; }
