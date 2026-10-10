#ifdef K0_ARCHIVE_ABI
#define k0_eight k0_component__k0_eight
#endif
/* SysV scalar ABI probe. Save our caller's nonvolatiles, seed all six registers,
 * pass two stack arguments with a 16-byte-aligned pre-call stack, and verify
 * preserved registers, stack balance and an out-of-frame stack canary afterward.
 * Normal calls only: this does not qualify asynchronous interrupt entry. */
.section .text
.globl k0_abi_probe
.type k0_abi_probe,@function
k0_abi_probe:
    push %rbp
    push %rbx
    push %r12
    push %r13
    push %r14
    push %r15
    sub $24, %rsp
    movq $7, (%rsp)
    movq $8, 8(%rsp)
    movabs $0x5566778899aabbcc, %rax
    mov %rax, 16(%rsp)
    mov $0x11223344, %ebp
    mov $0x22334455, %ebx
    mov $0x33445566, %r12d
    mov $0x44556677, %r13d
    mov $0x55667788, %r14d
    mov $0x66778899, %r15d
    mov $1, %edi
    mov $2, %esi
    mov $3, %edx
    mov $4, %ecx
    mov $5, %r8d
    mov $6, %r9d
    call k0_eight
    cmp $(1 ^ (2<<1) ^ (3<<2) ^ (4<<3) ^ (5<<4) ^ (6<<5) ^ (7<<6) ^ (8<<7)), %rax
    jne .Lfail
    cmp $0x11223344, %rbp
    jne .Lfail
    cmp $0x22334455, %rbx
    jne .Lfail
    cmp $0x33445566, %r12
    jne .Lfail
    cmp $0x44556677, %r13
    jne .Lfail
    cmp $0x55667788, %r14
    jne .Lfail
    cmp $0x66778899, %r15
    jne .Lfail
    movabs $0x5566778899aabbcc, %rax
    cmp %rax, 16(%rsp)
    jne .Lfail
    xor %eax, %eax
    jmp .Ldone
.Lfail:
    mov $1, %eax
.Ldone:
    add $24, %rsp
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbx
    pop %rbp
    ret
.size k0_abi_probe, .-k0_abi_probe
.section .note.GNU-stack,"",@progbits

/* All Stasha -> external-C calls currently permitted by this component are
 * panic calls. Validate the SysV callee entry alignment before tail-jumping
 * into the host/boot's C hook. An alignment violation is a distinct failure. */
.section .text
.globl __stasha_panic
.type __stasha_panic,@function
__stasha_panic:
    mov %rsp, %rax
    and $15, %eax
    cmp $8, %eax
    je .Lpanic_aligned
    mov $255, %edi
.Lpanic_aligned:
    jmp k0_panic_impl
.size __stasha_panic, .-__stasha_panic
