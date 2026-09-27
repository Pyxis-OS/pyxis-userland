#ifndef USERSPACE_ENDPOINT_H
#define USERSPACE_ENDPOINT_H

#include <abi/endpoint.h>
#include <abi/handle.h>
#include <abi/syscall.h>

/* CREATE returns receiver ownership to this process and a transferable client
 * grant. Close the receiver to shut down outstanding and future deliveries. */
enum call_status endpoint_create(handle_t service, struct endpoint_create_reply *endpoints);

/* CALL, SEND and REPLY borrow bytes and source grants until return. A successful CALL
 * returns an owned packet with reply grants; size is the actual payload length,
 * result is the opaque application result, and receipt is zero. On failure,
 * delivery says whether the receiver obtained the call if output was validated.
 * A queued call can still be NOT_DELIVERED when the endpoint closes. Do not
 * retry a delivered call merely because its transport failed. */
enum call_status endpoint_request(handle_t caller, const void *bytes, size_t size,
    const struct endpoint_grant *grants, size_t grant_count, struct endpoint_packet *reply);

/* SEND returns after admission. Success does not report provider execution.
 * The endpoint retains attached references after sender close or exit. */
enum call_status endpoint_send(handle_t caller, const void *bytes, size_t size,
    const struct endpoint_grant *grants, size_t grant_count);

/* RECEIVE returns a kernel-authenticated CALL or SEND kind, an owned receipt,
 * and zero to four owned attachment handles. The receiver may retain the
 * receipt while receiving more work. Successful REPLY consumes a CALL receipt
 * automatically. Finish a SEND receipt with
 * endpoint_finish(); doing so never closes separately delivered attachments.
 * Closing an unanswered CALL receipt abandons the call. */
enum call_status endpoint_receive(handle_t receiver, struct endpoint_packet *request);
enum call_status endpoint_reply(handle_t receipt, uint64_t application_result,
    const void *bytes, size_t size, const struct endpoint_grant *grants,
    size_t grant_count);
enum call_status endpoint_finish(handle_t receipt);

#endif
