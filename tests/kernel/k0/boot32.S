/* Auditable multiboot-v1, 32-bit entry shim. No BIOS services, heap or device
 * drivers. QEMU's pinned direct Multiboot loader supplies flat 32-bit protected
 * mode; we install our own GDT, page tables, stack and 64-bit state.
 * Identity-map physical 0..1 GiB and alias it at ffffffff80000000 with 2 MiB
 * pages. This is a test mapping, not an OS memory-management design. */
.section .multiboot,"a"
.balign 4
.long 0x1badb002, 0x00000003, -(0x1badb002 + 0x00000003)
.section .text.boot,"ax"
.code32
.globl _start32
_start32:
    cli
    cld
    cmp $0x2badb002, %eax
    jne .Lhalt32
    mov $boot_stack_top, %esp
    /* Do not rely on prior boot RAM contents for table construction. */
    mov $pml4, %edi
    xor %eax, %eax
    mov $(4 * 4096 / 4), %ecx
    rep stosl
    mov $(pdpt_low + 3), %eax
    mov %eax, pml4
    mov $(pdpt_high + 3), %eax
    mov %eax, pml4 + 511 * 8
    mov $(page_directory + 3), %eax
    mov %eax, pdpt_low
    mov %eax, pdpt_high + 510 * 8
    mov $page_directory, %edi
    mov $0x83, %eax
    mov $512, %ecx
.Lpages:
    mov %eax, (%edi)
    add $0x200000, %eax
    add $8, %edi
    loop .Lpages
    lgdt gdt_descriptor
    mov %cr4, %eax
    or $0x20, %eax
    mov %eax, %cr4
    mov $pml4, %eax
    mov %eax, %cr3
    mov $0xc0000080, %ecx
    rdmsr
    or $0x100, %eax
    wrmsr
    mov %cr0, %eax
    and $0xfffffffb, %eax
    or $0x80000001, %eax
    mov %eax, %cr0
    ljmp $8, $entry64_bridge
.Lhalt32:
    hlt
    jmp .Lhalt32
.code64
entry64_bridge:
    mov $16, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    xor %eax, %eax
    mov %ax, %fs
    mov %ax, %gs
    movabs $K0_PAYLOAD_ENTRY, %rax
    jmp *%rax
.balign 8
gdt:
    .quad 0
    .quad 0x00af9a000000ffff
    .quad 0x00cf92000000ffff
gdt_descriptor:
    .word . - gdt - 1
    .long gdt
.section .bss,"aw",@nobits
.balign 4096
pml4: .skip 4096
pdpt_low: .skip 4096
pdpt_high: .skip 4096
page_directory: .skip 4096
.balign 16
boot_stack: .skip 16384
boot_stack_top:
.section .note.GNU-stack,"",@progbits
