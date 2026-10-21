#ifndef USERSPACE_WIFI_AUTH_H
#define USERSPACE_WIFI_AUTH_H

#include <abi/wifi.h>
#include <abi/syscall.h>
#include <stdbool.h>
#include <stddef.h>

/* Trusted startup only. The returned RUN and OBSERVE pair publishes atomically;
 * only the selected leaf claims RUN. Neither grant owns a radio link. */
enum call_status wifi_auth_open(handle_t radio, struct wifi_auth_open_reply *pair);
enum call_status wifi_auth_claim(handle_t auth);
enum call_status wifi_auth_status(handle_t auth, struct wifi_auth_status_reply *status);
/* Nonblocking; a BEGIN transfers one credential FILE under caller ownership. */
enum call_status wifi_auth_read(handle_t auth, struct wifi_auth_event *event);
enum call_status wifi_auth_send(handle_t auth, uint64_t generation,
    const void *data, size_t bytes, uint32_t flags);
enum call_status wifi_auth_key(handle_t auth, uint64_t generation, bool group,
    uint32_t key_id, uint32_t action, uint64_t receive_pn, const uint8_t key[16]);
enum call_status wifi_auth_authorize(handle_t auth, uint64_t generation, bool enabled);
enum call_status wifi_auth_fail(handle_t auth, uint64_t generation, enum call_status status);

#endif
