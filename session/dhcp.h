#ifndef SESSION_DHCP_H
#define SESSION_DHCP_H

#include <abi/handle.h>
#include <stdbool.h>
#include <stdint.h>

struct dhcp_lease {
  uint32_t address, prefix, gateway, dns, server;
  uint32_t lease_seconds, t1_seconds, t2_seconds;
  uint64_t acquired_ns;
};

/* Borrowed UDP broadcast, clock READ and random READ authority. net0 must be
 * bound and unassigned. Owns and closes a port-68 wildcard endpoint; applies
 * no settings. Addresses are host-order, acquired_ns is the original REQUEST
 * send time. Failure clears lease. Acquisition is bounded to about ten seconds;
 * renewal and expiry belong to the later lease lifecycle task. */
bool dhcp_acquire(handle_t udp, handle_t clock, handle_t random,
    const uint8_t mac[6], struct dhcp_lease *lease);

#endif
