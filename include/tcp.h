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
/* LISTEN requires separate service authority and binds an exact local address.
 * ACCEPT returns an owned stream independent of the listener's lifetime. */
enum call_status tcp_listen(handle_t service, uint32_t address, uint16_t port,
    struct tcp_listen_reply *reply);
enum call_status tcp_listener_inspect(handle_t listener, struct tcp_listener_info *reply);
enum call_status tcp_accept(handle_t listener, uint64_t deadline_ns,
    struct tcp_accept_reply *reply);
enum call_status tcp_inspect(handle_t stream, struct tcp_connection_info *reply);
/* Idempotent shared abort. INSPECT remains available afterward. */
enum call_status tcp_abort(handle_t stream);
/* Commit shared write shutdown after accepted data; reads stay usable. This
 * does not wait for delivery. Repeating a committed shutdown succeeds. */
enum call_status tcp_shutdown_write(handle_t stream);

/* Capacity is clamped to TCP_READ_MAX_BYTES. Timeout preserves queued bytes;
 * zero capacity validates authority/deadline but does not poll stream state. */
enum call_status tcp_read(handle_t stream, void *data, size_t capacity,
    uint64_t deadline_ns, struct tcp_read_reply *reply);

/* Success means copied into send storage, not acknowledged by the peer. Retry
 * only the unaccepted suffix. Length is clamped to TCP_WRITE_MAX_BYTES. */
enum call_status tcp_write(handle_t stream, const void *data, size_t length,
    uint64_t deadline_ns, struct tcp_write_reply *reply);

/* One worker attempt; no network-readiness wait or pending operation remains
 * after WOULD_BLOCK. A stale readiness event can normally produce WOULD_BLOCK.
 * Transfer limits, short progress, EOF and failure outputs match blocking calls. */
enum call_status tcp_try_accept(handle_t listener, struct tcp_accept_reply *reply);
enum call_status tcp_try_read(handle_t stream, void *data, size_t capacity,
    struct tcp_read_reply *reply);
enum call_status tcp_try_write(handle_t stream, const void *data, size_t length,
    struct tcp_write_reply *reply);

#endif
