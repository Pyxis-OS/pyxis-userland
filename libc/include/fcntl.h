#ifndef LIBC_FCNTL_H
#define LIBC_FCNTL_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_CREAT 0x100
#define O_TRUNC 0x200
#define O_EXCL 0x400

/* Select exactly one access mode: O_RDONLY, O_WRONLY or O_RDWR. O_CREAT/O_TRUNC
 * require writable access; O_EXCL requires O_CREAT and creates a new file or
 * fails with EEXIST, without opening or truncating an existing name. Invalid
 * access combinations and unknown flags fail with EINVAL before lookup or
 * reading the variadic argument. O_CREAT requires mode_t: only 0666 is accepted
 * as native creation policy, not Unix permissions. Other modes fail with
 * ENOTSUP before lookup even if the file exists. Authority always comes from
 * the caller's grants. Public append is absent. */
int open(const char *path, int flags, ...);

#ifdef __cplusplus
}
#endif

#endif
