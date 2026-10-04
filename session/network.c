#include "network.h"
#include "dhcp.h"
#include "../libconfig/config.h"
#include <clock.h>
#include <lauxlib.h>
#include <net_config.h>
#include <startup.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IPV4_MULTICAST_BASE UINT32_C(0xe0000000)

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

static void read_selector(lua_State *state, struct net_selector *selector)
{
  config_field(state, 2, "driver");
  config_field(state, 2, "mac");
  bool has_driver = !lua_isnil(state, 3), has_mac = !lua_isnil(state, 4);
  if (has_driver == has_mac) {
    luaL_error(state, "net0 requires exactly one of driver or mac");
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
  } else {
    if (lua_type(state, 4) != LUA_TSTRING) {
      luaL_error(state, "net0.mac must be a string of six colon-separated hex pairs");
    }
    size_t length;
    const char *mac = lua_tolstring(state, 4, &length);
    if (length != 17) {
      luaL_error(state, "net0.mac must contain six colon-separated hex pairs");
    }
    uint8_t nonzero = 0;
    for (unsigned i = 0; i < 6; ++i) {
      int high = hex_digit((unsigned char)mac[3 * i]);
      int low = hex_digit((unsigned char)mac[3 * i + 1]);
      if (high < 0 || low < 0 || (i != 5 && mac[3 * i + 2] != ':')) {
        luaL_error(state, "net0.mac must contain six colon-separated hex pairs");
      }
      selector->mac[i] = (uint8_t)((high << 4) | low);
      nonzero |= selector->mac[i];
    }
    if (!nonzero || (selector->mac[0] & 1)) {
      luaL_error(state, "net0.mac must be a nonzero unicast address");
    }
    selector->kind = NET_SELECT_MAC;
  }
  lua_pop(state, 2);
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
  const char *settings[] = {"driver", "mac", "optional", "dhcp", "address", "prefix", "gateway"};
  config_keys(state, 2, settings, sizeof(settings) / sizeof(settings[0]));
  config->action = NETWORK_REPLACE;
  read_selector(state, &config->selector);

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

bool network_config_read(struct network_config *config)
{
  *config = (struct network_config){.dns_server = "1.1.1.1", .dns_address = UINT32_C(0x01010101)};
  return config_read(NETWORK_CONFIG_PATH, decode_network, config) != CONFIG_ERROR;
}

#define INITIAL_ACQUIRE_NS UINT64_C(10000000000)
#define DISCOVERY_WAIT_NS UINT64_C(30000000000)
#define REJECTED_LEASE_WAIT_NS UINT64_C(4000000000)

static uint32_t lease_dns(const struct network_config *config, const struct dhcp_lease *lease)
{
  return !config->dns_explicit && lease->dns ? lease->dns : config->dns_address;
}

static enum call_status apply_lease(const struct network_config *config,
    struct network_runtime *runtime, const struct dhcp_lease *lease)
{
  uint64_t now;
  if (clock_now(runtime->dhcp.clock, &now) != CALL_OK) {
    return CALL_UNAVAILABLE;
  }
  if (now >= lease->expires_ns) {
    return CALL_BAD_REQUEST;
  }
  const struct dhcp_lease *current = &runtime->lease;
  enum call_status status = CALL_OK;
  if (current->address != lease->address || current->prefix != lease->prefix ||
      current->gateway != lease->gateway) {
    status = net_config_replace(runtime->authority, lease->address, lease->prefix,
        lease->gateway, lease_dns(config, lease));
  } else if (lease_dns(config, current) != lease_dns(config, lease)) {
    status = net_config_set_dns(runtime->authority, lease_dns(config, lease));
  }
  if (status == CALL_OK) {
    runtime->lease = *lease;
  }
  return status;
}

static void report_lease(const struct dhcp_lease *lease)
{
  printf("net0: DHCP %u.%u.%u.%u/%u%s\n", lease->address >> 24,
      (lease->address >> 16) & 255, (lease->address >> 8) & 255,
      lease->address & 255, lease->prefix,
      lease->gateway ? " with default gateway" : "");
}

static bool clear_lease(const struct network_config *config, struct network_runtime *runtime)
{
  enum call_status cleared = net_config_clear(runtime->authority);
  if (cleared == CALL_OK) {
    runtime->lease = (struct dhcp_lease){0};
  }
  enum call_status dns = net_config_set_dns(runtime->authority, config->dns_address);
  if (cleared != CALL_OK || dns != CALL_OK) {
    fprintf(stderr, "session: clearing DHCP settings failed (IPv4 %u, DNS %u)\n", cleared, dns);
    return false;
  }
  return true;
}

bool network_config_stop(const struct network_config *config, struct network_runtime *runtime)
{
  if (runtime->dhcp.endpoint == HANDLE_INVALID) {
    return true;
  }
  bool cleared = clear_lease(config, runtime);
  bool closed = dhcp_close(&runtime->dhcp);
  if (!cleared || !closed) {
    fputs("session: DHCP cleanup failed; reboot required\n", stderr);
  }
  return cleared && closed;
}

static bool apply_ipv4(const struct network_config *config, struct network_runtime *runtime,
    bool *published_dns)
{
  handle_t authority = runtime->authority;
  enum call_status status;
  if (config->action == NETWORK_KEEP) {
    return true;
  }
  if (config->action == NETWORK_CLEAR) {
    status = net_config_clear(authority);
  } else {
    struct net_config_reply snapshot;
    status = net_config_bind(authority, &config->selector, &snapshot);
    if (status == CALL_NOT_FOUND) {
      if (config->optional) {
        return true;
      }
      fputs("session: required net0 is absent\n", stderr);
      return false;
    }
    if (status == CALL_BUSY) {
      fputs("session: net0 selector is ambiguous or a different controller is already bound\n", stderr);
      return false;
    }
    if (status == CALL_UNAVAILABLE) {
      fputs("session: net0 discovery or identity unavailable; continuing without network setup\n", stderr);
      return true;
    }
    if (status == CALL_OK && !(snapshot.flags & NET_CONFIG_READY)) {
      fputs("session: net0 transport unavailable; continuing without network setup\n", stderr);
      return true;
    }
    if (status == CALL_OK) {
      if (config->dhcp) {
        if (!dhcp_open(&runtime->dhcp, startup_resource("udp"), startup_resource("clock"),
            startup_resource("random"), snapshot.mac)) {
          fputs("session: cannot reserve DHCP port 68; keeping current settings\n", stderr);
          return false;
        }
        /* Reserve the wildcard endpoint before changing shared settings. */
        status = net_config_clear(authority);
        if (status != CALL_OK) {
          fprintf(stderr, "session: preparing DHCP failed (status %u)\n", status);
          return false;
        }
        uint64_t now;
        if (clock_now(runtime->dhcp.clock, &now) != CALL_OK ||
            now > UINT64_MAX - INITIAL_ACQUIRE_NS) {
          network_config_stop(config, runtime);
          return true;
        }
        struct dhcp_lease lease;
        enum dhcp_result result = dhcp_acquire(&runtime->dhcp, now + INITIAL_ACQUIRE_NS, &lease);
        if (result == DHCP_ACKNOWLEDGED) {
          status = apply_lease(config, runtime, &lease);
          if (status == CALL_OK) {
            *published_dns = true;
            report_lease(&lease);
            return true;
          }
          if (status != CALL_BAD_REQUEST) {
            network_config_stop(config, runtime);
            return true;
          }
          fputs("session: DHCP lease rejected; continuing offline\n", stderr);
          dhcp_discover(&runtime->dhcp);
        } else {
          fputs("session: DHCP acquisition incomplete; continuing offline\n", stderr);
          if (result == DHCP_FAILED) {
            network_config_stop(config, runtime);
          } else if (result == DHCP_NAK_RECEIVED) {
            dhcp_discover(&runtime->dhcp);
          }
        }
        return true;
      }
      status = net_config_replace(authority, config->address, config->prefix,
          config->gateway, config->dns_address);
    }
  }
  if (status == CALL_BAD_REQUEST) {
    fputs("session: invalid net0 selector, address, subnet or gateway\n", stderr);
    return false;
  }
  if (status != CALL_OK) {
    fprintf(stderr, "session: network setup failed (status %u); continuing session\n", status);
    return true;
  }
  *published_dns = config->action == NETWORK_REPLACE;
  if (config->action == NETWORK_REPLACE) {
    uint32_t address = config->address;
    printf("net0: %u.%u.%u.%u/%u%s\n", address >> 24, (address >> 16) & 255,
        (address >> 8) & 255, address & 255, config->prefix,
        config->gateway ? " with default gateway" : "");
  }
  return true;
}

bool network_config_apply(const struct network_config *config, struct network_runtime *runtime)
{
  *runtime = (struct network_runtime){.authority = startup_resource("net_config")};
  if (runtime->authority == HANDLE_INVALID) {
    fputs("session: no network configuration authority; keeping current settings\n", stderr);
    return true;
  }
  bool published_dns = false;
  if (!apply_ipv4(config, runtime, &published_dns)) {
    return false;
  }
  if (!published_dns) {
    enum call_status status = net_config_set_dns(runtime->authority, config->dns_address);
    if (status != CALL_OK) {
      fprintf(stderr, "session: selecting DNS failed (status %u); keeping current settings\n", status);
      return status != CALL_BAD_REQUEST;
    }
  }
  return true;
}

static bool continue_discovery(const struct network_config *config,
    struct network_runtime *runtime)
{
  uint64_t now;
  if (clock_now(runtime->dhcp.clock, &now) != CALL_OK ||
      now > UINT64_MAX - DISCOVERY_WAIT_NS) {
    return false;
  }
  struct dhcp_lease lease;
  enum dhcp_result result = dhcp_acquire(&runtime->dhcp, now + DISCOVERY_WAIT_NS, &lease);
  if (result == DHCP_FAILED) {
    return false;
  }
  if (result == DHCP_TIMEOUT) {
    return true;
  }
  if (result == DHCP_NAK_RECEIVED) {
    if (!clear_lease(config, runtime)) {
      return false;
    }
    dhcp_discover(&runtime->dhcp);
    return true;
  }
  enum call_status status = apply_lease(config, runtime, &lease);
  if (status == CALL_OK) {
    report_lease(&lease);
    return true;
  }
  if (status != CALL_BAD_REQUEST) {
    return false;
  }
  fputs("session: DHCP lease rejected; continuing discovery\n", stderr);
  dhcp_discover(&runtime->dhcp);
  return clock_sleep_for(runtime->dhcp.clock, REJECTED_LEASE_WAIT_NS) == CALL_OK;
}

bool network_config_wait_address(const struct network_config *config,
    struct network_runtime *runtime)
{
  while (runtime->dhcp.endpoint != HANDLE_INVALID && !runtime->lease.address) {
    if (!continue_discovery(config, runtime)) {
      network_config_stop(config, runtime);
      return false;
    }
  }
  if (config->dhcp && !runtime->lease.address) {
    fputs("session: DHCP stopped before remote network assignment\n", stderr);
    return false;
  }
  return true;
}

int network_config_maintain(const struct network_config *config, struct network_runtime *runtime)
{
  if (runtime->dhcp.endpoint == HANDLE_INVALID) {
    return EXIT_SUCCESS;
  }
  for (;;) {
    uint64_t now;
    if (clock_now(runtime->dhcp.clock, &now) != CALL_OK) {
      break;
    }
    if (runtime->lease.address && now >= runtime->lease.expires_ns) {
      if (!clear_lease(config, runtime)) {
        break;
      }
      dhcp_discover(&runtime->dhcp);
      fputs("net0: DHCP lease expired; rediscovering\n", stderr);
    }
    if (runtime->lease.address && now < runtime->lease.renewal_ns) {
      if (clock_sleep_until(runtime->dhcp.clock, runtime->lease.renewal_ns) != CALL_OK) {
        break;
      }
      continue;
    }
    if (!runtime->lease.address) {
      if (!continue_discovery(config, runtime)) {
        break;
      }
      continue;
    }
    struct dhcp_lease lease;
    enum dhcp_result result = dhcp_refresh(&runtime->dhcp, &runtime->lease, &lease);
    if (result == DHCP_FAILED) {
      break;
    }
    if (result == DHCP_TIMEOUT) {
      continue;
    }
    if (result == DHCP_NAK_RECEIVED) {
      if (!clear_lease(config, runtime)) {
        break;
      }
      dhcp_discover(&runtime->dhcp);
      fputs("net0: DHCP lease invalidated; rediscovering\n", stderr);
      continue;
    }
    enum call_status status = apply_lease(config, runtime, &lease);
    if (status == CALL_OK) {
      continue;
    }
    if (status != CALL_BAD_REQUEST) {
      break;
    }
    fputs("session: DHCP lease rejected; retaining current settings\n", stderr);
    if (clock_now(runtime->dhcp.clock, &now) != CALL_OK ||
        now > UINT64_MAX - REJECTED_LEASE_WAIT_NS) {
      break;
    }
    uint64_t retry_at = now + REJECTED_LEASE_WAIT_NS;
    if (retry_at > runtime->lease.expires_ns) {
      retry_at = runtime->lease.expires_ns;
    }
    if (clock_sleep_until(runtime->dhcp.clock, retry_at) != CALL_OK) {
      break;
    }
  }
  network_config_stop(config, runtime);
  fputs("session: DHCP maintenance failed; stopping network setup\n", stderr);
  return EXIT_FAILURE;
}
