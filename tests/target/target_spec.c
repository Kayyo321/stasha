#include "target/target.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    sts_target_spec t;
    assert(sts_target_init(&t, "x86_64-unknown-linux-gnu") == 0);
    assert(t.pointer_bits == 64 && t.c_long_bits == 64 && t.format == StsFormatELF);
    assert(sts_target_arch_matches(&t, "amd64") && sts_target_os_matches(&t, "linux"));
    assert(sts_target_init(&t, "x86_64-pc-windows-msvc") == 0);
    assert(t.pointer_bits == 64 && t.c_long_bits == 32 && t.format == StsFormatCOFF);
    assert(sts_target_init(&t, "aarch64-apple-darwin23.0") == 0);
    assert(t.pointer_bits == 64 && t.c_long_bits == 64 && t.format == StsFormatMachO);
    assert(sts_target_arch_matches(&t, "arm64") && sts_target_os_matches(&t, "darwin"));
    assert(sts_target_init(&t, "x86_64-unknown-none-elf") == 0);
    assert(!strcmp(t.os, "none") && t.format == StsFormatELF);
    assert(sts_target_init(&t, "i686-pc-linux-gnu") == 0);
    assert(t.pointer_bits == 32 && t.c_long_bits == 32);
    assert(sts_target_init(&t, "x86_64-unknown-linux-gnux32") == 0);
    assert(t.pointer_bits == 32 && t.c_long_bits == 32);
    assert(sts_target_init(&t, "riscv64-unknown-none-elf") == -1);
    assert(sts_target_init(&t, "x86_64-unknown-mystery") == -1);
    assert(sts_target_init(&t, "") == -1);
    assert(sts_target_init(&t, "x86_64-unknown-none-macho") == -1);
    assert(sts_target_init(&t, "x86_64-unknown-none-coff") == -1);
    assert(sts_target_init(&t, "x86_64-unknown-linux-macho") == -1);
    assert(sts_target_init(&t, "x86_64-unknown-linux-gnu-elf") == 0);
    assert(sts_target_init(&t, "x86_64-unknown-none-xcoff") == -1);
    puts("target descriptor: 14 target cases passed");
}
