#ifndef LIBC_FCNTL_H
#define LIBC_FCNTL_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT 0x100
#define O_TRUNC 0x200

/* O_CREAT/O_TRUNC require O_WRONLY. Unknown flags and read-only mutations fail
 * with EINVAL before lookup. O_CREAT requires a mode_t argument: only 0666 is
 * accepted, meaning native creation policy, not Unix permissions. Other modes
 * fail with ENOTSUP before lookup even if the file exists. Authority always
 * comes from the caller's grants. Public append/read-write opens are absent. */
int open(const char *path, int flags, ...);

#ifdef __cplusplus
}
#endif

#endif
