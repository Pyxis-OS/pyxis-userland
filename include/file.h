#ifndef USERSPACE_FILE_H
#define USERSPACE_FILE_H

#include <abi/handle.h>
#include <stddef.h>

/* Return zero on success, -1 on syscall or malformed-reply failure. Output
 * values are cleared on failure. Callers provide valid output storage. */
int file_size(handle_t file, uint64_t *size);

/* Reads up to capacity bytes at an explicit offset. Short reads are allowed;
 * zero with nonzero capacity means EOF. Zero capacity does not touch bytes.
 * No shared seek position is changed. */
int file_read(handle_t file, uint64_t offset, void *bytes, size_t capacity,
              size_t *read);

#endif
