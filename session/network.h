#ifndef SESSION_NETWORK_H
#define SESSION_NETWORK_H

#include <abi/net_config.h>
#include <stdbool.h>
#include <stdint.h>

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

/* Reading validates the Lua shape, explicit selector and numeric syntax without
 * side effects.
 * dns_server owns canonical dotted decimal, defaulting to 1.1.1.1.
 * Applying selects DNS in userspace and publishes it through NET_CONFIG;
 * DHCP acquisition is bounded and does not maintain the lease afterward. */
bool network_config_read(struct network_config *config);
bool network_config_apply(const struct network_config *config);

#endif
