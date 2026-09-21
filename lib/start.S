.section .text
.global _start
.type _start, @function
_start:
  /* The kernel supplies a 16-byte-aligned RSP. CALL pushes the return address
   * required by the C ABI. Clear inherited frame linkage and string direction. */
  xor %ebp, %ebp
  cld
  /* Preserve the kernel's startup-record pointer in RDI as main's argument. */
  call main

  mov %eax, %edi
  call exit
.size _start, . - _start

.section .note.GNU-stack,"",@progbits
