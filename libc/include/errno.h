#ifndef LIBC_ERRNO_H
#define LIBC_ERRNO_H

#ifdef __cplusplus
extern "C" {
#endif

/* One thread per process today; this must become thread-local with threads. */
extern int errno;

#define ENOMEM 1
#define EINVAL 2
#define EOVERFLOW 3

#define EBADF 4
#define EACCES 5
#define ENOTSUP 6
#define EFAULT 7
#define ENODEV 8
#define EAGAIN 9
#define EPIPE 10
#define EBUSY 11
#define ENOENT 12
#define EEXIST 13
#define EROFS 14
#define EIO 15
#define ESPIPE 16
#define ETIMEDOUT 17
#define ERANGE 18
#define ENOTEMPTY 19
#define ENOSPC 20
#define EDQUOT 21
#define EFBIG 22
#define EMFILE 23
#define EILSEQ 24
#define ENOTDIR 25

#ifdef __cplusplus
}
#endif

#endif
