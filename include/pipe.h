#ifndef USERSPACE_PIPE_H
#define USERSPACE_PIPE_H

#include <abi/pipe.h>
#include <abi/syscall.h>
#include <stddef.h>

/* Borrow CREATE authority. Both returned endpoints are owned by the caller;
 * close each when its reader or writer role ends. Output is unchanged on failure. */
enum call_status pipe_create(handle_t service, struct pipe_create_reply *reply);

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

#endif
