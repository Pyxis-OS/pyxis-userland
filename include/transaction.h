#ifndef USERSPACE_TRANSACTION_H
#define USERSPACE_TRANSACTION_H

#include <abi/transaction.h>
#include <resource.h>

#ifdef __cplusplus
extern "C" {
#endif

/* These helpers borrow control. STAGE owns its returned writable FILE;
 * COMMIT owns the resource result including any independent error body.
 * A failed resource outcome is a valid typed reply, reported through info.
 * The caller latches its commit attempt and never retries a delivered failure.
 * result.delivery preserves the transport's delivery evidence on every path. */
enum call_status transaction_stage(handle_t control, uint64_t deadline_ns,
    handle_t *body);
enum call_status transaction_commit(handle_t control, uint64_t deadline_ns,
    struct resource_result *result);
enum call_status transaction_abort(handle_t control, uint64_t deadline_ns);

#ifdef __cplusplus
}
#endif

#endif
