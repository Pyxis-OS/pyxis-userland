/* Eight 64-bit slots in the public jmp_buf storage. */
.equ JMP_RBX, 0
.equ JMP_RBP, 8
.equ JMP_R12, 16
.equ JMP_R13, 24
.equ JMP_R14, 32
.equ JMP_R15, 40
.equ JMP_RSP, 48
.equ JMP_RIP, 56

.section .text
.global setjmp
.type setjmp, @function
setjmp:
  mov %rbx, JMP_RBX(%rdi)
  mov %rbp, JMP_RBP(%rdi)
  mov %r12, JMP_R12(%rdi)
  mov %r13, JMP_R13(%rdi)
  mov %r14, JMP_R14(%rdi)
  mov %r15, JMP_R15(%rdi)

  /* Resume as though this call returned: exclude its pushed return address. */
  lea 8(%rsp), %rax
  mov %rax, JMP_RSP(%rdi)
  mov (%rsp), %rax
  mov %rax, JMP_RIP(%rdi)

  xor %eax, %eax
  ret
.size setjmp, . - setjmp

.global longjmp
.type longjmp, @function
longjmp:
  mov %esi, %eax
  test %eax, %eax
  jnz .Lrestore_context
  mov $1, %eax

.Lrestore_context:
  /* C keeps the FP environment from the longjmp call, not the setjmp call. */
  mov JMP_RBX(%rdi), %rbx
  mov JMP_RBP(%rdi), %rbp
  mov JMP_R12(%rdi), %r12
  mov JMP_R13(%rdi), %r13
  mov JMP_R14(%rdi), %r14
  mov JMP_R15(%rdi), %r15
  mov JMP_RIP(%rdi), %rdx
  mov JMP_RSP(%rdi), %rsp
  jmp *%rdx
.size longjmp, . - longjmp

.section .note.GNU-stack,"",@progbits
