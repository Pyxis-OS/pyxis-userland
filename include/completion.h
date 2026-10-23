#ifndef USERSPACE_COMPLETION_H
#define USERSPACE_COMPLETION_H

#include <abi/completion.h>
#include <abi/syscall.h>
#include <stddef.h>

/* SUBMIT copies the payload before returning admission status. Correlation IDs
 * are nonzero and increase without reuse during the owned receiver's lifetime.
 * Concurrent operations share a separate 16 MiB process metadata allowance,
 * across all receivers and native backends; exhaustion refuses with CALL_LIMIT
 * before acceptance. Each receiver still has ENDPOINT_DELIVERIES_MAX slots.
 * Collect ENDPOINT_MESSAGE_COMPLETION with endpoint_receive(); the attached
 * grants are independent, and closing its receipt acknowledges the operation.
 * Charges remain until actual metadata reclamation after ACK or uncollected
 * work drain; receiver closure cannot bypass pending ownership. */
enum call_status completion_submit(handle_t target, handle_t receiver,
    uint64_t correlation_id, uint64_t protocol, uint64_t operation,
    uint64_t deadline_ns, const void *payload, size_t size);

/* Cancellation requests safe interruption, never rollback or result collection.
 * Terminal results remain available; repeated requests are idempotent until ACK.
 * Actual result/progress and cancellation reason are separate packet fields.
 * Forwarder CUT before native work starts reports DENIED as its reason. */
enum call_status completion_cancel(handle_t receiver, uint64_t correlation_id);

#endif
