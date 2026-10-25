#ifndef USERSPACE_COMPLETION_H
#define USERSPACE_COMPLETION_H

#include <abi/completion.h>
#include <abi/file.h>
#include <abi/syscall.h>
#include <stddef.h>

/* SUBMIT copies the payload before returning admission status. Correlation IDs
 * are nonzero and increase without reuse during the owned receiver's lifetime.
 * Concurrent operations share a per-process kernel metadata allowance (see
 * Pyxis docs/interfaces/native-completion.md), across receivers and native
 * backends; exhaustion refuses with CALL_LIMIT
 * before acceptance. Each receiver still has ENDPOINT_DELIVERIES_MAX slots.
 * Collect ENDPOINT_MESSAGE_COMPLETION with endpoint_receive(); the attached
 * grants are independent, and closing its receipt acknowledges the operation.
 * Charges remain until actual metadata reclamation after ACK or uncollected
 * work drain; receiver closure cannot bypass pending ownership. */
enum call_status completion_submit(handle_t target, handle_t receiver,
    uint64_t correlation_id, uint64_t protocol, uint64_t operation,
    uint64_t deadline_ns, const void *payload, size_t size);

/* Explicit native FILE bulk admission, without ordinary-read fallback.
 * Capacity is bounded and offset + capacity cannot wrap. One absolute deadline
 * covers admission and all backend work; zero supplies no caller deadline.
 * Completion contains only file_read_reply metadata. Inspect its native result
 * and independent cancellation reason before endpoint_collect_bulk(), then
 * close the receipt to acknowledge. Data has its own 16 MiB process allowance
 * and remains charged through actual worker/copy/data retirement. Generic
 * completion_submit() may supply an earlier inner FILE deadline; it can only
 * shorten the outer submission deadline. */
enum call_status completion_submit_file_read_bulk(handle_t target, handle_t receiver,
    uint64_t correlation_id, uint64_t offset, size_t capacity, uint64_t deadline_ns);

/* Cancellation requests safe interruption, never rollback or result collection.
 * Terminal results remain available; repeated requests are idempotent until ACK.
 * Actual result/progress and cancellation reason are separate packet fields.
 * Forwarder CUT before native work starts reports DENIED as its reason. */
enum call_status completion_cancel(handle_t receiver, uint64_t correlation_id);

#endif
