#ifndef NETWORK_RUNTIME_DHCP_H
#define NETWORK_RUNTIME_DHCP_H

#include <abi/handle.h>
#include <stdbool.h>
#include <stdint.h>

struct dhcp_lease {
  uint32_t address, prefix, gateway, dns, server;
  uint64_t connection_generation;
  uint64_t acquired_ns, renewal_ns, rebind_ns, expires_ns;
};

struct dhcp_client {
  handle_t endpoint, clock, random;
  uint8_t mac[6];
  bool requesting, xid_ready, request_sent;
  uint32_t xid, retry_seconds;
  uint64_t started_ns, retry_ns, request_ns;
  /* Captured when this endpoint's work starts; never refreshed by an ACK. */
  uint64_t connection_generation;
  struct dhcp_lease offer;
  bool refresh_started, refresh_xid_ready, refresh_sent, rebinding;
  uint32_t refresh_xid;
  uint64_t refresh_request_ns, refresh_started_ns, refresh_retry_ns;
};

enum dhcp_result {
  DHCP_ACKNOWLEDGED,
  DHCP_TIMEOUT,
  DHCP_NAK_RECEIVED,
  DHCP_FAILED,
};

/* Owns one wildcard port-68 endpoint; borrows clock/random READ authority.
 * No function changes interface settings. Addresses are host-order. Deadlines are
 * absolute monotonic nanoseconds; infinite leases have UINT64_MAX deadlines. */
bool dhcp_open(struct dhcp_client *client, handle_t udp, handle_t clock,
    handle_t random, const uint8_t mac[6], uint64_t connection_generation);
bool dhcp_close(struct dhcp_client *client);
void dhcp_discover(struct dhcp_client *client);
/* The held interface must be unassigned. Timeout retains discovery/REQUEST state and backoff
 * for a later call. Only ACKNOWLEDGED fills lease; all other results clear it. */
enum dhcp_result dhcp_acquire(struct dhcp_client *client, uint64_t deadline_ns,
    struct dhcp_lease *lease);
/* Resumes renewal/rebind until deadline, preserving transaction/backoff. A matching NAK or
 * expiry returns control to the configuration owner to clear IPv4 and restart
 * discovery. ACK retains omitted mask/router/DNS and requires the same IP. */
enum dhcp_result dhcp_refresh(struct dhcp_client *client,
    const struct dhcp_lease *current, uint64_t deadline_ns, struct dhcp_lease *updated);

#endif
