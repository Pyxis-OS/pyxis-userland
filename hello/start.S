.section .text
.global _start
.type _start, @function
_start:
  /* The kernel supplies a 16-byte-aligned RSP. CALL pushes the return address
   * required by the C ABI. Clear inherited frame linkage and string direction. */
  xor %ebp, %ebp
  cld
  call main
  /* There is no exit syscall yet; returning from main deliberately raises #GP. */
  hlt
.size _start, . - _start

.section .note.GNU-stack,"",@progbits
