/* External C/assembly owns privileged I/O and startup; Stasha has neither
 * volatile/MMIO nor inline-assembly qualification. Polling COM1 is bounded. */
#include "component.h"
#include "run_config.h"
__attribute__((used, section(".k0.keep")))
const uint64_t k0_retention_cookie = UINT64_C(0xabcdef0123456789);
extern uint64_t __stack_bottom;
static inline void out8(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0,%1" : : "a"(value), "Nd"(port));
}
static inline uint8_t in8(uint16_t port) {
    uint8_t value;
    __asm__ volatile ("inb %1,%0" : "=a"(value) : "Nd"(port));
    return value;
}
static _Noreturn void finish(uint32_t code) {
    __asm__ volatile ("outl %0,%1" : : "a"(code), "Nd"((uint16_t)0xf4));
    for (;;) __asm__ volatile ("cli; hlt");
}
static void character(char c) {
    for (unsigned n = 0; n < 1000000; ++n) {
        if (in8(0x3fd) & 0x20) { out8(0x3f8, (uint8_t)c); return; }
    }
    finish(63);
}
static void text(const char *s) { while (*s) character(*s++); }
static void hex(uint64_t value) {
    const char *digits = "0123456789abcdef";
    for (int i = 60; i >= 0; i -= 4) character(digits[(value >> i) & 15]);
}
static void prefix(const char *kind) {
    text(kind); text(" run=" K0_RUN_ID " build=" K0_BUILD_ID);
}
_Noreturn void k0_panic_impl(uint32_t reason) {
    prefix("K0_PANIC"); text(" reason="); hex(reason); character('\n');
    finish(32 + reason);
}
_Noreturn void k0_boot_main(void) {
    out8(0x3f9, 0); out8(0x3fb, 0x80); out8(0x3f8, 1); out8(0x3f9, 0);
    out8(0x3fb, 3); out8(0x3fa, 0xc7); out8(0x3fc, 0x0b);
    prefix("K0_START"); text(" mode="); hex(K0_PANIC_MODE); character('\n');
    if (K0_PANIC_MODE) k0_trigger_panic(K0_PANIC_MODE);
    uint32_t status = k0_reference_tests();
    if (__stack_bottom != UINT64_C(0x5a6b7c8d9eaf1021)) status = 25;
    if (k0_retention_cookie != UINT64_C(0xabcdef0123456789)) status = 26;
    prefix("K0_COMPLETE"); text(" status="); hex(status);
    text(" observations="); hex(k0_observations);
    text(" checksum="); hex(k0_result_digest); character('\n');
    finish(status ? 2 : 1);
}
