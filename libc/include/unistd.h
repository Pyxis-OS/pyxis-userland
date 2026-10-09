#ifndef LIBC_UNISTD_H
#define LIBC_UNISTD_H

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4

/* Native lookup using the caller's grants, with each owned handle immediately
 * released. F_OK requests no child rights; files R_OK/W_OK require READ/WRITE,
 * directories require ENUMERATE / CREATE|REMOVE. Combined modes require both.
 * X_OK and provider routes fail with ENOTSUP; unknown mode bits with EINVAL.
 * No Unix mode/owner checks or mutation. Success is a time-of-check authority
 * observation, not a guarantee of later lookup, I/O, or executable launch. */
int access(const char *path, int mode);
/* Remove one empty directory through parent REMOVE authority. Wrong type,
 * nonempty and missing names fail with ENOTDIR, ENOTEMPTY and ENOENT.
 * Roots and final . or .. are refused; no recursive removal. */
int rmdir(const char *path);

/* Validate descriptor/access first, then reject counts above SSIZE_MAX with
 * EINVAL. Zero count touches no buffer or backend. Nonempty calls return one
 * transfer, including short progress, independently of FILE indicators. A read
 * first returns bytes the associated FILE read ahead, without a backend call;
 * read itself never reads ahead. */
ssize_t read(int descriptor, void *buffer, size_t count);
ssize_t write(int descriptor, const void *buffer, size_t count);
/* Files only; use offset without changing the private descriptor position.
 * Validate descriptor/access and count as above, then reject negative offsets
 * with EINVAL and consoles/pipes with ESPIPE, including zero-count requests.
 * pread bypasses read-ahead without consuming or filling it. A nonempty pwrite
 * discards file read-ahead before the native write, even on an uncertain outcome;
 * it uses offset independently of a FILE's append policy. Neither call changes
 * FILE indicators or pushback. Zero count touches no buffer or backend. */
ssize_t pread(int descriptor, void *buffer, size_t count, off_t offset);
ssize_t pwrite(int descriptor, const void *buffer, size_t count, off_t offset);
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

#ifdef __cplusplus
}
#endif

#endif
