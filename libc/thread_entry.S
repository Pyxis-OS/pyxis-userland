.section .text
.global libc_thread_entry
.type libc_thread_entry, @function
libc_thread_entry:
  /* Native entry has aligned RSP but no return address. Establish the C ABI. */
  xor %ebp, %ebp
  cld
  call libc_thread_run
  ud2
.size libc_thread_entry, . - libc_thread_entry
.section .note.GNU-stack,"",@progbits
