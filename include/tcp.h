#ifndef USERSPACE_TCP_H
#define USERSPACE_TCP_H

#include <abi/tcp.h>
#include <stddef.h>

/* Borrow explicit authority; CONNECT returns an owned stream after handshake.
 * Address and port are host-order; deadline uses CLOCK_NOW's monotonic epoch.
 * Helpers leave output untouched on failure. Copy/restrict/close use handle.h.
 * READ may return short; a nonempty read returns zero only for orderly EOF. */
enum call_status tcp_connect(handle_t service, uint32_t address, uint16_t port,
    uint64_t deadline_ns, struct tcp_connect_reply *reply);
enum call_status tcp_inspect(handle_t stream, struct tcp_connection_info *reply);
/* Idempotent shared abort. INSPECT remains available afterward. */
enum call_status tcp_abort(handle_t stream);

/* Capacity is clamped to TCP_READ_MAX_BYTES. Timeout preserves queued bytes;
 * zero capacity validates authority/deadline but does not poll stream state. */
enum call_status tcp_read(handle_t stream, void *data, size_t capacity,
    uint64_t deadline_ns, struct tcp_read_reply *reply);

#endif
