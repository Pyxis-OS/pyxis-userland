#ifndef USERSPACE_ENDPOINT_H
#define USERSPACE_ENDPOINT_H

#include <abi/endpoint.h>
#include <abi/handle.h>
#include <abi/syscall.h>

/* CREATE returns receiver ownership to this process and a transferable caller
 * grant. Close the receiver to shut down outstanding and future calls. */
enum call_status endpoint_create(handle_t service, struct endpoint_create_reply *endpoints);

/* CALL and REPLY borrow bytes and source grants until return. A successful CALL
 * returns an owned packet with reply grants; size is the actual payload length,
 * result is the opaque application result, and receipt is zero. On failure,
 * delivery says whether the receiver obtained the call if output was validated.
 * A queued call can still be NOT_DELIVERED when the endpoint closes. Do not
 * retry a delivered call merely because its transport failed. */
enum call_status endpoint_request(handle_t caller, const void *bytes, size_t size,
    const struct endpoint_grant *grants, size_t grant_count, struct endpoint_packet *reply);

/* RECEIVE returns an owned receipt and zero to four owned attachment handles.
 * The receiver may retain the receipt while receiving more work. Successful
 * REPLY consumes it automatically; CLOSE abandons an unanswered call. Closing
 * the receipt never closes separately delivered attachments. */
enum call_status endpoint_receive(handle_t receiver, struct endpoint_packet *request);
enum call_status endpoint_reply(handle_t receipt, uint64_t application_result,
    const void *bytes, size_t size, const struct endpoint_grant *grants,
    size_t grant_count);

#endif
