#ifndef USERSPACE_ENDPOINT_H
#define USERSPACE_ENDPOINT_H

#include <abi/endpoint.h>
#include <abi/describe.h>
#include <abi/handle.h>
#include <abi/syscall.h>

/* CREATE returns receiver ownership to this process and a transferable client
 * grant. Close the receiver to shut down outstanding and future deliveries. */
enum call_status endpoint_create(handle_t service, struct endpoint_create_reply *endpoints);

/* Export one resource behind the owned receiver. Resource rights and transport
 * authority are separate masks. Keep the returned CUT controller until RETIRE
 * or provider teardown; closing its last logical grant cuts the export. */
enum call_status endpoint_export(handle_t service, handle_t receiver,
    uint64_t object_id, uint64_t protocol, uint64_t rights, uint64_t transport,
    handle_t *client, handle_t *controller);
enum call_status endpoint_retire_ack(handle_t receiver, uint64_t object_id);

/* Publish an immutable description on the issuer-owned CONTROL receiver.
 * Optional bounded body bytes are copied into an independent native snapshot;
 * ERROR_BODY includes a zero-byte body. Failure preserves the prior snapshot.
 * read_file may lend raw native RAM with READ|WRITE for a completed READ-only
 * FILE export. Publication seals it and enables kernel-serviced READ; export
 * lifetime, metadata and revocation remain unchanged. Error bodies never use
 * this backing. Zero leaves byte operations with the provider. */
enum call_status endpoint_describe_publish(handle_t receiver, uint64_t object_id,
    const struct resource_description *description, const void *error_body,
    size_t error_body_size, handle_t read_file);
/* Owner CONTROL query; cumulative successful local READ bytes until RETIRE_ACK. */
enum call_status endpoint_read_bytes(handle_t receiver, uint64_t object_id, uint64_t *bytes);

/* CALL, SEND and REPLY borrow bytes and source grants until return. A successful CALL
 * returns an owned packet with reply grants; size is the actual payload length,
 * result is the opaque application result, and receipt is zero. On failure,
 * delivery says whether the receiver obtained the call if output was validated.
 * A queued call can still be NOT_DELIVERED when the endpoint closes. deadline_ns
 * is absolute monotonic time; zero waits without a deadline. An expired call
 * returns TIMED_OUT and keeps its delivery state and original deadline. Do not
 * retry a delivered call merely because its transport failed. */
enum call_status endpoint_request(handle_t caller, const void *bytes, size_t size,
    const struct endpoint_grant *grants, size_t grant_count, uint64_t deadline_ns,
    struct endpoint_packet *reply);

/* Invoke a resource through its exported grant. The kernel authenticates the
 * object ID and actual rights delivered to the provider. */
enum call_status endpoint_invoke(handle_t client, uint64_t protocol,
    uint64_t operation, const void *bytes, size_t size,
    const struct endpoint_grant *grants, size_t grant_count, uint64_t deadline_ns,
    struct endpoint_packet *reply);

/* SEND returns after admission. Success does not report provider execution.
 * The endpoint retains attached references after sender close or exit. */
enum call_status endpoint_send(handle_t caller, const void *bytes, size_t size,
    const struct endpoint_grant *grants, size_t grant_count);
enum call_status endpoint_notify(handle_t client, uint64_t protocol,
    uint64_t operation, const void *bytes, size_t size,
    const struct endpoint_grant *grants, size_t grant_count);

/* RECEIVE returns a kernel-authenticated kind, an owned receipt,
 * and zero to four owned attachment handles. CANCEL names an already owned
 * CALL receipt: it is an alias, not another grant or handle to close. It has
 * no payload, attachments or result and carries a timeout or closure reason. The
 * RETIRE notice has no receipt and needs endpoint_retire_ack() before ID reuse. The
 * receiver may retain receipts while receiving more work. Successful REPLY
 * consumes a CALL receipt automatically. Finish SEND or canceled CALL with
 * endpoint_finish(); separately delivered attachments remain owned. Closing
 * an unanswered CALL receipt abandons the call. Native COMPLETION carries the
 * correlation ID, actual result/progress and separate cancellation reason.
 * Pending native work stopped by forwarder CUT reports DENIED as its reason.
 * Closing its zero-right receipt acknowledges that operation; attached native
 * grants remain independent. Failed collection leaves completion work queued
 * for retry without replaying the native operation. */
enum call_status endpoint_receive(handle_t receiver, struct endpoint_packet *request);

/* Same collection/receipt/authority as RECEIVE. An empty open receiver returns
 * WOULD_BLOCK without parking for a message, including after stale readiness.
 * Closure returns ENDPOINT_CLOSED; failure clears request. Ordinary grant-table
 * admission/growth remains unchanged when a real message is selected. */
enum call_status endpoint_try_receive(handle_t receiver, struct endpoint_packet *request);
enum call_status endpoint_reply(handle_t receipt, uint64_t application_result,
    const void *bytes, size_t size, const struct endpoint_grant *grants,
    size_t grant_count);
enum call_status endpoint_finish(handle_t receipt);

#endif
