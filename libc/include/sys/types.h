#ifndef LIBC_SYS_TYPES_H
#define LIBC_SYS_TYPES_H

#include <stdint.h>

/* Pyxis x86-64 LP64 signed byte count. */
typedef signed long ssize_t;

/* Creation-mode argument representation; permission enforcement is deferred. */
typedef unsigned int mode_t;
/* Signed file size and offset for POSIX interfaces such as ftruncate. */
typedef signed long off_t;
/* Microsecond durations for usleep, including values above one second. */
typedef unsigned int useconds_t;

/* Native identity components, copied losslessly without host device encoding. */
typedef uint64_t dev_t;
typedef uint64_t ino_t;
typedef uint64_t nlink_t;

#endif
