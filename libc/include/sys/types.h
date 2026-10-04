#ifndef LIBC_SYS_TYPES_H
#define LIBC_SYS_TYPES_H

/* Pyxis x86-64 LP64 signed byte count. */
typedef signed long ssize_t;

/* Creation-mode argument representation; permission enforcement is deferred. */
typedef unsigned int mode_t;
/* Signed file size and offset for POSIX interfaces such as ftruncate. */
typedef signed long off_t;

#endif
