#ifndef BOOT_INIT_NETWORK_H
#define BOOT_INIT_NETWORK_H

#include "../libconfig/network.h"
#include <abi/handle.h>

struct boot_network {
  struct network_config config;
  handle_t policy;
  uint64_t *ids;
  handle_t *facets, *dhcp;
  char (*facet_names)[32], (*dhcp_names)[32];
  handle_t echo, udp, tcp;
  handle_t remote_interface, remote_tcp, remote_udp;
};

/* Trusted boot setup fixes identities and the immutable stock packet scopes.
 * Policy remains private to boot-init and the explicitly configured owner. */
bool boot_network_prepare(struct boot_network *network, handle_t registry, handle_t policy);
uint64_t boot_network_id(const struct boot_network *network, const char *name);
/* Resolve selectors for the entire plan before committing a binding batch.
 * Missing optional/controllers without usable carrier remain defined/unbound. */
enum call_status boot_network_bind(const struct network_config *config, const uint64_t *ids,
    handle_t registry, handle_t policy);

#endif
