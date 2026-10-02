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
 * Nonempty READ blocks and may return a short byte sequence. Independent
 * terminals return zero at input EOF; framebuffer consoles have no input EOF.
 * Zero capacity succeeds without waiting or acknowledging input loss. */
enum call_status console_read(handle_t input, void *bytes, size_t capacity, size_t *read);
/* Same transfer contract; 0 polls, UINT32_MAX is the largest finite wait.
 * CALL_TIMED_OUT leaves *read zero and consumes nothing. */
enum call_status console_read_timeout(handle_t input, void *bytes, size_t capacity,
                                     uint32_t timeout_ms, size_t *read);

/* Dimensions in character cells; requires either READ or WRITE authority. */
enum call_status console_size(handle_t console, size_t *columns, size_t *rows);

/* WRITE authority. Advance only when not already at column zero; also discard
 * an incomplete output escape sequence. Does not clear text or reset colors. */
enum call_status console_fresh_line(handle_t output);

/* WRITE authority; 1..32 columns, otherwise CALL_BAD_REQUEST. Affects future
 * tabs on the shared TTY without moving the cursor or changing existing text. */
enum call_status console_set_tab_width(handle_t output, size_t columns);

/* INTERRUPT authority. On success *armed is a new handle that keeps Ctrl+C
 * armed on this input until it closes; observe it with wait_many
 * WAIT_INTERRUPT. CALL_BUSY means the input is already armed, or briefly that
 * another arm request is still installing its handle. Both calls
 * leave the output HANDLE_INVALID on failure. */
enum call_status console_arm_interrupt(handle_t input, handle_t *armed);

/* READ authority. On success *passthrough is a new handle; while any such
 * handle exists, Ctrl+C on this input is ordinary data. Close it to withdraw. */
enum call_status console_passthrough(handle_t input, handle_t *passthrough);

#endif
