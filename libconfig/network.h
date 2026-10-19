#ifndef LIBCONFIG_NETWORK_H
#define LIBCONFIG_NETWORK_H

#include "config.h"
#include <abi/net_config.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NETWORK_CONFIG_PATH "boot://config/network.lua"

struct network_interface_config {
  char name[16];
  uint32_t interface_class;
  bool optional, dhcp;
  struct net_selector selector;
  uint8_t (*preferred_macs)[6];
  size_t preference_count;
  uint32_t address, prefix, gateway;
};

struct network_config {
  struct network_interface_config *interfaces;
  size_t count;
  char default_interface[16], dns_interface[16];
  char remote_interface[16], log_interface[16], debug_interface[16];
  bool dns_explicit;
  uint32_t dns_address;
  char dns_server[sizeof("255.255.255.255")];
};

/* Full ordered profile validation has no network effects. Output owns all arrays.
 * Missing files produce an empty plan; legacy net0 keys fail explicitly. */
bool network_config_read(struct network_config *config);
enum config_result network_config_read_bytes(const char *name, const char *bytes,
    size_t size, struct network_config *config);
enum config_result network_config_read_literal_bytes(const char *name, const char *bytes,
    size_t size, struct network_config *config);
void network_config_free(struct network_config *config);
bool network_static_valid(uint32_t address, uint32_t prefix, uint32_t gateway);

#endif
