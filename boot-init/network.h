#ifndef BOOT_INIT_NETWORK_H
#define BOOT_INIT_NETWORK_H

#include "../libconfig/handoff.h"
#include "../libconfig/network.h"
#include <abi/handle.h>

struct boot_network {
  struct network_config config;
  handle_t policy;
  uint64_t *ids;
  handle_t *facets, *dhcp;
  char (*facet_names)[32], (*dhcp_names)[32];
  char (*controller_names)[32], (*controller_values)[16];
  handle_t echo, udp, tcp;
  handle_t remote_interface, remote_tcp, remote_udp;
};

/* Reads a profile and resolves its wired controllers, with no effect on the
 * network: BYTES is the pool's network.lua, borrowed, or NULL for
 * boot://config/network.lua. False reports why on stderr and leaves CONFIG empty. */
bool boot_network_read(struct network_config *config, handle_t registry,
    const struct config_handoff_bytes *bytes);
/* Trusted boot setup fixes identities and the immutable stock packet scopes.
 * Policy remains private to boot-init and the explicitly configured owner.
 * Takes CONFIG, leaving it empty; an empty profile publishes loopback only.
 * Interfaces defined before a failure stay defined. */
bool boot_network_prepare(struct boot_network *network, handle_t registry, handle_t policy,
    struct network_config *config);
/* Closes NETWORK's handles and frees it after success or failure; keeps policy. */
void boot_network_release(struct boot_network *network);
uint64_t boot_network_id(const struct boot_network *network, const char *name);
/* Resolve selectors for the entire plan before committing a binding batch.
 * Missing optional/controllers without usable carrier remain defined/unbound. */
enum call_status boot_network_bind(const struct network_config *config, const uint64_t *ids,
    handle_t registry, handle_t policy);

#endif
