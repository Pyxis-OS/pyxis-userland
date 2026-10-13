#ifndef LIBCONFIG_NETWORK_H
#define LIBCONFIG_NETWORK_H

#include "config.h"
#include <abi/net_config.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NETWORK_CONFIG_PATH "boot://config/network.lua"

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

/* Static shape, selector and numeric validity only; no network effects.
 * Fresh output owns its preference array. Failure frees it and clears output.
 * Missing trusted file keeps the current net0 and default DNS, as at startup.
 * Literal inputs use the same typed decoder without executing Lua and add the
 * shared address/subnet/gateway check. Trusted startup retains its existing
 * admission timing: NET_CONFIG validates those constraints when applied. */
bool network_config_read(struct network_config *config);
enum config_result network_config_read_bytes(const char *name, const char *bytes,
    size_t size, struct network_config *config);
enum config_result network_config_read_literal_bytes(const char *name, const char *bytes,
    size_t size, struct network_config *config);
void network_config_free(struct network_config *config);
/* Host-order IPv4 rules match NET_CONFIG replacement, including /31 and /32. */
bool network_static_valid(uint32_t address, uint32_t prefix, uint32_t gateway);

#endif
