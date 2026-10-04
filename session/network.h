#ifndef SESSION_NETWORK_H
#define SESSION_NETWORK_H

#include <abi/net_config.h>
#include <stdbool.h>
#include <stdint.h>
#include "dhcp.h"

#define NETWORK_CONFIG_PATH "app://config/network.lua"

enum network_action { NETWORK_KEEP, NETWORK_REPLACE, NETWORK_CLEAR };
struct network_config {
  enum network_action action;
  bool optional;
  bool dhcp, dns_explicit;
  struct net_selector selector;
  uint32_t address, prefix, gateway;
  uint32_t dns_address;
  char dns_server[sizeof("255.255.255.255")];
};

struct network_runtime {
  struct dhcp_client dhcp;
  struct dhcp_lease lease;
  handle_t authority;
};

/* Reading validates the Lua shape, explicit selector and numeric syntax without
 * side effects.
 * dns_server owns canonical dotted decimal, defaulting to 1.1.1.1.
 * Applying selects DNS in userspace and publishes it through NET_CONFIG;
 * Initial acquisition is bounded; runtime owns the DHCP endpoint until stopped.
 * Keep config and runtime alive through successor handoff and maintenance. */
bool network_config_read(struct network_config *config);
bool network_config_apply(const struct network_config *config, struct network_runtime *runtime);
/* A configuration-owning remote handoff must keep discovery moving while it
 * waits for its first address; other remote sessions use the existing query wait. */
bool network_config_wait_address(const struct network_config *config, struct network_runtime *runtime);
int network_config_maintain(const struct network_config *config, struct network_runtime *runtime);
bool network_config_stop(const struct network_config *config, struct network_runtime *runtime);

#endif
