#ifndef USERSPACE_CONSOLE_H
#define USERSPACE_CONSOLE_H

#include <abi/handle.h>
#include <abi/syscall.h>
#include <stddef.h>

/* Unbuffered capability output. Preserves native call statuses. *written is
 * zero on failure and the actual transfer count on success; a nonempty write
 * always makes progress. */
enum call_status console_write(handle_t output, const void *bytes, size_t size, size_t *written);

/* Repeats partial writes. Returns CALL_OK once all bytes are written, or the
 * failure status; earlier writes may already be visible. No terminator needed. */
enum call_status console_write_all(handle_t output, const void *bytes, size_t size);

/* Valid NUL-terminated string, no added newline. Handles partial writes and
 * returns CALL_OK on completion or the first failure status. */
enum call_status console_print(handle_t output, const char *text);

/* Native statuses are preserved, including INPUT_LOST and UNAVAILABLE.
 * Nonempty READ blocks, may return a short byte sequence, and never reports EOF.
 * Zero capacity succeeds without waiting or acknowledging input loss. */
enum call_status console_read(handle_t input, void *bytes, size_t capacity, size_t *read);

/* Dimensions in character cells; requires either READ or WRITE authority. */
enum call_status console_size(handle_t console, size_t *columns, size_t *rows);

#endif
