#ifndef SESSION_NETWORK_H
#define SESSION_NETWORK_H

#include <stdbool.h>
#include <stdint.h>

#define NETWORK_CONFIG_PATH "app://config/network.lua"

enum network_action { NETWORK_KEEP, NETWORK_REPLACE, NETWORK_CLEAR };
struct network_config {
  enum network_action action;
  bool optional;
  uint32_t address, prefix, gateway;
  char dns_server[sizeof("255.255.255.255")];
};

/* Reading validates the Lua shape and numeric syntax without side effects.
 * dns_server owns canonical dotted decimal, defaulting to 1.1.1.1.
 * Applying delegates full address/route validation to the kernel; DNS is only
 * exported to userspace and has no network-configuration side effects. */
bool network_config_read(struct network_config *config);
bool network_config_apply(const struct network_config *config);

#endif
