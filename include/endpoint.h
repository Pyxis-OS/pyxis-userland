#ifndef USERSPACE_ENDPOINT_H
#define USERSPACE_ENDPOINT_H

#include <abi/endpoint.h>
#include <abi/handle.h>
#include <abi/syscall.h>

/* CREATE returns receiver ownership to this process and a transferable client
 * grant. Close the receiver to shut down outstanding and future deliveries. */
enum call_status endpoint_create(handle_t service, struct endpoint_create_reply *endpoints);

/* Export one resource behind the owned receiver. Resource rights and transport
 * authority are separate masks; the provider keeps withdrawal control. */
enum call_status endpoint_export(handle_t service, handle_t receiver,
    uint64_t object_id, uint64_t protocol, uint64_t rights, uint64_t transport,
    handle_t *client);
enum call_status endpoint_withdraw(handle_t receiver, uint64_t object_id);
enum call_status endpoint_retire_ack(handle_t receiver, uint64_t object_id);

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

/* RECEIVE returns a kernel-authenticated CALL or SEND kind, an owned receipt,
 * and zero to four owned attachment handles. CANCEL names an already owned
 * CALL receipt: it is an alias, not another grant or handle to close. It has
 * no payload, attachments or result and carries a timeout or closure reason. The
 * RETIRE notice has no receipt and needs endpoint_retire_ack() before ID reuse. The
 * receiver may retain receipts while receiving more work. Successful REPLY
 * consumes a CALL receipt automatically. Finish SEND or canceled CALL with
 * endpoint_finish(); separately delivered attachments remain owned. Closing
 * an unanswered CALL receipt abandons the call. */
enum call_status endpoint_receive(handle_t receiver, struct endpoint_packet *request);
enum call_status endpoint_reply(handle_t receipt, uint64_t application_result,
    const void *bytes, size_t size, const struct endpoint_grant *grants,
    size_t grant_count);
enum call_status endpoint_finish(handle_t receipt);

#endif
