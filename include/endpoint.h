#ifndef USERSPACE_ENDPOINT_H
#define USERSPACE_ENDPOINT_H

#include <abi/endpoint.h>
#include <abi/handle.h>
#include <abi/syscall.h>

/* Returns native status, including ENDPOINT_CLOSED and QUEUE_FULL. Packet
 * output is cleared on failure. Buffers are borrowed only for the call;
 * grant may be NULL; otherwise copies one source capability with the selected
 * rights. RECEIVE returns a new owned handle that the recipient must close.
 * Ordinary message bytes do not transfer handles or pointed-to data. */
enum call_status endpoint_request(handle_t endpoint, const void *bytes, size_t size,
                                  const struct endpoint_grant *grant,
                                  struct endpoint_packet *reply);
enum call_status endpoint_receive(handle_t endpoint, struct endpoint_packet *request);
enum call_status endpoint_reply(handle_t endpoint, uint64_t id,
                                const void *bytes, size_t size);

#endif
