#ifndef USERSPACE_UDP_H
#define USERSPACE_UDP_H

#include <abi/udp.h>
#include <abi/syscall.h>
#include <stddef.h>

/* Host-order IPv4/port; zero port selects an ephemeral binding. OPEN returns
 * an owned handle with UDP_RIGHTS. Copy/restrict it with the handle helpers.
 * Failure clears output (including setting the handle to HANDLE_INVALID).
 * Neither helper discovers authority. */
enum call_status udp_open(handle_t service, uint32_t address, uint16_t port,
    struct udp_open_reply *reply);
/* Select a source through the route to destination, then bind atomically with
 * respect to configuration changes. Same port, ownership and output rules as
 * udp_open. This does not connect the endpoint or restrict subsequent peers. */
enum call_status udp_open_route(handle_t service, uint32_t destination, uint16_t port,
    struct udp_open_reply *reply);
/* Requires UDP_SERVICE_RIGHT_BROADCAST. Binds wildcard net0 across address
 * changes; sends use the current net0 source, including zero before assignment.
 * Same handle ownership and failure output rules as udp_open. */
enum call_status udp_open_broadcast(handle_t service, uint16_t port,
    struct udp_open_reply *reply);
enum call_status udp_inspect(handle_t endpoint, struct udp_endpoint_info *reply);
/* Idempotently stops the shared endpoint and releases its binding. Closing a
 * copied handle alone does not stop it. Inspect remains available afterward. */
enum call_status udp_shutdown(handle_t endpoint);

/* One complete datagram per call, up to UDP_MAX_PAYLOAD bytes, including zero.
 * Deadlines are absolute monotonic nanoseconds (see clock.h), bounded by the
 * corresponding UDP_*_MAX_WAIT_NS. Successful send means local acceptance.
 * Receive clears metadata on failure and preserves data. Its metadata and data
 * buffers must not overlap (overlap is rejected without modifying either).
 * BUFFER_TOO_SMALL leaves the datagram queued.
 * Calls on shared endpoints allow one send and one receive concurrently. */
enum call_status udp_send(handle_t endpoint, uint32_t address, uint16_t port,
    const void *data, size_t length, uint64_t deadline_ns);
enum call_status udp_receive(handle_t endpoint, void *data, size_t capacity,
    uint64_t deadline_ns, struct udp_receive_reply *reply);

#endif
