#ifndef USERSPACE_WIFI_H
#define USERSPACE_WIFI_H

#include <abi/wifi.h>
#include <abi/syscall.h>
#include <stddef.h>

/* READ observes only; SCAN starts an explicit operation. The operation is local
 * to its owning process; closing requests cancellation and asynchronous drain. */
enum call_status wifi_status(handle_t wifi, struct wifi_status_reply *status);
enum call_status wifi_scan_start(handle_t wifi, uint32_t channels,
    uint64_t deadline_ns, handle_t *operation);
enum call_status wifi_scan_status(handle_t operation,
    struct wifi_scan_status_reply *status);
/* Nonblocking, retryable on local handle admission failure. */
enum call_status wifi_scan_take(handle_t operation, handle_t *file);
enum call_status wifi_scan_cancel(handle_t operation);
/* Trusted boot init only; never transferred to successors. */
enum call_status wifi_install_policy(handle_t wifi, const char country[2],
    uint32_t channels);
/* Copies caller-supplied bytes into an anonymous, sealed, erased-on-release FILE.
 * No environment entry is created. The caller must erase its own input. */
enum call_status wifi_credential_seal(handle_t wifi, uint32_t format,
    const void *bytes, size_t length, handle_t *file);

enum call_status wifi_connect_start(handle_t radio, const struct wifi_target *target, handle_t credential,
    uint64_t deadline_ns, handle_t *operation);
enum call_status wifi_connect_status(handle_t operation, struct wifi_connect_status_reply *status);
enum call_status wifi_connect_take(handle_t operation, handle_t *link);
enum call_status wifi_connect_cancel(handle_t operation);
enum call_status wifi_link_status(handle_t link, struct wifi_link_status_reply *status);
enum call_status wifi_link_disconnect(handle_t link);

#endif
