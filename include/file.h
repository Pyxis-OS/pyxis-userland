#ifndef USERSPACE_FILE_H
#define USERSPACE_FILE_H

#include <abi/handle.h>
#include <abi/syscall.h>
#include <stddef.h>

/* Return native status; malformed replies yield CALL_BAD_REQUEST. Output values
 * are cleared on failure. Callers provide valid output storage, written after
 * the transfer if it aliases bytes. SIZE accepts either READ or WRITE. */
enum call_status file_size(handle_t file, uint64_t *size);

/* Reads up to capacity bytes at an explicit offset. Short reads are allowed;
 * zero with nonzero capacity means EOF. Zero capacity does not touch bytes.
 * Requires READ. No shared seek position is changed. */
enum call_status file_read(handle_t file, uint64_t offset, void *bytes, size_t capacity,
                            size_t *read);

/* Requires WRITE independently of READ. Success writes all size bytes; failure
 * leaves the file unchanged. Beyond-EOF gaps read as zero. Zero size neither
 * touches bytes nor extends the file. Initrd backing returns CALL_READ_ONLY. */
enum call_status file_write(handle_t file, uint64_t offset, const void *bytes,
                             size_t size, size_t *written);

/* Requires WRITE. Growth exposes zeroes, shrinking discards the tail. Failure
 * leaves contents/size unchanged. Resizing to zero also releases RAM backing. */
enum call_status file_resize(handle_t file, uint64_t size);

#endif
