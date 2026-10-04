#ifndef SESSION_NETWORK_H
#define SESSION_NETWORK_H

#include <abi/net_config.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "dhcp.h"

#define NETWORK_CONFIG_PATH "app://config/network.lua"

enum network_action { NETWORK_KEEP, NETWORK_REPLACE, NETWORK_CLEAR };
struct network_config {
  enum network_action action;
  bool optional;
  bool dhcp, dns_explicit;
  struct net_selector selector;
  uint8_t (*preferred_macs)[6];
  size_t preference_count;
  uint32_t address, prefix, gateway;
  uint32_t dns_address;
  char dns_server[sizeof("255.255.255.255")];
};

struct network_runtime {
  struct dhcp_client dhcp;
  struct dhcp_lease lease;
  handle_t authority;
  handle_t udp;
  bool waiting_link, background;
  /* Ownership starts after endpoint reservation and successful initial clear. */
  bool owns_dhcp_settings;
};

/* Reading validates the Lua shape, selector and numeric syntax without side
 * effects. Free the owned MAC preference array after handoff/maintenance.
 * Link selection considers only prepared controllers with reported carrier,
 * reuses an existing binding, and keeps waiting when none is eligible. optional
 * controls absent explicit selectors only.
 * dns_server owns canonical dotted decimal, defaulting to 1.1.1.1.
 * Applying selects DNS in userspace and publishes it through NET_CONFIG;
 * Link selection and initial acquisition share one bounded foreground deadline.
 * Runtime owns pending selection and the DHCP endpoint until stopped; the
 * trusted setup process retains UDP creation authority only until opening it.
 * Keep config and runtime alive through successor handoff and maintenance. */
bool network_config_read(struct network_config *config);
void network_config_free(struct network_config *config);
bool network_config_apply(const struct network_config *config, struct network_runtime *runtime);
/* A configuration-owning remote handoff must keep discovery moving while it
 * waits for its first address; other remote sessions use the existing query wait. */
bool network_config_wait_address(const struct network_config *config, struct network_runtime *runtime);
int network_config_maintain(const struct network_config *config, struct network_runtime *runtime);
bool network_config_stop(const struct network_config *config, struct network_runtime *runtime);

#endif
