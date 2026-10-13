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

static void read_preferences(lua_State *state, struct network_config *config)
{
  config_field(state, 2, "prefer");
  if (lua_isnil(state, 3)) {
    lua_pop(state, 1);
    return;
  }
  if (config->selector.kind != NET_SELECT_LINKED_CONTROLLER) {
    luaL_error(state, "net0.prefer requires select = 'link'");
  }
  luaL_checktype(state, 3, LUA_TTABLE);
  size_t count = lua_rawlen(state, 3);
  if (count > SIZE_MAX / sizeof(*config->preferred_macs) || count > LUA_MAXINTEGER) {
    luaL_error(state, "net0.prefer is too large");
  }
  size_t entries = 0;
  lua_pushnil(state);
  while (lua_next(state, 3)) {
    if (!lua_isinteger(state, -2) || lua_tointeger(state, -2) < 1 ||
        (lua_Unsigned)lua_tointeger(state, -2) > count) {
      luaL_error(state, "net0.prefer must be a consecutive array of MAC strings");
    }
    ++entries;
    lua_pop(state, 1);
  }
  if (entries != count) {
    luaL_error(state, "net0.prefer must be a consecutive array of MAC strings");
  }
  if (count) {
    config->preferred_macs = malloc(count * sizeof(*config->preferred_macs));
    if (!config->preferred_macs) {
      luaL_error(state, "cannot allocate net0.prefer");
    }
  }
  config->preference_count = count;
  for (size_t i = 0; i < count; ++i) {
    lua_rawgeti(state, 3, (lua_Integer)i + 1);
    read_mac(state, 4, config->preferred_macs[i], "net0.prefer entry");
    lua_pop(state, 1);
  }
  lua_pop(state, 1);
}

static void read_selector(lua_State *state, struct network_config *config)
{
  struct net_selector *selector = &config->selector;
  config_field(state, 2, "driver");
  config_field(state, 2, "mac");
  config_field(state, 2, "select");
  bool has_driver = !lua_isnil(state, 3), has_mac = !lua_isnil(state, 4);
  bool has_select = !lua_isnil(state, 5);
  if ((unsigned)has_driver + (unsigned)has_mac + (unsigned)has_select != 1) {
    luaL_error(state, "net0 requires exactly one of driver, mac or select");
  }
  if (has_driver) {
    if (lua_type(state, 3) != LUA_TSTRING) {
      luaL_error(state, "net0.driver must be the string 'virtio'");
    }
    size_t length;
    const char *driver = lua_tolstring(state, 3, &length);
    if (length != sizeof("virtio") - 1 || memcmp(driver, "virtio", length)) {
      luaL_error(state, "net0.driver must be the string 'virtio'");
    }
    selector->kind = NET_SELECT_VIRTIO;
  } else if (has_mac) {
    read_mac(state, 4, selector->mac, "net0.mac");
    selector->kind = NET_SELECT_MAC;
  } else {
    if (lua_type(state, 5) != LUA_TSTRING) {
      luaL_error(state, "net0.select must be the string 'link'");
    }
    size_t length;
    const char *selection = lua_tolstring(state, 5, &length);
    if (length != sizeof("link") - 1 || memcmp(selection, "link", length)) {
      luaL_error(state, "net0.select must be the string 'link'");
    }
    selector->kind = NET_SELECT_LINKED_CONTROLLER;
  }
  lua_pop(state, 3);
  read_preferences(state, config);
}

/* Leaves the root table alone; DNS is independent of net0 being absent/false. */
static void decode_dns(lua_State *state, struct network_config *config)
{
  config_field(state, 1, "dns");
  if (!lua_isnil(state, 2)) {
    const char *keys[] = {"server"};
    config_keys(state, 2, keys, 1);
    config_field(state, 2, "server");
    if (!lua_isnil(state, 3)) {
      uint32_t address = read_address(state, "dns.server");
      /* Exclude 0/8, multicast and reserved high addresses. Loopback is useful
       * for a local resolver; subnet validity/reachability belongs to routing. */
      if (!(address >> 24) || address >= IPV4_MULTICAST_BASE) {
        luaL_error(state, "dns.server must be a unicast IPv4 address");
      }
      snprintf(config->dns_server, sizeof(config->dns_server), "%u.%u.%u.%u",
          address >> 24, (address >> 16) & 255, (address >> 8) & 255, address & 255);
      config->dns_address = address;
      config->dns_explicit = true;
    }
    lua_pop(state, 1);
  }
  lua_pop(state, 1);
}

static int decode_network(lua_State *state)
{
  struct network_config *config = lua_touserdata(state, 2);
  lua_settop(state, 1);
  const char *keys[] = {"net0", "dns"};
  config_keys(state, 1, keys, sizeof(keys) / sizeof(keys[0]));
  decode_dns(state, config);
  config_field(state, 1, "net0");
  if (lua_isnil(state, 2)) {
    return 0;
  }
  if (lua_type(state, 2) == LUA_TBOOLEAN && !lua_toboolean(state, 2)) {
    config->action = NETWORK_CLEAR;
    return 0;
  }
  const char *settings[] = {"driver", "mac", "select", "prefer", "optional", "dhcp",
      "address", "prefix", "gateway"};
  config_keys(state, 2, settings, sizeof(settings) / sizeof(settings[0]));
  config->action = NETWORK_REPLACE;
  read_selector(state, config);

  config_field(state, 2, "optional");
  if (!lua_isnil(state, 3)) {
    if (lua_type(state, 3) != LUA_TBOOLEAN) {
      return luaL_error(state, "net0.optional must be a boolean");
    }
    config->optional = lua_toboolean(state, 3);
  }
  lua_pop(state, 1);

  config_field(state, 2, "dhcp");
  if (!lua_isnil(state, 3)) {
    if (lua_type(state, 3) != LUA_TBOOLEAN) {
      return luaL_error(state, "net0.dhcp must be a boolean");
    }
    config->dhcp = lua_toboolean(state, 3);
  }
  lua_pop(state, 1);
  if (config->dhcp) {
    const char *static_fields[] = {"address", "prefix", "gateway"};
    for (size_t i = 0; i < sizeof(static_fields) / sizeof(static_fields[0]); ++i) {
      config_field(state, 2, static_fields[i]);
      if (!lua_isnil(state, 3)) {
        return luaL_error(state, "net0.dhcp excludes address, prefix and gateway");
      }
      lua_pop(state, 1);
    }
    return 0;
  }

  config_field(state, 2, "address");
  config->address = read_address(state, "net0.address");
  lua_pop(state, 1);
  config_field(state, 2, "prefix");
  int integer;
  lua_Integer prefix = lua_tointegerx(state, 3, &integer);
  if (lua_type(state, 3) != LUA_TNUMBER || !integer || prefix < 1 || prefix > 32) {
    return luaL_error(state, "net0.prefix must be an integer from 1 to 32");
  }
  config->prefix = (uint32_t)prefix;
  lua_pop(state, 1);
  config_field(state, 2, "gateway");
  if (!lua_isnil(state, 3)) {
    config->gateway = read_address(state, "net0.gateway");
  }
  return 0;
}

static int decode_literal_network(lua_State *state)
{
  struct network_config *config = lua_touserdata(state, 2);
  decode_network(state);
  if (config->action == NETWORK_REPLACE && !config->dhcp &&
      !network_static_valid(config->address, config->prefix, config->gateway)) {
    return luaL_error(state, "net0 address, subnet or gateway violates IPv4 host constraints");
  }
  return 0;
}

bool network_config_read(struct network_config *config)
{
  *config = (struct network_config){.dns_server = "1.1.1.1", .dns_address = UINT32_C(0x01010101)};
  if (config_read(NETWORK_CONFIG_PATH, decode_network, config) == CONFIG_ERROR) {
    network_config_free(config);
    return false;
  }
  return true;
}

void network_config_free(struct network_config *config)
{
  free(config->preferred_macs);
  *config = (struct network_config){0};
}

enum config_result network_config_read_bytes(const char *name, const char *bytes,
    size_t size, struct network_config *config)
{
  *config = (struct network_config){.dns_server = "1.1.1.1", .dns_address = UINT32_C(0x01010101)};
  enum config_result result = config_read_bytes(name, bytes, size, decode_network, config);
  if (result == CONFIG_ERROR) {
    network_config_free(config);
  }
  return result;
}

enum config_result network_config_read_literal_bytes(const char *name, const char *bytes,
    size_t size, struct network_config *config)
{
  *config = (struct network_config){.dns_server = "1.1.1.1", .dns_address = UINT32_C(0x01010101)};
  enum config_result result = config_read_literal_bytes(name, bytes, size, decode_literal_network, config);
  if (result == CONFIG_ERROR) {
    network_config_free(config);
  }
  return result;
}
