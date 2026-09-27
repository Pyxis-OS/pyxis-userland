#ifndef LIBC_FCNTL_H
#define LIBC_FCNTL_H

#define O_RDONLY 0

/* Only O_RDONLY is supported. Other flag values fail with EINVAL before path
 * lookup. Paths resolve through the caller's existing capability grants. */
int open(const char *path, int flags, ...);

#endif
