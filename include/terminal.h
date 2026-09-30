#ifndef USERSPACE_TERMINAL_H
#define USERSPACE_TERMINAL_H

#include <abi/terminal.h>
#include <abi/syscall.h>

/* Borrow CREATE authority. All three returned handles are caller-owned;
 * application input/output implement CONSOLE with READ/WRITE respectively.
 * Attachment authority is separate. Dimensions are immutable character cells.
 * Helpers preserve native statuses and leave outputs unchanged on failure. */
enum call_status terminal_create(handle_t service, size_t columns, size_t rows,
    struct terminal_create_reply *reply);

/* One queue attempt, without waiting or retaining a pending operation.
 * Injection accepts at most TERMINAL_TRANSFER_MAX bytes and can return short;
 * retry only the unaccepted suffix. Zero length is a validated no-op.
 * Drain copies exactly one complete terminal_record and its payload, or zero
 * at output EOF. BUFFER_TOO_SMALL consumes nothing. Empty live queues report
 * WOULD_BLOCK. The data buffer and reply must not overlap. */
enum call_status terminal_try_inject(handle_t attachment, const void *data, size_t length,
    struct terminal_transfer_reply *reply);
enum call_status terminal_try_drain(handle_t attachment, void *data, size_t capacity,
    struct terminal_transfer_reply *reply);

/* Idempotently preserve queued input, then deliver application EOF. Later
 * nonempty injection reports ENDPOINT_CLOSED. Requires INJECT authority. */
enum call_status terminal_end_input(handle_t attachment);

/* Idempotently discard both queues and close blocked application operations.
 * Requires HANGUP; closing the last HANGUP grant has the same effect. Neither
 * operation terminates applications. */
enum call_status terminal_hangup(handle_t attachment);

#endif
