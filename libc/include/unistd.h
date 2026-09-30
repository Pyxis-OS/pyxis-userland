#ifndef LIBC_UNISTD_H
#define LIBC_UNISTD_H

#include <stddef.h>
#include <sys/types.h>

#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

/* Validate descriptor/access first, then reject counts above SSIZE_MAX with
 * EINVAL. Zero count touches no buffer or backend. Nonempty calls return one
 * transfer, including short progress, independently of FILE indicators. A read
 * first returns bytes the associated FILE read ahead, without a backend call;
 * read itself never reads ahead. */
ssize_t read(int descriptor, void *buffer, size_t count);
ssize_t write(int descriptor, const void *buffer, size_t count);
/* Invalidates any FILE association even on release failure; never retries. */
int close(int descriptor);

#endif
