#ifndef LIBC_UNISTD_H
#define LIBC_UNISTD_H

#include <stddef.h>
#include <sys/types.h>

#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* Validate descriptor/access first, then reject counts above SSIZE_MAX with
 * EINVAL. Zero count touches no buffer or backend. Nonempty calls return one
 * transfer, including short progress, independently of FILE indicators. A read
 * first returns bytes the associated FILE read ahead, without a backend call;
 * read itself never reads ahead. */
ssize_t read(int descriptor, void *buffer, size_t count);
ssize_t write(int descriptor, const void *buffer, size_t count);
/* Invalidates any FILE association even on release failure; never retries. */
int close(int descriptor);
/* Set a writable file's size through native RESIZE. Growth reads as zero and
 * shrinking discards the tail; the descriptor position is unchanged. Negative
 * lengths and consoles or pipes fail with EINVAL. An uncertain backend outcome
 * is EIO; the size must then be treated as unknown. */
int ftruncate(int descriptor, off_t length);
/* Files only; consoles and pipes fail with ESPIPE. Returns the new position.
 * Seeking past the end is allowed; a later write fills the gap with zeros. */
off_t lseek(int descriptor, off_t offset, int origin);
/* Native SYNC on a file open for writing; see abi/file.h for what success
 * promises. Read-only descriptors fail with EBADF, consoles and pipes with
 * EINVAL. An uncertain outcome is EIO. */
int fsync(int descriptor);
/* Remove a file, resolving path like fopen. Requires REMOVE on the parent.
 * Directories fail with EINVAL; open handles to the file stay valid. */
int unlink(const char *path);

#endif
