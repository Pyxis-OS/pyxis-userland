#ifndef USERSPACE_PIPE_H
#define USERSPACE_PIPE_H

#include <abi/pipe.h>
#include <abi/syscall.h>
#include <stddef.h>

/* Borrow CREATE authority. Both returned endpoints are owned by the caller;
 * close each when its reader or writer role ends. Output is unchanged on failure. */
enum call_status pipe_create(handle_t service, struct pipe_create_reply *reply);
/* As pipe_create, for a pipe that also carries value segments. */
enum call_status pipe_create_values(handle_t service, struct pipe_create_reply *reply);

/* Each call transfers at most the ABI limit and may return short. A nonempty
 * read returns zero only at EOF; zero length is a no-op independent of peer
 * closure. Outputs are cleared on failure. */
enum call_status pipe_read(handle_t reader, void *bytes, size_t capacity, size_t *read);
enum call_status pipe_write(handle_t writer, const void *bytes, size_t length, size_t *written);

/* Borrow the same READ or WRITE authority and use the same limits and output
 * rules as above, without waiting for peer progress. A nonempty try-read returns
 * CALL_WOULD_BLOCK while empty with writers still open; a nonempty try-write
 * returns CALL_WOULD_BLOCK while full with readers still open. Positive progress
 * may be short. Try-read returns zero only after the last writer closes and all
 * bytes drain; try-write returns CALL_ENDPOINT_CLOSED after the last reader
 * closes. Zero length remains a no-op independent of peer closure. */
enum call_status pipe_try_read(handle_t reader, void *bytes, size_t capacity, size_t *read);
enum call_status pipe_try_write(handle_t writer, const void *bytes, size_t length, size_t *written);

/* Reads on a value-capable pipe stop at each value with CALL_PIPE_VALUE. */

/* Moves the owned value into the pipe; on success *value is HANDLE_INVALID.
 * Any refusal leaves *value with the caller; BAD_OPERATION means byte-only.
 * The try form returns CALL_WOULD_BLOCK instead of waiting for a value slot. */
enum call_status pipe_handoff(handle_t writer, handle_t *value);
enum call_status pipe_try_handoff(handle_t writer, handle_t *value);

/* Reports the next segment as PIPE_HEAD_VALUE (with an owned *value),
 * PIPE_HEAD_BYTES or PIPE_HEAD_END. The blocking form waits while the pipe is
 * empty and writers remain; the try form returns CALL_WOULD_BLOCK. A bad reply
 * is CALL_OUTCOME_UNKNOWN: a moved value may have been lost. Outputs are
 * cleared on failure. */
enum call_status pipe_receive(handle_t reader, uint64_t *head, handle_t *value);
enum call_status pipe_try_receive(handle_t reader, uint64_t *head, handle_t *value);

#endif
