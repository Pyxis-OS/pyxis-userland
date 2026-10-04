#ifndef LIBC_SYS_STAT_H
#define LIBC_SYS_STAT_H

#include <sys/types.h>

/* Create one directory, resolving path like fopen: startup roots and the
 * initial working-directory chain. Requires CREATE on the parent. mode has no
 * effect; native directories carry no permission bits. Missing parents are not
 * created. Returns 0, or -1 with errno (EEXIST if the name exists). */
int mkdir(const char *path, mode_t mode);

#endif
