.set SYSCALL_PUTCHAR, 0

.section .text
.global _start
.type _start, @function
_start:
  mov $SYSCALL_PUTCHAR, %eax
  mov $'A', %edi
  syscall
  /* Deliberate CPL3 #GP(0), matching the original kernel byte-array example. */
  hlt
.size _start, . - _start

.section .note.GNU-stack,"",@progbits
