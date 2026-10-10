.section .text.entry,"ax"
.globl _k0_entry
.type _k0_entry,@function
_k0_entry:
    cli
    cld
    /* Poison first so fresh emulator zero-fill cannot hide broken BSS clearing.
     * This deliberate qualification-only step is not required by the ABI. */
    lea __bss_start(%rip), %rdi
    lea __bss_end(%rip), %rcx
    sub %rdi, %rcx
    mov $0xa5, %eax
    rep stosb
    /* Explicit BSS initialization is part of this reviewed external startup. */
    lea __bss_start(%rip), %rdi
    lea __bss_end(%rip), %rcx
    sub %rdi, %rcx
    xor %eax, %eax
    rep stosb
    lea __stack_top(%rip), %rsp
    and $-16, %rsp
    xor %ebp, %ebp
    movabs $0x5a6b7c8d9eaf1021, %rax
    mov %rax, __stack_bottom(%rip)
    call k0_boot_main
.Lunexpected_return:
    ud2
    hlt
    jmp .Lunexpected_return
.size _k0_entry, .-_k0_entry
.section .stack,"aw",@nobits
.balign 16
.globl __stack_bottom, __stack_top
__stack_bottom: .skip 65536
__stack_top:
.section .note.GNU-stack,"",@progbits
