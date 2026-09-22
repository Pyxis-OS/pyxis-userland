#ifndef LIBC_ERRNO_H
#define LIBC_ERRNO_H

/* One thread per process today; this must become thread-local with threads. */
extern int errno;

#define ENOMEM 1
#define EINVAL 2
#define EOVERFLOW 3

#endif
