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
};

/* Reading validates the Lua shape and numeric syntax without side effects.
 * Applying delegates full address/route validation to the kernel. */
bool network_config_read(struct network_config *config);
bool network_config_apply(const struct network_config *config);

#endif
