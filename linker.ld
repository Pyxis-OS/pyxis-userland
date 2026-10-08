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

  /* C++ exception unwinding. libunwind finds the tables through these bounds;
   * the index exists when the link passes --eh-frame-hdr. C code is compiled
   * without unwind tables, so C executables have none. */
  .eh_frame_hdr : {
    __eh_frame_hdr_start = .;
    *(.eh_frame_hdr)
    __eh_frame_hdr_end = .;
  } :rodata
  .eh_frame : {
    __eh_frame_start = .;
    KEEP(*(.eh_frame))
    __eh_frame_end = .;
  } :rodata
  .gcc_except_table : { *(.gcc_except_table .gcc_except_table.*) } :rodata

  . = ALIGN(USER_PAGE_SIZE);
  /* libc runs .init_array before main and .fini_array after exit handlers. */
  .init_array : {
    __init_array_start = .;
    KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
    KEEP(*(.init_array))
    __init_array_end = .;
  } :data
  .fini_array : {
    __fini_array_start = .;
    KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*)))
    KEEP(*(.fini_array))
    __fini_array_end = .;
  } :data
  .data : { *(.data .data.*) } :data
  .bss : { *(.bss .bss.*) *(COMMON) } :data
  /DISCARD/ : { *(.note .note.*) *(.comment) }
}
