OUTPUT_FORMAT(elf32-i386)
OUTPUT_ARCH(i386)
ENTRY(_start32)
PHDRS { boot PT_LOAD FLAGS(5); tables PT_LOAD FLAGS(6); payload PT_LOAD FLAGS(4); }
SECTIONS {
  . = 0x100000;
  .multiboot : { KEEP(*(.multiboot)) } :boot
  .text : { *(.text.boot) } :boot
  . = ALIGN(4096);
  .bss (NOLOAD) : { *(.bss) } :tables
  ASSERT(. < 0x180000, "bootstrap overlaps payload guard")
  . = 0x200000;
  .payload : { KEEP(*(.data)) } :payload
  ASSERT(. < 0x280000, "qualification payload exceeds 512 KiB")
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
