OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)
PF_EXECUTE = 1;
PF_WRITE = 2;
PF_READ = 4;
USER_LINK_BASE = 0x400000;
USER_PAGE_SIZE = 4096;

PHDRS {
  text PT_LOAD FLAGS(PF_READ | PF_EXECUTE);
  rodata PT_LOAD FLAGS(PF_READ);
  data PT_LOAD FLAGS(PF_READ | PF_WRITE);
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
