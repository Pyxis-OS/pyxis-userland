OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)
USER_LINK_BASE = 0x400000;
USER_PAGE_SIZE = 4096;

/* ELF segment flags: PF_X = 1, PF_W = 2, PF_R = 4. LLD evaluates FLAGS()
 * before symbol assignments, so the values are written as numbers. */
PHDRS {
  text PT_LOAD FLAGS(5);   /* PF_R | PF_X */
  rodata PT_LOAD FLAGS(4); /* PF_R */
  data PT_LOAD FLAGS(6);   /* PF_R | PF_W */
}
SECTIONS {
  . = USER_LINK_BASE;
  .text : { *(.text .text.*) } :text
  . = ALIGN(USER_PAGE_SIZE);
  .rodata : { *(.rodata .rodata.*) } :rodata
  . = ALIGN(USER_PAGE_SIZE);
  .data : { *(.data .data.*) } :data
  .bss : { *(.bss .bss.*) *(COMMON) } :data
  /DISCARD/ : { *(.eh_frame*) *(.note .note.*) *(.comment) }
}
