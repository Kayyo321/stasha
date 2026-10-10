#ifndef STASHA_K0_COMPONENT_H
#define STASHA_K0_COMPONENT_H
#include <stdint.h>
#include <stddef.h>
#ifdef K0_ARCHIVE_ABI
#define K0_LINK(name) __asm__("k0_component__" #name)
#else
#define K0_LINK(name)
#endif
uint64_t k0_checksum(const uint8_t *, uint64_t) K0_LINK(k0_checksum);
uint32_t k0_take(uint64_t *) K0_LINK(k0_take);
uint32_t k0_release(uint64_t *, uint32_t) K0_LINK(k0_release);
uint64_t k0_checked_bytes(uint64_t) K0_LINK(k0_checked_bytes);
uint64_t k0_divide(uint64_t, uint64_t) K0_LINK(k0_divide);
int64_t k0_signed_divide(int64_t, int64_t) K0_LINK(k0_signed_divide);
uint64_t k0_shift(uint64_t, uint64_t) K0_LINK(k0_shift);
uint64_t k0_high_half(void) K0_LINK(k0_high_half);
void *k0_echo_pointer(void *) K0_LINK(k0_echo_pointer);
uint8_t k0_u8(uint8_t) K0_LINK(k0_u8);
int8_t k0_i8(int8_t) K0_LINK(k0_i8);
uint16_t k0_u16(uint16_t) K0_LINK(k0_u16);
int16_t k0_i16(int16_t) K0_LINK(k0_i16);
uint32_t k0_u32(uint32_t) K0_LINK(k0_u32);
int32_t k0_i32(int32_t) K0_LINK(k0_i32);
uint64_t k0_u64(uint64_t) K0_LINK(k0_u64);
int64_t k0_i64(int64_t) K0_LINK(k0_i64);
uint64_t k0_eight(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) K0_LINK(k0_eight);
uint64_t k0_global_probe(void) K0_LINK(k0_global_probe);
/* No aggregate crosses the C ABI; every block has exactly 1,025 bytes. */
#define K0_MEMORY_BYTES 1025
void k0_copy_block(void *, const void *) K0_LINK(k0_copy_block);
void k0_zero_block(void *) K0_LINK(k0_zero_block);
uint64_t k0_copy_owned(uint64_t) K0_LINK(k0_copy_owned);
extern uint64_t k0_memcpy_calls, k0_memmove_calls, k0_memset_calls;
uint64_t k0_abi_probe(void);
uint32_t k0_reference_tests(void);
extern uint64_t k0_observations;
extern uint64_t k0_result_digest;
_Noreturn void k0_trigger_panic(unsigned);
#endif
