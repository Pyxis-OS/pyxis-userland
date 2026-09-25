#include "network.h"
#include "../libconfig/config.h"
#include <lauxlib.h>
#include <net_config.h>
#include <startup.h>
#include <stdio.h>
#include <string.h>

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

static int decode_network(lua_State *state)
{
  struct network_config *config = lua_touserdata(state, 2);
  lua_settop(state, 1);
  const char *keys[] = {"net0"};
  config_keys(state, 1, keys, 1);
  config_field(state, 1, "net0");
  if (lua_isnil(state, 2)) {
    return 0;
  }
  if (lua_type(state, 2) == LUA_TBOOLEAN && !lua_toboolean(state, 2)) {
    config->action = NETWORK_CLEAR;
    return 0;
  }
  const char *settings[] = {"optional", "address", "prefix", "gateway"};
  config_keys(state, 2, settings, sizeof(settings) / sizeof(settings[0]));
  config->action = NETWORK_REPLACE;

  config_field(state, 2, "optional");
  if (!lua_isnil(state, 3)) {
    if (lua_type(state, 3) != LUA_TBOOLEAN) {
      return luaL_error(state, "net0.optional must be a boolean");
    }
    config->optional = lua_toboolean(state, 3);
  }
  lua_pop(state, 1);

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

bool network_config_read(struct network_config *config)
{
  *config = (struct network_config){0};
  return config_read(NETWORK_CONFIG_PATH, decode_network, config) != CONFIG_ERROR;
}

bool network_config_apply(const struct network_config *config)
{
  if (config->action == NETWORK_KEEP) {
    return true;
  }
  handle_t authority = startup_resource("net_config");
  if (authority == HANDLE_INVALID) {
    fputs("session: no network configuration authority; keeping current settings\n", stderr);
    return true;
  }
  enum call_status status;
  if (config->action == NETWORK_CLEAR) {
    status = net_config_clear(authority);
  } else {
    struct net_config_reply snapshot;
    status = net_config_query(authority, &snapshot);
    if (status == CALL_OK && !(snapshot.flags & NET_CONFIG_PRESENT)) {
      if (config->optional) {
        return true;
      }
      fputs("session: required net0 is absent\n", stderr);
      return false;
    }
    if (status == CALL_OK && !(snapshot.flags & NET_CONFIG_READY)) {
      fputs("session: net0 transport unavailable; continuing without network setup\n", stderr);
      return true;
    }
    if (status == CALL_OK) {
      status = net_config_replace(authority, config->address, config->prefix, config->gateway);
    }
  }
  if (status == CALL_BAD_REQUEST) {
    fputs("session: invalid net0 address, subnet or gateway\n", stderr);
    return false;
  }
  if (status != CALL_OK) {
    fprintf(stderr, "session: network setup failed (status %u); continuing session\n", status);
    return true;
  }
  if (config->action == NETWORK_REPLACE) {
    uint32_t address = config->address;
    printf("net0: %u.%u.%u.%u/%u%s\n", address >> 24, (address >> 16) & 255,
        (address >> 8) & 255, address & 255, config->prefix,
        config->gateway ? " with default gateway" : "");
  }
  return true;
}
