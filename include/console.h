#ifndef USERSPACE_CONSOLE_H
#define USERSPACE_CONSOLE_H

#include <abi/handle.h>
#include <stddef.h>

/* Unbuffered capability output. Returns zero on success, -1 on syscall or
 * malformed-reply failure. *written is zero on failure and the actual transfer
 * count on success; a nonempty write always makes progress. */
int console_write(handle_t output, const void *bytes, size_t size, size_t *written);

/* Valid NUL-terminated string, no added newline. Handles partial writes and
 * returns zero on completion or -1 on the first failure. */
int console_print(handle_t output, const char *text);

#endif
