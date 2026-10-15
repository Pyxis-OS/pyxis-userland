#ifndef USERSPACE_RETENTION_H
#define USERSPACE_RETENTION_H

#include <abi/retention.h>
#include <abi/syscall.h>
#include <stddef.h>

/* Borrow trusted factory authority. Limits are immutable for the new account;
 * grant only account capture authority to trusted session successors. Success
 * returns one owned account, unchanged on failure. No reset/increase operation. */
enum call_status retention_create_account(handle_t factory,
    const struct retention_limits *limits, handle_t *account);
/* Provider-free account limits and current logical retained bytes. */
enum call_status retention_status(handle_t account, struct retention_status_reply *reply);
/* Construct one owned immutable READ FILE with a non-OK outcome and bounded
 * counted UTF-8 detail. No prefix/body is published. Output is unchanged on
 * failure; detail is borrowed only until return. */
enum call_status retention_create_failure(handle_t account, uint64_t outcome,
    const char *detail, size_t detail_size, handle_t *value);

#endif
