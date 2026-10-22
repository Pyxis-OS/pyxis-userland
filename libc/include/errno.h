#ifndef LIBC_ERRNO_H
#define LIBC_ERRNO_H

#ifdef __cplusplus
extern "C" {
#endif

/* Libc establishes the FS-addressed record before any errno-using code. */
int *__errno_location(void);
#define errno (*__errno_location())

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
#define ENOTTY 26
#define ENAMETOOLONG 27
#define E2BIG 28
/* Compatibility vocabulary; current native calls do not report signal interruption. */
#define EINTR 29
/* Lookup met a symbolic link: Pyxis follows none, with or without O_NOFOLLOW. */
#define ELOOP 30
#define EPERM 31
#define EDEADLK 32
#define ESRCH 33
#define ECANCELED 34
#define EISDIR 35
#define ENOSYS 36

#ifdef __cplusplus
}
#endif

#endif
