#ifndef USERSPACE_UDP_H
#define USERSPACE_UDP_H

#include <abi/udp.h>
#include <abi/syscall.h>

/* Host-order IPv4/port; zero port selects an ephemeral binding. OPEN returns
 * an owned handle with UDP_RIGHTS. Copy/restrict it with the handle helpers.
 * Failure clears output (including setting the handle to HANDLE_INVALID).
 * No helper discovers authority or selects a local address implicitly. */
enum call_status udp_open(handle_t service, uint32_t address, uint16_t port,
    struct udp_open_reply *reply);
enum call_status udp_inspect(handle_t endpoint, struct udp_endpoint_info *reply);
/* Idempotently stops the shared endpoint and releases its binding. Closing a
 * copied handle alone does not stop it. Inspect remains available afterward. */
enum call_status udp_shutdown(handle_t endpoint);

#endif
