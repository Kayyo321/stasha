/* Shared host/boot C differential oracle: no runtime, heap, I/O or UB required. */
#include "component.h"
uint64_t k0_observations;
uint64_t k0_result_digest;
static uint64_t next_random(uint64_t *state) {
    *state = *state * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
    return *state;
}
static uint64_t checksum(const uint8_t *p, size_t n) {
    uint64_t d = UINT64_C(0xcbf29ce484222325);
    for (size_t i = 0; i < n; ++i) d = (d ^ p[i]) * UINT64_C(0x100000001b3);
    return d;
}
static uint32_t take(uint64_t *p) {
    for (uint32_t i = 0; i < 64; ++i) {
        uint64_t mask = UINT64_C(1) << i;
        if (!(*p & mask)) { *p |= mask; return i; }
    }
    return 64;
}
static uint32_t release(uint64_t *p, uint32_t i) {
    if (i >= 64 || !(*p & (UINT64_C(1) << i))) return 0;
    *p &= ~(UINT64_C(1) << i);
    return 1;
}
#define CHECK(c, id) do { ++k0_observations; if (!(c)) return (id); } while (0)
/* -1 is the independent C baseline; 0 is scalarized Stasha O0; 1 requires
 * actual optimized Stasha libcalls. Every route makes the same observations. */
#ifndef K0_REQUIRE_MEMORY_HELPERS
#error "runner must specify expected lowering coverage"
#endif
static uint32_t memory_tests(uint64_t *state) {
    enum { STORAGE = K0_MEMORY_BYTES * 2 + 32 };
    uint8_t actual[STORAGE], expected[STORAGE], snapshot[K0_MEMORY_BYTES];
    const unsigned offsets[][2] = {
        {16, 16}, {17, 16}, {16, 17}, {33, 16}, {16, 33},
        {1040, 16}, {16, 1040}, {1041, 16}
    };
    for (unsigned round = 0; round < 8; ++round) {
        for (unsigned c = 0; c < sizeof(offsets) / sizeof(offsets[0]); ++c) {
            unsigned dst = offsets[c][0], src = offsets[c][1];
            for (unsigned i = 0; i < STORAGE; ++i)
                actual[i] = expected[i] = (uint8_t)(next_random(state) >> 32);
            for (unsigned i = 0; i < K0_MEMORY_BYTES; ++i) snapshot[i] = expected[src+i];
            for (unsigned i = 0; i < K0_MEMORY_BYTES; ++i) expected[dst+i] = snapshot[i];
            k0_memcpy_calls = k0_memmove_calls = k0_memset_calls = 0;
            k0_copy_block(actual + dst, actual + src);
            CHECK(K0_REQUIRE_MEMORY_HELPERS < 0 ||
                  (k0_memmove_calls == (uint64_t)K0_REQUIRE_MEMORY_HELPERS &&
                   k0_memcpy_calls == 0 && k0_memset_calls == 0), 27);
            for (unsigned i = 0; i < STORAGE; ++i) CHECK(actual[i] == expected[i], 28);
            k0_result_digest = (k0_result_digest ^ checksum(actual, STORAGE)) * UINT64_C(0x100000001b3);
        }
        unsigned offset = 16 + round;
        for (unsigned i = 0; i < STORAGE; ++i)
            actual[i] = expected[i] = (uint8_t)(next_random(state) >> 32);
        for (unsigned i = 0; i < K0_MEMORY_BYTES; ++i) expected[offset+i] = 0;
        k0_memcpy_calls = k0_memmove_calls = k0_memset_calls = 0;
        k0_zero_block(actual + offset);
        CHECK(K0_REQUIRE_MEMORY_HELPERS < 0 ||
              (k0_memset_calls == (uint64_t)K0_REQUIRE_MEMORY_HELPERS &&
               k0_memcpy_calls == 0 && k0_memmove_calls == 0), 29);
        for (unsigned i = 0; i < STORAGE; ++i) CHECK(actual[i] == expected[i], 30);
        k0_result_digest = (k0_result_digest ^ checksum(actual, STORAGE)) * UINT64_C(0x100000001b3);
    }
    for (unsigned round = 0; round < 16; ++round) {
        uint64_t seed = next_random(state), local = seed;
        for (unsigned i = 0; i < K0_MEMORY_BYTES; ++i)
            snapshot[i] = (uint8_t)(next_random(&local) >> 32);
        k0_memcpy_calls = k0_memmove_calls = k0_memset_calls = 0;
        uint64_t got = k0_copy_owned(seed);
        CHECK(K0_REQUIRE_MEMORY_HELPERS < 0 ||
              (k0_memcpy_calls == (uint64_t)K0_REQUIRE_MEMORY_HELPERS &&
               k0_memmove_calls == 0 && k0_memset_calls == 0), 31);
        CHECK(got == checksum(snapshot, K0_MEMORY_BYTES), 32);
        k0_result_digest = (k0_result_digest ^ got) * UINT64_C(0x100000001b3);
    }
    return 0;
}
uint32_t k0_reference_tests(void) {
    uint8_t bytes[257];
    uint64_t state = UINT64_C(0x1337cafedeadbeef), actual = 0, expected = 0;
    k0_observations = 0;
    k0_result_digest = UINT64_C(0x84222325cbf29ce4);
    CHECK(k0_global_probe() == UINT64_C(0x13579bdf2468ace0), 1);
    CHECK(k0_global_probe() == (UINT64_C(0x13579bdf2468ace0) ^ UINT64_C(0x1122334455667788)), 2);
    for (unsigned i = 0; i < sizeof(bytes); ++i) bytes[i] = (uint8_t)next_random(&state);
    for (unsigned n = 0; n <= sizeof(bytes); ++n) {
        uint64_t got = k0_checksum(bytes, n), want = checksum(bytes, n);
        CHECK(got == want, 3);
        k0_result_digest ^= got;
    }
    /* Reproducible changing inputs ensure no constant-folded oracle can pass. */
    for (unsigned n = 0; n < 1024; ++n) {
        unsigned pos = (unsigned)(next_random(&state) % sizeof(bytes));
        bytes[pos] ^= (uint8_t)(next_random(&state) >> 32);
        uint64_t got = k0_checksum(bytes, sizeof(bytes));
        CHECK(got == checksum(bytes, sizeof(bytes)), 4);
        k0_result_digest = (k0_result_digest ^ got) * UINT64_C(0x100000001b3);
    }
    for (unsigned i = 0; i < 65; ++i) {
        CHECK(k0_take(&actual) == take(&expected), 5);
        CHECK(actual == expected, 6);
    }
    for (unsigned n = 0; n < 2048; ++n) {
        uint64_t r = next_random(&state);
        if (r & UINT64_C(0x40000000)) {
            uint32_t index = (uint32_t)((r >> 32) % 67);
            CHECK(k0_release(&actual, index) == release(&expected, index), 7);
        } else {
            CHECK(k0_take(&actual) == take(&expected), 8);
        }
        CHECK(actual == expected, 9);
        k0_result_digest ^= actual;
    }
    const uint64_t edges[] = {0, 1, 127, 128, 255, 32767, 32768, 65535,
        UINT64_C(0x7fffffff), UINT64_C(0x80000000), UINT64_C(0xffffffff),
        UINT64_C(0x7fffffffffffffff), UINT64_C(0x8000000000000000), UINT64_MAX};
    for (unsigned i = 0; i < sizeof(edges) / sizeof(edges[0]); ++i) {
        uint64_t v = edges[i];
        CHECK(k0_u8((uint8_t)v) == (uint8_t)v, 10);
        CHECK(k0_i8((int8_t)v) == (int8_t)v, 11);
        CHECK(k0_u16((uint16_t)v) == (uint16_t)v, 12);
        CHECK(k0_i16((int16_t)v) == (int16_t)v, 13);
        CHECK(k0_u32((uint32_t)v) == (uint32_t)v, 14);
        CHECK(k0_i32((int32_t)v) == (int32_t)v, 15);
        CHECK(k0_u64(v) == v, 16);
        CHECK(k0_i64((int64_t)v) == (int64_t)v, 17);
        CHECK(k0_echo_pointer((void *)(uintptr_t)v) == (void *)(uintptr_t)v, 18);
        CHECK(k0_checked_bytes(v >> 3) == (v >> 3) * 8, 19);
        CHECK(k0_divide(v, 7) == v / 7, 20);
        CHECK(k0_shift(v, 17) == (v << 17), 21);
    }
    CHECK(k0_signed_divide(INT64_MIN, 7) == INT64_MIN / 7, 22);
    CHECK(k0_high_half() == UINT64_C(0xffffffff80200000), 23);
    CHECK(k0_abi_probe() == 0, 24);
    return memory_tests(&state);
}
_Noreturn void k0_trigger_panic(unsigned reason) {
    if (reason == 1) (void)k0_checked_bytes(UINT64_MAX);
    if (reason == 2) (void)k0_divide(UINT64_MAX, 0);
    if (reason == 3) (void)k0_signed_divide(INT64_MIN, -1);
    if (reason == 4) (void)k0_shift(1, 64);
    for (;;) { __asm__ volatile ("ud2"); }
}
