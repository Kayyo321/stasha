/* Independently linkable C substitute: qualifies harness startup/oracle before
 * adding Stasha objects. Not evidence that Stasha itself works. */
#include "component.h"
extern _Noreturn void __stasha_panic(uint32_t);
static uint64_t data_cookie = UINT64_C(0x13579bdf2468ace0), bss_cookie;

uint64_t k0_checksum(const uint8_t *p, uint64_t n) {
    uint64_t digest = UINT64_C(0xcbf29ce484222325);
    for (uint64_t i = 0; i < n; ++i)
        digest = (digest ^ p[i]) * UINT64_C(0x100000001b3);
    return digest;
}
uint32_t k0_take(uint64_t *p) {
    for (uint32_t i = 0; i < 64; ++i) {
        if (!(*p & (UINT64_C(1) << i))) {
            *p |= UINT64_C(1) << i;
            return i;
        }
    }
    return 64;
}
uint32_t k0_release(uint64_t *p, uint32_t i) {
    if (i >= 64 || !(*p & (UINT64_C(1) << i))) return 0;
    *p &= ~(UINT64_C(1) << i);
    return 1;
}
uint64_t k0_checked_bytes(uint64_t value) {
    if (value > UINT64_MAX / 8) __stasha_panic(1);
    return value * 8;
}
uint64_t k0_divide(uint64_t a, uint64_t b) {
    if (!b) __stasha_panic(2);
    return a / b;
}
int64_t k0_signed_divide(int64_t a, int64_t b) {
    if (!b) __stasha_panic(2);
    if (a == INT64_MIN && b == -1) __stasha_panic(3);
    return a / b;
}
uint64_t k0_shift(uint64_t a, uint64_t b) {
    if (b >= 64) __stasha_panic(4);
    return a << b;
}
uint64_t k0_high_half(void) { return UINT64_C(0xffffffff80200000); }
void *k0_echo_pointer(void *p) { return p; }
#define ID(name, type) type name(type value) { return value; }
ID(k0_u8, uint8_t)
ID(k0_i8, int8_t)
ID(k0_u16, uint16_t)
ID(k0_i16, int16_t)
ID(k0_u32, uint32_t)
ID(k0_i32, int32_t)
ID(k0_u64, uint64_t)
ID(k0_i64, int64_t)
uint64_t k0_eight(uint64_t a, uint64_t b, uint64_t c, uint64_t d,
                  uint64_t e, uint64_t f, uint64_t g, uint64_t h) {
    return a ^ (b << 1) ^ (c << 2) ^ (d << 3) ^ (e << 4) ^ (f << 5) ^ (g << 6) ^ (h << 7);
}
uint64_t k0_global_probe(void) {
    uint64_t result = data_cookie ^ bss_cookie;
    bss_cookie = UINT64_C(0x1122334455667788);
    return result;
}

/* Byte loops and a temporary snapshot independently specify overlap behavior.
 * Do not use a C struct assignment for partially overlapping byte ranges. */
void k0_copy_block(void *dst, const void *src) {
    uint8_t snapshot[K0_MEMORY_BYTES];
    uint8_t *d = dst;
    const uint8_t *s = src;
    for (size_t i = 0; i < sizeof(snapshot); ++i) snapshot[i] = s[i];
    for (size_t i = 0; i < sizeof(snapshot); ++i) d[i] = snapshot[i];
}
void k0_zero_block(void *dst) {
    uint8_t *d = dst;
    for (size_t i = 0; i < K0_MEMORY_BYTES; ++i) d[i] = 0;
}
uint64_t k0_copy_owned(uint64_t seed) {
    uint8_t snapshot[K0_MEMORY_BYTES];
    for (size_t i = 0; i < sizeof(snapshot); ++i) {
        seed = seed * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
        snapshot[i] = (uint8_t)(seed >> 32);
    }
    return k0_checksum(snapshot, sizeof(snapshot));
}
