.text
.global threadprobe_entry
.type threadprobe_entry, @function
threadprobe_entry:
  /* Native entry has no return address and RSP is 16-byte aligned. */
  call threadprobe_child
  ud2
.size threadprobe_entry, . - threadprobe_entry
.section .note.GNU-stack,"",@progbits
