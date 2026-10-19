#include "network.h"
#include <lauxlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IPV4_MULTICAST_BASE UINT32_C(0xe0000000)

static bool ipv4_unicast(uint32_t address)
{
  unsigned first = address >> 24;
  return first && first != 127 && address < IPV4_MULTICAST_BASE;
}

static bool subnet_host(uint32_t address, uint32_t mask, uint32_t prefix)
{
  uint32_t host = address & ~mask;
  return prefix >= 31 || (host && host != ~mask);
}

bool network_static_valid(uint32_t address, uint32_t prefix, uint32_t gateway)
{
  if (prefix < 1 || prefix > 32 || !ipv4_unicast(address)) {
    return false;
  }
  uint32_t mask = UINT32_MAX << (32 - prefix);
  return subnet_host(address, mask, prefix) &&
      (!gateway || (ipv4_unicast(gateway) && gateway != address &&
       (gateway & mask) == (address & mask) && subnet_host(gateway, mask, prefix)));
}

static uint32_t read_address(lua_State *state, const char *field)
{
  if (lua_type(state, -1) != LUA_TSTRING) {
    luaL_error(state, "%s must be a numeric IPv4 string", field);
  }
  size_t length;
  const char *text = lua_tolstring(state, -1, &length);
  if (strlen(text) != length) {
    luaL_error(state, "%s contains a NUL byte", field);
  }
  uint32_t address = 0;
  for (unsigned i = 0; i < 4; ++i) {
    unsigned value = 0, digits = 0;
    while (*text >= '0' && *text <= '9') {
      if (++digits > 3) {
        luaL_error(state, "invalid IPv4 %s", field);
      }
      value = value * 10 + (unsigned)(*text++ - '0');
    }
    if (!digits || value > 255 || (i != 3 && *text++ != '.')) {
      luaL_error(state, "invalid IPv4 %s", field);
    }
    address = (address << 8) | value;
  }
  if (*text) {
    luaL_error(state, "invalid IPv4 %s", field);
  }
  return address;
}

static int hex_digit(unsigned char value)
{
  if (value >= '0' && value <= '9') {
    return value - '0';
  }
  if (value >= 'a' && value <= 'f') {
    return value - 'a' + 10;
  }
  if (value >= 'A' && value <= 'F') {
    return value - 'A' + 10;
  }
  return -1;
}

static void read_mac(lua_State *state, int index, uint8_t mac[6], const char *field)
{
  if (lua_type(state, index) != LUA_TSTRING) {
    luaL_error(state, "%s must be a string of six colon-separated hex pairs", field);
  }
  size_t length;
  const char *text = lua_tolstring(state, index, &length);
  if (length != 17) {
    luaL_error(state, "%s must contain six colon-separated hex pairs", field);
  }
  uint8_t nonzero = 0;
  for (unsigned i = 0; i < 6; ++i) {
    int high = hex_digit((unsigned char)text[3 * i]);
    int low = hex_digit((unsigned char)text[3 * i + 1]);
    if (high < 0 || low < 0 || (i != 5 && text[3 * i + 2] != ':')) {
      luaL_error(state, "%s must contain six colon-separated hex pairs", field);
    }
    mac[i] = (uint8_t)((high << 4) | low);
    nonzero |= mac[i];
  }
  if (!nonzero || (mac[0] & 1)) {
    luaL_error(state, "%s must be a nonzero unicast address", field);
  }
}

static void read_name(lua_State *state, int index, char name[16], const char *field)
{
  size_t size = 0;
  const char *text = lua_type(state, index) == LUA_TSTRING ? lua_tolstring(state, index, &size) : NULL;
  if (!text || !size || size >= 16 || strlen(text) != size) {
    luaL_error(state, "%s must be an interface name", field);
  }
  memcpy(name, text, size + 1);
}

static bool read_boolean(lua_State *state, int table, const char *name)
{
  config_field(state, table, name);
  if (!lua_isnil(state, -1) && lua_type(state, -1) != LUA_TBOOLEAN) {
    luaL_error(state, "%s must be a boolean", name);
  }
  bool value = lua_toboolean(state, -1);
  lua_pop(state, 1);
  return value;
}

static size_t array_count(lua_State *state, int table, const char *field)
{
  luaL_checktype(state, table, LUA_TTABLE);
  size_t count = lua_rawlen(state, table), entries = 0;
  lua_pushnil(state);
  while (lua_next(state, table)) {
    if (!lua_isinteger(state, -2) || lua_tointeger(state, -2) < 1 ||
        (lua_Unsigned)lua_tointeger(state, -2) > count) {
      luaL_error(state, "%s must be a consecutive array", field);
    }
    ++entries;
    lua_pop(state, 1);
  }
  if (entries != count) {
    luaL_error(state, "%s must be a consecutive array", field);
  }
  return count;
}

static void read_selector(lua_State *state, struct network_interface_config *config)
{
  config_field(state, 3, "driver");
  config_field(state, 3, "mac");
  config_field(state, 3, "select");
  bool driver = !lua_isnil(state, 4), mac = !lua_isnil(state, 5), select = !lua_isnil(state, 6);
  if ((unsigned)driver + (unsigned)mac + (unsigned)select != 1) {
    luaL_error(state, "interface requires exactly one of driver, mac or select");
  }
  if (driver) {
    if (lua_type(state, 4) != LUA_TSTRING || strcmp(lua_tostring(state, 4), "virtio")) {
      luaL_error(state, "interface.driver must be 'virtio'");
    }
    config->selector.kind = NET_SELECT_VIRTIO;
  } else if (mac) {
    read_mac(state, 5, config->selector.mac, "interface.mac");
    config->selector.kind = NET_SELECT_MAC;
  } else {
    if (lua_type(state, 6) != LUA_TSTRING || strcmp(lua_tostring(state, 6), "link")) {
      luaL_error(state, "interface.select must be 'link'");
    }
    config->selector.kind = NET_SELECT_LINKED_CONTROLLER;
  }
  lua_pop(state, 3);
  config_field(state, 3, "prefer");
  if (!lua_isnil(state, 4)) {
    if (config->selector.kind != NET_SELECT_LINKED_CONTROLLER) {
      luaL_error(state, "interface.prefer requires select = 'link'");
    }
    size_t count = array_count(state, 4, "interface.prefer");
    if (count > SIZE_MAX / sizeof(*config->preferred_macs)) {
      luaL_error(state, "interface.prefer is too large");
    }
    config->preferred_macs = calloc(count, sizeof(*config->preferred_macs));
    if (count && !config->preferred_macs) {
      luaL_error(state, "cannot allocate interface.prefer");
    }
    config->preference_count = count;
    for (size_t i = 0; i < count; ++i) {
      lua_rawgeti(state, 4, (lua_Integer)i + 1);
      read_mac(state, 5, config->preferred_macs[i], "interface.prefer entry");
      lua_pop(state, 1);
    }
  }
  lua_pop(state, 1);
}

static void read_policy_name(lua_State *state, int table, const char *field,
    const struct network_config *config, char name[16])
{
  config_field(state, table, field);
  if (!lua_isnil(state, -1)) {
    read_name(state, -1, name, field);
    bool found = !strcmp(name, "lo");
    for (size_t i = 0; i < config->count; ++i) {
      found |= !strcmp(config->interfaces[i].name, name);
    }
    if (!found) {
      luaL_error(state, "%s names an undefined interface", field);
    }
  }
  lua_pop(state, 1);
}

static int decode_network(lua_State *state)
{
  struct network_config *config = lua_touserdata(state, 2);
  lua_settop(state, 1);
  const char *keys[] = {"interfaces", "routing", "dns", "management"};
  config_keys(state, 1, keys, 4);
  config_field(state, 1, "interfaces");
  size_t count = array_count(state, 2, "interfaces");
  if (count > SIZE_MAX / sizeof(*config->interfaces)) {
    return luaL_error(state, "interfaces is too large");
  }
  config->interfaces = calloc(count, sizeof(*config->interfaces));
  if (count && !config->interfaces) {
    return luaL_error(state, "cannot allocate interfaces");
  }
  config->count = count;
  for (size_t i = 0; i < count; ++i) {
    struct network_interface_config *entry = &config->interfaces[i];
    lua_rawgeti(state, 2, (lua_Integer)i + 1);
    const char *fields[] = {"name", "class", "driver", "mac", "select", "prefer", "optional",
        "dhcp", "address", "prefix", "gateway"};
    config_keys(state, 3, fields, sizeof(fields) / sizeof(*fields));
    config_field(state, 3, "name");
    read_name(state, 4, entry->name, "interface.name");
    lua_pop(state, 1);
    config_field(state, 3, "class");
    const char *class = lua_type(state, 4) == LUA_TSTRING ? lua_tostring(state, 4) : "";
    const char *prefix;
    if (!strcmp(class, "ethernet")) {
      entry->interface_class = NET_INTERFACE_CLASS_ETHERNET;
      prefix = "eth";
    } else if (!strcmp(class, "wifi")) {
      entry->interface_class = NET_INTERFACE_CLASS_WIFI;
      prefix = "wlan";
    } else {
      return luaL_error(state, "interface.class must be 'ethernet' or 'wifi'");
    }
    size_t length = strlen(prefix);
    const char *number = entry->name + length;
    if (strncmp(entry->name, prefix, length) || !*number ||
        (number[0] == '0' && number[1])) {
      return luaL_error(state, "interface.name must use its canonical class prefix and number");
    }
    for (; *number; ++number) {
      if (*number < '0' || *number > '9') {
        return luaL_error(state, "interface.name must end in decimal digits");
      }
    }
    for (size_t j = 0; j < i; ++j) {
      if (!strcmp(entry->name, config->interfaces[j].name)) {
        return luaL_error(state, "duplicate interface.name");
      }
    }
    lua_pop(state, 1);
    read_selector(state, entry);
    entry->optional = read_boolean(state, 3, "optional");
    entry->dhcp = read_boolean(state, 3, "dhcp");
    config_field(state, 3, "address");
    config_field(state, 3, "prefix");
    config_field(state, 3, "gateway");
    if (entry->dhcp) {
      if (!lua_isnil(state, 4) || !lua_isnil(state, 5) || !lua_isnil(state, 6)) {
        return luaL_error(state, "interface.dhcp excludes address, prefix and gateway");
      }
    } else {
      lua_pushvalue(state, 4);
      entry->address = read_address(state, "interface.address");
      lua_pop(state, 1);
      int integer;
      lua_Integer prefix = lua_tointegerx(state, 5, &integer);
      if (!integer || prefix < 1 || prefix > 32) {
        return luaL_error(state, "interface.prefix must be an integer from 1 to 32");
      }
      entry->prefix = (uint32_t)prefix;
      if (!lua_isnil(state, 6)) {
        entry->gateway = read_address(state, "interface.gateway");
      }
      if (!network_static_valid(entry->address, entry->prefix, entry->gateway)) {
        return luaL_error(state, "interface address, subnet or gateway violates IPv4 host constraints");
      }
      for (size_t j = 0; j < i; ++j) {
        if (!config->interfaces[j].dhcp && config->interfaces[j].address == entry->address) {
          return luaL_error(state, "duplicate local IPv4 address");
        }
      }
    }
    lua_settop(state, 2);
  }
  lua_pop(state, 1);
  config_field(state, 1, "routing");
  if (!lua_isnil(state, 2)) {
    const char *fields[] = {"default"};
    config_keys(state, 2, fields, 1);
    read_policy_name(state, 2, "default", config, config->default_interface);
  }
  lua_pop(state, 1);
  config_field(state, 1, "dns");
  if (!lua_isnil(state, 2)) {
    const char *fields[] = {"interface", "server"};
    config_keys(state, 2, fields, 2);
    read_policy_name(state, 2, "interface", config, config->dns_interface);
    if (!config->dns_interface[0]) {
      return luaL_error(state, "dns requires an interface");
    }
    config_field(state, 2, "server");
    if (!lua_isnil(state, 3)) {
      config->dns_address = read_address(state, "dns.server");
      if (!(config->dns_address >> 24) || config->dns_address >= IPV4_MULTICAST_BASE) {
        return luaL_error(state, "dns.server must be a unicast IPv4 address");
      }
      config->dns_explicit = true;
      snprintf(config->dns_server, sizeof(config->dns_server), "%u.%u.%u.%u",
          config->dns_address >> 24, config->dns_address >> 16 & 255,
          config->dns_address >> 8 & 255, config->dns_address & 255);
    }
    lua_pop(state, 1);
  }
  lua_pop(state, 1);
  config_field(state, 1, "management");
  if (!lua_isnil(state, 2)) {
    const char *fields[] = {"remote", "log", "debug"};
    config_keys(state, 2, fields, 3);
    read_policy_name(state, 2, "remote", config, config->remote_interface);
    read_policy_name(state, 2, "log", config, config->log_interface);
    read_policy_name(state, 2, "debug", config, config->debug_interface);
  }
  return 0;
}

bool network_config_read(struct network_config *config)
{
  *config = (struct network_config){0};
  if (config_read(NETWORK_CONFIG_PATH, decode_network, config) == CONFIG_ERROR) {
    network_config_free(config);
    return false;
  }
  return true;
}

void network_config_free(struct network_config *config)
{
  for (size_t i = 0; i < config->count; ++i) {
    free(config->interfaces[i].preferred_macs);
  }
  free(config->interfaces);
  *config = (struct network_config){0};
}

enum config_result network_config_read_bytes(const char *name, const char *bytes,
    size_t size, struct network_config *config)
{
  *config = (struct network_config){0};
  enum config_result result = config_read_bytes(name, bytes, size, decode_network, config);
  if (result == CONFIG_ERROR) {
    network_config_free(config);
  }
  return result;
}

enum config_result network_config_read_literal_bytes(const char *name, const char *bytes,
    size_t size, struct network_config *config)
{
  *config = (struct network_config){0};
  enum config_result result = config_read_literal_bytes(name, bytes, size, decode_network, config);
  if (result == CONFIG_ERROR) {
    network_config_free(config);
  }
  return result;
}
