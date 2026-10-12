.text
.global threadprobe_entry
.type threadprobe_entry, @function
threadprobe_entry:
  /* Native entry has no return address and RSP is 16-byte aligned. */
  call threadprobe_child
  ud2
.size threadprobe_entry, . - threadprobe_entry
.global threadprobe_tls_entry
.type threadprobe_tls_entry, @function
threadprobe_tls_entry:
  call threadprobe_tls_child
  ud2
.size threadprobe_tls_entry, . - threadprobe_tls_entry
.section .note.GNU-stack,"",@progbits
