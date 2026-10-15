#include "network.h"
#include "handoff.h"
#include <handle.h>
#include <environment.h>
#include <lauxlib.h>
#include <startup.h>
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

static bool string_is(lua_State *state, int index, const char *expected)
{
  if (lua_type(state, index) != LUA_TSTRING) {
    return false;
  }
  size_t length;
  const char *text = lua_tolstring(state, index, &length);
  return length == strlen(expected) && !memcmp(text, expected, length);
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
    if (!string_is(state, 4, "virtio")) {
      luaL_error(state, "interface.driver must be 'virtio'");
    }
    config->selector.kind = NET_SELECT_VIRTIO;
  } else if (mac) {
    read_mac(state, 5, config->selector.mac, "interface.mac");
    config->selector.kind = NET_SELECT_MAC;
  } else {
    if (!string_is(state, 6, "link")) {
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
    bool automatic = config->wired_auto && !strncmp(name, "eth", 3) && name[3] &&
        !(name[3] == '0' && name[4]);
    unsigned number = 0;
    for (size_t i = 3; automatic && name[i]; ++i) {
      unsigned digit = (unsigned char)name[i] - '0';
      automatic = digit <= 9 && number <= (NETWORK_INTERFACE_LIMIT - 1 - digit) / 10;
      if (automatic) {
        number = number * 10 + digit;
      }
    }
    if (!found && !automatic) {
      luaL_error(state, "%s names an undefined interface", field);
    }
  }
  lua_pop(state, 1);
}

static int decode_network(lua_State *state)
{
  struct network_config *config = lua_touserdata(state, 2);
  lua_settop(state, 1);
  const char *keys[] = {"interfaces", "wired", "routing", "dns", "management"};
  config_keys(state, 1, keys, 5);
  config_field(state, 1, "wired");
  if (!lua_isnil(state, 2)) {
    const char *fields[] = {"auto", "exclude"};
    config_keys(state, 2, fields, 2);
    config->wired_auto = read_boolean(state, 2, "auto");
    config_field(state, 2, "exclude");
    if (!lua_isnil(state, 3)) {
      size_t count = array_count(state, 3, "wired.exclude");
      if (count > SIZE_MAX / sizeof(*config->excluded_macs)) {
        return luaL_error(state, "wired.exclude is too large");
      }
      config->excluded_macs = calloc(count, sizeof(*config->excluded_macs));
      if (count && !config->excluded_macs) {
        return luaL_error(state, "cannot allocate wired.exclude");
      }
      config->exclusion_count = count;
      for (size_t i = 0; i < count; ++i) {
        lua_rawgeti(state, 3, (lua_Integer)i + 1);
        read_mac(state, 4, config->excluded_macs[i], "wired.exclude entry");
        lua_pop(state, 1);
        for (size_t j = 0; j < i; ++j) {
          if (!memcmp(config->excluded_macs[i], config->excluded_macs[j], 6)) {
            return luaL_error(state, "duplicate wired.exclude entry");
          }
        }
      }
    }
  }
  lua_settop(state, 1);
  config_field(state, 1, "interfaces");
  size_t count = array_count(state, 2, "interfaces");
  if (count > NETWORK_INTERFACE_LIMIT) {
    return luaL_error(state, "interfaces exceeds the 254-entry plan limit");
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
    const char *prefix;
    if (string_is(state, 4, "ethernet")) {
      entry->interface_class = NET_INTERFACE_CLASS_ETHERNET;
      prefix = "eth";
    } else if (string_is(state, 4, "wifi")) {
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
      if (lua_type(state, 5) != LUA_TNUMBER || !integer || prefix < 1 || prefix > 32) {
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
    const char *fields[] = {"auto", "default"};
    config_keys(state, 2, fields, 2);
    config->routing_auto = read_boolean(state, 2, "auto");
    read_policy_name(state, 2, "default", config, config->default_interface);
    if (config->routing_auto && config->default_interface[0]) {
      return luaL_error(state, "routing.auto excludes routing.default");
    }
  }
  lua_pop(state, 1);
  config_field(state, 1, "dns");
  if (!lua_isnil(state, 2)) {
    const char *fields[] = {"interface", "server"};
    config_keys(state, 2, fields, 2);
    read_policy_name(state, 2, "interface", config, config->dns_interface);
    config_field(state, 2, "server");
    if (!lua_isnil(state, 3)) {
      config->dns_address = read_address(state, "dns.server");
      if (!(config->dns_address >> 24) || config->dns_address >= IPV4_MULTICAST_BASE) {
        return luaL_error(state, "dns.server must be a unicast IPv4 address");
      }
      config->dns_explicit = true;
    }
    lua_pop(state, 1);
    if (!config->dns_interface[0] && !config->dns_explicit) {
      return luaL_error(state, "dns requires a numeric server or an interface");
    }
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
  handle_t source = HANDLE_INVALID;
  enum call_status source_status = environment_capability(startup_environment_handle(), "network_config", &source);
  if (source_status != CALL_OK && source_status != CALL_NOT_FOUND) {
    fprintf(stderr, "config: selected network FILE unavailable (status %u)\n", source_status);
    return false;
  }
  if (source != HANDLE_INVALID) {
    *config = (struct network_config){0};
    struct config_handoff_bytes snapshot = {0};
    enum call_status status;
    bool read = config_handoff_read(source, &snapshot, &status);
    bool closed = handle_close(source) == 0;
    if (!read || !closed) {
      fprintf(stderr, "config: selected network FILE unavailable (status %u, closed %u)\n",
          status, (unsigned)closed);
      config_handoff_bytes_free(&snapshot);
      return false;
    }
    enum config_result result = network_config_read_literal_bytes("selected network FILE",
        snapshot.bytes, snapshot.size, config);
    config_handoff_bytes_free(&snapshot);
    return result == CONFIG_OK;
  }
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
  free(config->excluded_macs);
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
