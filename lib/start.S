.section .text
.global _start
.type _start, @function
_start:
  /* The kernel supplies a 16-byte-aligned RSP. CALL pushes the return address
   * required by the C ABI. Clear inherited frame linkage and string direction. */
  xor %ebp, %ebp
  cld
  /* The C entry consumes RDI, initializes native bindings and calls main. */
  call startup_enter
  ud2
.size _start, . - _start

.section .note.GNU-stack,"",@progbits
