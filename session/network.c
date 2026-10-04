#include "network.h"
#include "dhcp.h"
#include "../libconfig/config.h"
#include <clock.h>
#include <handle.h>
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
  config->preferred_macs = NULL;
  config->preference_count = 0;
}

#define LINK_POLL_NS UINT64_C(100000000)
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
    fprintf(stderr, "session: clearing network settings failed (IPv4 %u, DNS %u)\n", cleared, dns);
    return false;
  }
  return true;
}

bool network_config_stop(const struct network_config *config, struct network_runtime *runtime)
{
  runtime->waiting_link = false;
  bool cleared = !runtime->owns_dhcp_settings || clear_lease(config, runtime);
  if (cleared) {
    runtime->owns_dhcp_settings = false;
  }
  bool closed = dhcp_close(&runtime->dhcp);
  if (runtime->background && runtime->udp != HANDLE_INVALID) {
    closed &= handle_close(runtime->udp) == 0;
    runtime->udp = HANDLE_INVALID;
  }
  if (!cleared || !closed) {
    fputs("session: network cleanup failed; reboot required\n", stderr);
  }
  return cleared && closed;
}

static size_t preference_rank(const struct network_config *config,
    const struct net_controller_reply *controller)
{
  for (size_t i = 0; i < config->preference_count; ++i) {
    if (!memcmp(config->preferred_macs[i], controller->mac, sizeof(controller->mac))) {
      return i;
    }
  }
  return config->preference_count;
}

static unsigned driver_rank(uint32_t driver)
{
  return driver == NET_DRIVER_RTL8111 ? 0 : driver == NET_DRIVER_VIRTIO ? 1 : 2;
}

static bool prefer_controller(const struct network_config *config,
    const struct net_controller_reply *candidate, const struct net_controller_reply *current)
{
  if (!current->controller_id) {
    return true;
  }
  size_t candidate_rank = preference_rank(config, candidate);
  size_t current_rank = preference_rank(config, current);
  if (candidate_rank != current_rank) {
    return candidate_rank < current_rank;
  }
  unsigned candidate_driver = driver_rank(candidate->driver);
  unsigned current_driver = driver_rank(current->driver);
  if (candidate_driver != current_driver) {
    return candidate_driver < current_driver;
  }
  return candidate->controller_id < current->controller_id;
}

enum link_selection { LINK_WAIT, LINK_BOUND, LINK_FAILED };

bool network_config_binding_pending(handle_t authority, bool *pending)
{
  *pending = false;
  uint32_t after_id = 0;
  for (;;) {
    struct net_controller_reply controller;
    enum call_status status = net_config_next_controller(authority, after_id, &controller);
    if (status == CALL_UNAVAILABLE || status == CALL_QUEUE_FULL) {
      *pending = true;
      return true;
    }
    if (status != CALL_OK) {
      fprintf(stderr, "session: cannot inspect bound network controller (status %u)\n", status);
      return false;
    }
    if (controller.flags & NET_CONTROLLER_BOUND) {
      /* Stable configuration may still be sampled after activation. Permanent
       * transport failure clears preparation while retaining the binding. */
      *pending = (controller.flags & NET_CONTROLLER_PREPARED) != 0;
      return true;
    }
    if (!controller.controller_id) {
      if (!(controller.flags & NET_CONTROLLER_INVENTORY_COMPLETE)) {
        *pending = true;
        return true;
      }
      fputs("session: bound network controller missing from inventory\n", stderr);
      return false;
    }
    if (controller.controller_id <= after_id) {
      fputs("session: invalid network controller inventory order\n", stderr);
      return false;
    }
    after_id = controller.controller_id;
  }
}

static enum link_selection bound_readiness(struct network_runtime *runtime,
    const struct net_config_reply *snapshot)
{
  if (snapshot->flags & NET_CONFIG_READY) {
    return LINK_BOUND;
  }
  bool pending;
  if (!network_config_binding_pending(runtime->authority, &pending)) {
    return LINK_FAILED;
  }
  return pending ? LINK_WAIT : LINK_BOUND;
}

static enum link_selection select_link(const struct network_config *config,
    struct network_runtime *runtime, struct net_config_reply *snapshot)
{
  enum call_status status = net_config_query(runtime->authority, snapshot);
  if (status == CALL_UNAVAILABLE || status == CALL_QUEUE_FULL) {
    return LINK_WAIT;
  }
  if (status != CALL_OK) {
    fprintf(stderr, "session: cannot query net0 binding (status %u)\n", status);
    return LINK_FAILED;
  }
  if (snapshot->flags & NET_CONFIG_BOUND) {
    return bound_readiness(runtime, snapshot);
  }
  if (config->selector.kind != NET_SELECT_LINKED_CONTROLLER) {
    fputs("session: selected net0 binding unavailable\n", stderr);
    return LINK_FAILED;
  }
  struct net_controller_reply selected = {0};
  uint32_t after_id = 0;
  bool complete = true;
  for (;;) {
    struct net_controller_reply controller;
    status = net_config_next_controller(runtime->authority, after_id, &controller);
    if (status == CALL_UNAVAILABLE || status == CALL_QUEUE_FULL) {
      return LINK_WAIT;
    }
    if (status != CALL_OK) {
      fprintf(stderr, "session: cannot enumerate network controllers (status %u)\n", status);
      return LINK_FAILED;
    }
    complete &= (controller.flags & NET_CONTROLLER_INVENTORY_COMPLETE) != 0;
    if (!controller.controller_id) {
      break;
    }
    if (controller.controller_id <= after_id) {
      fputs("session: invalid network controller inventory order\n", stderr);
      return LINK_FAILED;
    }
    after_id = controller.controller_id;
    uint32_t usable = NET_CONTROLLER_PREPARED | NET_CONTROLLER_CARRIER_KNOWN |
        NET_CONTROLLER_LINK_UP;
    if ((controller.flags & usable) == usable &&
        prefer_controller(config, &controller, &selected)) {
      selected = controller;
    }
  }
  if (!complete || !selected.controller_id) {
    return LINK_WAIT;
  }
  struct net_selector selector = {
    .kind = NET_SELECT_LINKED_CONTROLLER, .controller_id = selected.controller_id,
  };
  status = net_config_bind(runtime->authority, &selector, snapshot);
  if (status == CALL_OK) {
    return bound_readiness(runtime, snapshot);
  }
  if (status == CALL_UNAVAILABLE || status == CALL_BUSY || status == CALL_NOT_FOUND) {
    /* A lost carrier does not consume selection. A committed binding, including
     * failed activation, is authoritative and must never trigger fallback. */
    status = net_config_query(runtime->authority, snapshot);
    if (status == CALL_OK) {
      return snapshot->flags & NET_CONFIG_BOUND ? bound_readiness(runtime, snapshot) : LINK_WAIT;
    }
  }
  if (status == CALL_UNAVAILABLE || status == CALL_QUEUE_FULL) {
    return LINK_WAIT;
  }
  fprintf(stderr, "session: binding linked net0 failed (status %u)\n", status);
  return LINK_FAILED;
}

static void report_static(const struct network_config *config)
{
  uint32_t address = config->address;
  printf("net0: %u.%u.%u.%u/%u%s\n", address >> 24, (address >> 16) & 255,
      (address >> 8) & 255, address & 255, config->prefix,
      config->gateway ? " with default gateway" : "");
}

static bool apply_binding(const struct network_config *config, struct network_runtime *runtime,
    const struct net_config_reply *snapshot, uint64_t deadline, bool *published_dns)
{
  runtime->waiting_link = false;
  if (!(snapshot->flags & NET_CONFIG_READY)) {
    fputs("session: net0 transport unavailable; continuing without network setup\n", stderr);
    return true;
  }
  if (!config->dhcp) {
    enum call_status status = net_config_replace(runtime->authority, config->address,
        config->prefix, config->gateway, config->dns_address);
    if (status == CALL_BAD_REQUEST) {
      fputs("session: invalid net0 address, subnet or gateway\n", stderr);
      return false;
    }
    if (status != CALL_OK) {
      fprintf(stderr, "session: network setup failed (status %u); continuing session\n", status);
      return true;
    }
    *published_dns = true;
    report_static(config);
    return true;
  }
  if (!dhcp_open(&runtime->dhcp, runtime->udp, runtime->dhcp.clock,
      runtime->dhcp.random, snapshot->mac)) {
    fputs("session: cannot reserve DHCP port 68; keeping current settings\n", stderr);
    return false;
  }
  if (runtime->background) {
    int closed = handle_close(runtime->udp);
    runtime->udp = HANDLE_INVALID;
    if (closed != 0) {
      fputs("session: cannot release DHCP endpoint creation authority\n", stderr);
      return false;
    }
  }
  /* Reserve the wildcard endpoint before changing shared settings. */
  enum call_status status = net_config_clear(runtime->authority);
  if (status != CALL_OK) {
    fprintf(stderr, "session: preparing DHCP failed (status %u)\n", status);
    return false;
  }
  runtime->owns_dhcp_settings = true;
  if (!deadline) {
    uint64_t now;
    if (clock_now(runtime->dhcp.clock, &now) != CALL_OK ||
        now > UINT64_MAX - INITIAL_ACQUIRE_NS) {
      network_config_stop(config, runtime);
      return true;
    }
    deadline = now + INITIAL_ACQUIRE_NS;
  }
  struct dhcp_lease lease;
  enum dhcp_result result = dhcp_acquire(&runtime->dhcp, deadline, &lease);
  if (result == DHCP_ACKNOWLEDGED) {
    status = apply_lease(config, runtime, &lease);
    if (status == CALL_OK) {
      *published_dns = true;
      report_lease(&lease);
      return true;
    }
    if (status != CALL_BAD_REQUEST) {
      network_config_stop(config, runtime);
      return !runtime->background;
    }
    fputs("session: DHCP lease rejected; continuing offline\n", stderr);
    dhcp_discover(&runtime->dhcp);
  } else {
    fputs("session: DHCP acquisition incomplete; continuing offline\n", stderr);
    if (result == DHCP_FAILED) {
      network_config_stop(config, runtime);
      return !runtime->background;
    } else if (result == DHCP_NAK_RECEIVED) {
      dhcp_discover(&runtime->dhcp);
    }
  }
  return true;
}

static bool wait_link(const struct network_config *config, struct network_runtime *runtime,
    uint64_t deadline, bool *published_dns)
{
  for (;;) {
    struct net_config_reply snapshot;
    enum link_selection result = select_link(config, runtime, &snapshot);
    if (result == LINK_FAILED) {
      runtime->waiting_link = false;
      return false;
    }
    if (result == LINK_BOUND) {
      return apply_binding(config, runtime, &snapshot, deadline, published_dns);
    }
    uint64_t now;
    if (clock_now(runtime->dhcp.clock, &now) != CALL_OK) {
      return false;
    }
    if (now >= deadline) {
      return true;
    }
    uint64_t wake = deadline - now < LINK_POLL_NS ? deadline : now + LINK_POLL_NS;
    if (clock_sleep_until(runtime->dhcp.clock, wake) != CALL_OK) {
      return false;
    }
  }
}

static bool apply_ipv4(const struct network_config *config, struct network_runtime *runtime,
    uint64_t deadline, bool *published_dns)
{
  if (config->action == NETWORK_KEEP) {
    return true;
  }
  if (config->action == NETWORK_CLEAR) {
    enum call_status status = net_config_clear(runtime->authority);
    if (status != CALL_OK) {
      fprintf(stderr, "session: network clear failed (status %u); continuing session\n", status);
    }
    return status != CALL_BAD_REQUEST;
  }
  if (config->selector.kind == NET_SELECT_LINKED_CONTROLLER) {
    runtime->waiting_link = true;
    bool applied = wait_link(config, runtime, deadline, published_dns);
    if (applied && runtime->waiting_link) {
      fputs("session: net0 setup pending; continuing offline while waiting\n", stderr);
    }
    return applied;
  }
  struct net_config_reply snapshot;
  enum call_status status = net_config_bind(runtime->authority, &config->selector, &snapshot);
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
  if (status == CALL_BAD_REQUEST) {
    fputs("session: invalid net0 selector\n", stderr);
    return false;
  }
  if (status != CALL_OK) {
    fprintf(stderr, "session: network setup failed (status %u); continuing session\n", status);
    return true;
  }
  enum link_selection readiness = bound_readiness(runtime, &snapshot);
  if (readiness == LINK_FAILED) {
    return false;
  }
  if (readiness == LINK_WAIT) {
    uint64_t now;
    if (clock_now(runtime->dhcp.clock, &now) != CALL_OK ||
        now > UINT64_MAX - INITIAL_ACQUIRE_NS) {
      fputs("session: network setup clock unavailable; keeping current settings\n", stderr);
      return true;
    }
    runtime->waiting_link = true;
    bool applied = wait_link(config, runtime, now + INITIAL_ACQUIRE_NS, published_dns);
    if (applied && runtime->waiting_link) {
      fputs("session: net0 setup pending; continuing offline while waiting\n", stderr);
    }
    return applied;
  }
  return apply_binding(config, runtime, &snapshot, deadline, published_dns);
}

bool network_config_apply(const struct network_config *config, struct network_runtime *runtime)
{
  *runtime = (struct network_runtime){
    .authority = startup_resource("net_config"),
    .udp = config->dhcp ? startup_resource("udp") : HANDLE_INVALID,
    .dhcp = {.clock = startup_resource("clock"),
        .random = config->dhcp ? startup_resource("random") : HANDLE_INVALID},
  };
  if (runtime->authority == HANDLE_INVALID) {
    fputs("session: no network configuration authority; keeping current settings\n", stderr);
    return true;
  }
  uint64_t deadline = 0;
  if (config->action == NETWORK_REPLACE &&
      config->selector.kind == NET_SELECT_LINKED_CONTROLLER) {
    uint64_t now;
    if (clock_now(runtime->dhcp.clock, &now) != CALL_OK ||
        now > UINT64_MAX - INITIAL_ACQUIRE_NS) {
      fputs("session: network setup clock unavailable; keeping current settings\n", stderr);
      return true;
    }
    deadline = now + INITIAL_ACQUIRE_NS;
  }
  bool published_dns = false;
  if (!apply_ipv4(config, runtime, deadline, &published_dns)) {
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

static bool continue_selection(const struct network_config *config,
    struct network_runtime *runtime)
{
  uint64_t now;
  if (clock_now(runtime->dhcp.clock, &now) != CALL_OK ||
      now > UINT64_MAX - DISCOVERY_WAIT_NS) {
    return false;
  }
  bool published_dns = false;
  return wait_link(config, runtime, now + DISCOVERY_WAIT_NS, &published_dns);
}

bool network_config_wait_address(const struct network_config *config,
    struct network_runtime *runtime)
{
  while (runtime->waiting_link) {
    if (!continue_selection(config, runtime)) {
      network_config_stop(config, runtime);
      return false;
    }
  }
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
  while (runtime->waiting_link) {
    if (!continue_selection(config, runtime)) {
      network_config_stop(config, runtime);
      fputs("session: network selection failed; stopping setup\n", stderr);
      return EXIT_FAILURE;
    }
  }
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
