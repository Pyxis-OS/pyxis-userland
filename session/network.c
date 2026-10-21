#include "../common/bootstrap_environment.h"
#include "../common/system_log.h"
#include "../boot-init/network.h"
#include "network.h"
#include <clock.h>
#include <abi/wait.h>
#include <handle.h>
#include <net_config.h>
#include <startup.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CLIENT_SLICE_NS UINT64_C(20000000)
#define NETWORK_POLL_NS UINT64_C(100000000)
#define INITIAL_ACQUIRE_NS UINT64_C(10000000000)

static bool report(struct interface_runtime *runtime, uint32_t phase)
{
  if (runtime->wifi && !runtime->connection_generation) {
    return true;
  }
  struct net_interface_reply snapshot;
  enum call_status status = net_interface_query(runtime->authority, &snapshot);
  if (status != CALL_OK) {
    return false;
  }
  if (snapshot.connection_generation != runtime->connection_generation) {
    return true;
  }
  if (runtime->reported_phase == phase && runtime->reported_epoch == snapshot.address_epoch) {
    return true;
  }
  status = net_interface_report_dhcp(runtime->authority, runtime->connection_generation,
      snapshot.address_epoch, phase);
  if (status == CALL_OK) {
    runtime->reported_phase = phase;
    runtime->reported_epoch = snapshot.address_epoch;
  }
  return status == CALL_OK || status == CALL_UNAVAILABLE;
}

static bool clear_lease(struct interface_runtime *runtime)
{
  bool retired = false;
  if (runtime->wifi) {
    struct net_interface_reply snapshot;
    if (net_interface_query(runtime->authority, &snapshot) != CALL_OK) {
      return false;
    }
    retired = snapshot.connection_generation != runtime->connection_generation ||
        !runtime->connection_generation;
  }
  if (!retired) {
    enum call_status status = net_interface_clear(runtime->authority, runtime->connection_generation);
    if (status != CALL_OK) {
      struct net_interface_reply snapshot;
      if (!runtime->wifi || status != CALL_UNAVAILABLE ||
          net_interface_query(runtime->authority, &snapshot) != CALL_OK ||
          snapshot.connection_generation == runtime->connection_generation) {
        return false;
      }
    }
  }
  runtime->lease = (struct dhcp_lease){0};
  runtime->configured = false;
  runtime->needs_clear = false;
  runtime->reported_epoch = 0;
  dhcp_discover(&runtime->dhcp);
  return true;
}

static bool apply_lease(const struct network_interface_config *config,
    struct interface_runtime *runtime, const struct dhcp_lease *lease)
{
  uint64_t now;
  if (lease->connection_generation != runtime->connection_generation ||
      clock_now(runtime->dhcp.clock, &now) != CALL_OK || now >= lease->expires_ns) {
    return false;
  }
  enum call_status status = net_interface_replace(runtime->authority, lease->connection_generation,
      lease->address, lease->prefix, lease->gateway, lease->dns);
  if (status != CALL_OK) {
    if (runtime->wifi && status == CALL_ALREADY_EXISTS) {
      system_log("session: %s lease address is already used by another interface, gateway or DNS server (status %u)\n",
          config->name, status);
    } else {
      system_log("session: %s lease rejected (status %u)\n", config->name, status);
    }
    return false;
  }
  runtime->lease = *lease;
  runtime->configured = true;
  runtime->dhcp.refresh_started = false;
  system_log("%s: DHCP %u.%u.%u.%u/%u\n", config->name, lease->address >> 24,
      lease->address >> 16 & 255, lease->address >> 8 & 255, lease->address & 255, lease->prefix);
  return report(runtime, NET_DHCP_BOUND);
}

static bool service_interface(const struct network_interface_config *config,
    struct interface_runtime *runtime, handle_t clock, handle_t random, uint64_t deadline, bool *working)
{
  struct net_interface_reply snapshot;
  enum call_status status = net_interface_query(runtime->authority, &snapshot);
  if (status == CALL_UNAVAILABLE || status == CALL_QUEUE_FULL) {
    return true;
  }
  if (status != CALL_OK || snapshot.interface_id != runtime->id) {
    return false;
  }
  if (snapshot.connection_generation != runtime->connection_generation) {
    if (!runtime->wifi || !dhcp_close(&runtime->dhcp)) {
      return false;
    }
    /* Retirement already cleared the old IPv4 incarnation in the kernel.
     * Close its endpoint and discard its work before adopting another link. */
    runtime->lease = (struct dhcp_lease){0};
    runtime->configured = runtime->owns_dhcp_settings = false;
    runtime->failed = runtime->needs_clear = false;
    runtime->reported_epoch = 0;
    runtime->reported_phase = NET_DHCP_NONE;
    runtime->connection_generation = snapshot.connection_generation;
  }
  if (runtime->failed || runtime->needs_clear) {
    return true;
  }
  bool ready = (snapshot.flags & (NET_INTERFACE_BOUND | NET_INTERFACE_READY)) ==
      (NET_INTERFACE_BOUND | NET_INTERFACE_READY);
  bool usable = ready &&
      (!(snapshot.flags & NET_INTERFACE_CARRIER_KNOWN) || (snapshot.flags & NET_INTERFACE_LINK_UP)) &&
      (!(snapshot.flags & NET_INTERFACE_AUTHENTICATION_VALID) ||
       (snapshot.flags & (NET_INTERFACE_AUTHENTICATED | NET_INTERFACE_OPEN))) &&
      (!runtime->wifi || (snapshot.connection_generation &&
       (snapshot.flags & (NET_INTERFACE_AUTHENTICATED | NET_INTERFACE_OPEN))));
  if (!config->dhcp) {
    if (!runtime->configured && (runtime->wifi ? usable : ready)) {
      status = net_interface_replace(runtime->authority, runtime->connection_generation,
          config->address, config->prefix, config->gateway, 0);
      if (status == CALL_OK) {
        runtime->configured = true;
      } else if (status != CALL_UNAVAILABLE && status != CALL_QUEUE_FULL) {
        fprintf(stderr, "session: %s configuration failed (status %u)\n", config->name, status);
        runtime->failed = true;
      }
    }
    *working |= runtime->configured;
    return true;
  }
  uint64_t now;
  if (clock_now(clock, &now) != CALL_OK) {
    return false;
  }
  if (!usable) {
    return report(runtime, NET_DHCP_WAIT_LINK);
  }
  if (runtime->dhcp.endpoint == HANDLE_INVALID) {
    if (!dhcp_open(&runtime->dhcp, runtime->udp, clock, random, snapshot.mac,
        runtime->connection_generation)) {
      return report(runtime, NET_DHCP_WAIT_LINK);
    }
    if (!clear_lease(runtime)) {
      dhcp_close(&runtime->dhcp);
      return false;
    }
    runtime->owns_dhcp_settings = true;
  }
  *working |= runtime->lease.address != 0;
  if (runtime->lease.address && now < runtime->lease.renewal_ns) {
    return report(runtime, NET_DHCP_BOUND);
  }
  if (now >= deadline) {
    return true;
  }
  uint32_t phase = !runtime->lease.address ? NET_DHCP_SELECTING :
      now < runtime->lease.rebind_ns ? NET_DHCP_RENEWING : NET_DHCP_REBINDING;
  if (!report(runtime, phase) || now > UINT64_MAX - CLIENT_SLICE_NS) {
    return false;
  }
  uint64_t until = deadline - now < CLIENT_SLICE_NS ? deadline : now + CLIENT_SLICE_NS;
  struct dhcp_lease lease;
  enum dhcp_result result = runtime->lease.address ?
      dhcp_refresh(&runtime->dhcp, &runtime->lease, until, &lease) :
      dhcp_acquire(&runtime->dhcp, until, &lease);
  if (result == DHCP_ACKNOWLEDGED) {
    if (apply_lease(config, runtime, &lease)) {
      *working = true;
    } else if (!runtime->lease.address) {
      dhcp_discover(&runtime->dhcp);
    }
  } else if (result == DHCP_NAK_RECEIVED) {
    if (!clear_lease(runtime) || !report(runtime, NET_DHCP_EXPIRED)) {
      return false;
    }
  } else if (result == DHCP_FAILED) {
    if (!clear_lease(runtime) || !dhcp_close(&runtime->dhcp) || !report(runtime, NET_DHCP_FAILED)) {
      return false;
    }
  }
  return true;
}

static bool expire_leases(struct network_runtime *runtime)
{
  uint64_t now;
  if (clock_now(runtime->clock, &now) != CALL_OK) {
    return false;
  }
  for (size_t i = 0; i < runtime->count; ++i) {
    struct interface_runtime *entry = &runtime->interfaces[i];
    if (entry->needs_clear || (entry->lease.address && now >= entry->lease.expires_ns)) {
      if (!clear_lease(entry)) {
        if (!entry->needs_clear) {
          fprintf(stderr, "session: interface %llu clear unavailable; retrying\n", (unsigned long long)entry->id);
        }
        entry->needs_clear = true;
        continue;
      }
      report(entry, entry->failed ? NET_DHCP_FAILED : NET_DHCP_EXPIRED);
    }
  }
  return true;
}

static bool service(const struct network_config *config, struct network_runtime *runtime,
    uint64_t deadline, bool *working)
{
  if (!expire_leases(runtime)) {
    return false;
  }
  bool unbound = false;
  for (size_t i = 0; i < runtime->count; ++i) {
    struct net_interface_reply snapshot;
    if (net_interface_query(runtime->interfaces[i].authority, &snapshot) == CALL_OK) {
      unbound |= !(snapshot.flags & NET_INTERFACE_BOUND);
    }
  }
  if (unbound && !runtime->binding_failed && runtime->policy != HANDLE_INVALID) {
    enum call_status status = boot_network_bind(config, runtime->ids, runtime->registry, runtime->policy);
    if (status != CALL_OK && status != CALL_UNAVAILABLE && status != CALL_QUEUE_FULL && status != CALL_NO_MEMORY) {
      fprintf(stderr, "session: late interface binding failed (status %u)\n", status);
      runtime->binding_failed = true;
    }
  }
  runtime->waiting_link = false;
  *working = false;
  for (size_t i = 0; i < runtime->count; ++i) {
    if (!expire_leases(runtime)) {
      return false;
    }
    if (!service_interface(&config->interfaces[i], &runtime->interfaces[i], runtime->clock,
        runtime->random, deadline, working)) {
      fprintf(stderr, "session: %s network maintainer failed\n", config->interfaces[i].name);
      struct interface_runtime *entry = &runtime->interfaces[i];
      entry->failed = true;
      entry->needs_clear = entry->owns_dhcp_settings;
      if (entry->needs_clear) {
        clear_lease(entry);
      }
      dhcp_close(&entry->dhcp);
      report(entry, NET_DHCP_FAILED);
    }
    runtime->waiting_link |= runtime->interfaces[i].needs_clear ||
        (!runtime->interfaces[i].failed && (config->interfaces[i].dhcp || !runtime->interfaces[i].configured));
  }
  return true;
}

static bool captured_controller(const char *interface, bool required, uint32_t *controller)
{
  char name[32], text[16];
  snprintf(name, sizeof(name), "network.%s", interface);
  struct environment_entry_reply hint;
  enum call_status status = environment_lookup(startup_environment_handle(), name, &hint, text, sizeof(text));
  *controller = 0;
  if (!required && status == CALL_NOT_FOUND) {
    return true;
  }
  if (status == CALL_OK && hint.kind == ENVIRONMENT_CAP) {
    handle_close(hint.handle);
  }
  bool valid = status == CALL_OK && hint.kind == ENVIRONMENT_TEXT && hint.text_length &&
      hint.text_length < sizeof(text) && text[0] != '0';
  for (size_t i = 0; valid && i < hint.text_length; ++i) {
    unsigned digit = (unsigned char)text[i] - '0';
    if (digit > 9 || *controller > (UINT32_MAX - digit) / 10) {
      valid = false;
    } else {
      *controller = *controller * 10 + digit;
    }
  }
  return valid && *controller;
}

/* Boot-init already chose the immutable initial set. Adopt only definitions
 * backed by this owner's private configuration and DHCP grants, never a fresh
 * controller scan or a name observed through registry READ alone. */
static bool adopt_wired(struct network_config *config, handle_t registry)
{
  if (!config->wired_auto) {
    return true;
  }
  for (uint64_t after = 0;;) {
    struct net_interface_reply published;
    if (net_registry_next_interface(registry, after, &published) != CALL_OK) {
      return false;
    }
    if (!published.interface_id) {
      return true;
    }
    after = published.interface_id;
    bool explicit = false;
    for (size_t i = 0; i < config->count; ++i) {
      explicit |= !strcmp(config->interfaces[i].name, published.name);
    }
    if (explicit || published.interface_class != NET_INTERFACE_CLASS_ETHERNET) {
      continue;
    }
    char name[32];
    snprintf(name, sizeof(name), "config.%s", published.name);
    handle_t authority = bootstrap_capability(name);
    snprintf(name, sizeof(name), "dhcp.%s", published.name);
    handle_t udp = bootstrap_capability(name);
    if (authority == HANDLE_INVALID && udp == HANDLE_INVALID) {
      continue;
    }
    uint32_t controller;
    struct net_interface_reply owned;
    if (!captured_controller(published.name, true, &controller) ||
        authority == HANDLE_INVALID || udp == HANDLE_INVALID ||
        net_interface_query(authority, &owned) != CALL_OK ||
        owned.interface_id != published.interface_id || strcmp(owned.name, published.name) ||
        owned.interface_class != NET_INTERFACE_CLASS_ETHERNET ||
        (owned.controller_id && owned.controller_id != controller) ||
        config->count == NETWORK_INTERFACE_LIMIT) {
      fputs("session: invalid declared automatic interface facets\n", stderr);
      return false;
    }
    struct network_interface_config *entries = realloc(config->interfaces,
        (config->count + 1) * sizeof(*entries));
    if (!entries) {
      return false;
    }
    config->interfaces = entries;
    struct network_interface_config *entry = &entries[config->count++];
    *entry = (struct network_interface_config){.interface_class = NET_INTERFACE_CLASS_ETHERNET,
      .optional = true, .dhcp = true, .automatic = true, .resolved_controller = controller,
      .selector = {.kind = NET_SELECT_CONTROLLER, .controller_id = controller}};
    memcpy(entry->name, owned.name, sizeof(entry->name));
  }
}

bool network_config_apply(struct network_config *config, struct network_runtime *runtime)
{
  *runtime = (struct network_runtime){
    .registry = bootstrap_capability("net_registry"), .policy = bootstrap_capability("net_policy"),
    .clock = bootstrap_capability("clock"), .random = bootstrap_capability("random")};
  if (runtime->registry == HANDLE_INVALID || !adopt_wired(config, runtime->registry)) {
    return false;
  }
  runtime->count = config->count;
  runtime->interfaces = calloc(config->count, sizeof(*runtime->interfaces));
  runtime->ids = calloc(config->count + 1, sizeof(*runtime->ids));
  if ((config->count && !runtime->interfaces) || !runtime->ids ||
      runtime->registry == HANDLE_INVALID || runtime->clock == HANDLE_INVALID) {
    return false;
  }
  for (size_t i = 0; i < config->count; ++i) {
    struct interface_runtime *entry = &runtime->interfaces[i];
    struct network_interface_config *definition = &config->interfaces[i];
    uint32_t captured;
    if (!captured_controller(definition->name, definition->automatic, &captured)) {
      fprintf(stderr, "session: invalid initial controller for %s\n", definition->name);
      return false;
    }
    definition->resolved_controller = captured;
    char name[32];
    snprintf(name, sizeof(name), "config.%s", config->interfaces[i].name);
    entry->authority = bootstrap_capability(name);
    snprintf(name, sizeof(name), "dhcp.%s", config->interfaces[i].name);
    entry->udp = config->interfaces[i].dhcp ? bootstrap_capability(name) : HANDLE_INVALID;
    entry->dhcp.endpoint = HANDLE_INVALID;
    struct net_interface_reply snapshot;
    if (entry->authority == HANDLE_INVALID || net_interface_query(entry->authority, &snapshot) != CALL_OK ||
        strcmp(snapshot.name, config->interfaces[i].name) ||
        snapshot.interface_class != config->interfaces[i].interface_class ||
        (captured && snapshot.controller_id && snapshot.controller_id != captured) ||
        (config->interfaces[i].dhcp && entry->udp == HANDLE_INVALID)) {
      fprintf(stderr, "session: missing declared network facets for %s\n", config->interfaces[i].name);
      return false;
    }
    if (snapshot.controller_id) {
      config->interfaces[i].resolved_controller = snapshot.controller_id;
    }
    runtime->ids[i + 1] = entry->id = snapshot.interface_id;
    entry->wifi = snapshot.interface_class == NET_INTERFACE_CLASS_WIFI;
    entry->connection_generation = snapshot.connection_generation;
  }
  uint64_t now;
  if (clock_now(runtime->clock, &now) != CALL_OK || now > UINT64_MAX - INITIAL_ACQUIRE_NS) {
    return false;
  }
  uint64_t deadline = now + INITIAL_ACQUIRE_NS;
  for (;;) {
    bool working;
    if (!service(config, runtime, deadline, &working)) {
      return false;
    }
    bool acquiring = false;
    for (size_t i = 0; i < runtime->count; ++i) {
      const struct interface_runtime *entry = &runtime->interfaces[i];
      if (!entry->failed && !entry->configured) {
        struct net_interface_reply snapshot;
        if (net_interface_query(entry->authority, &snapshot) == CALL_OK) {
          /* An absent optional interface never delays a working/local session.
           * An active client shares this one aggregate foreground budget. */
          acquiring |= entry->dhcp.endpoint != HANDLE_INVALID ||
              (!config->interfaces[i].optional && (snapshot.flags & NET_INTERFACE_BOUND));
        }
      }
    }
    if (working || !acquiring) {
      return true;
    }
    if (clock_now(runtime->clock, &now) != CALL_OK) {
      return false;
    }
    if (now >= deadline) {
      return true;
    }
    uint64_t wake = deadline - now < NETWORK_POLL_NS ? deadline : now + NETWORK_POLL_NS;
    if (clock_sleep_until(runtime->clock, wake) != CALL_OK) {
      return false;
    }
  }
}

bool network_config_wait_address(const struct network_config *config, struct network_runtime *runtime)
{
  /* Remote's separately held read facet chooses the interface; other clients
   * continue independently while this trusted owner waits for that address. */
  handle_t remote = bootstrap_capability("remote_interface");
  for (;;) {
    struct net_interface_reply snapshot;
    if (remote == HANDLE_INVALID || net_interface_query(remote, &snapshot) != CALL_OK) {
      return false;
    }
    if (snapshot.flags & NET_INTERFACE_ADDRESS_VALID) {
      return true;
    }
    bool working;
    if (!service(config, runtime, UINT64_MAX, &working) || clock_sleep_for(runtime->clock, NETWORK_POLL_NS) != CALL_OK) {
      return false;
    }
  }
}

static bool wait_maintenance(struct network_runtime *runtime, struct session_wifi_supplicant *supplicant)
{
  uint64_t now;
  handle_t clock = runtime->clock ? runtime->clock : bootstrap_capability("clock");
  if (clock_now(clock, &now) != CALL_OK) {
    return false;
  }
  uint64_t wake = UINT64_MAX;
  uint64_t poll = UINT64_MAX - now < NETWORK_POLL_NS ? UINT64_MAX : now + NETWORK_POLL_NS;
  for (size_t i = 0; i < runtime->count; ++i) {
    const struct interface_runtime *entry = &runtime->interfaces[i];
    if (entry->wifi && poll < wake) {
      wake = poll;
    }
    if (entry->lease.address && !entry->needs_clear && entry->lease.expires_ns < wake) {
      wake = entry->lease.expires_ns;
    }
    if (entry->needs_clear || (!entry->failed && (!entry->configured ||
        entry->reported_phase == NET_DHCP_WAIT_LINK || entry->dhcp.refresh_started ||
        (entry->lease.address && now >= entry->lease.renewal_ns)))) {
      if (poll < wake) {
        wake = poll;
      }
    } else if (!entry->failed && entry->lease.address && entry->lease.renewal_ns < wake) {
      wake = entry->lease.renewal_ns;
    }
  }
  if (session_wifi_active(supplicant)) {
    if (wake > now && wake - now > WAIT_MAX_WAIT_NS) {
      wake = now + WAIT_MAX_WAIT_NS;
    }
    if (session_wifi_wait(supplicant, wake)) {
      return true;
    }
    /* Optional worker supervision cannot retire unrelated interface leases. */
    if (poll < wake) {
      wake = poll;
    }
  }
  return clock_sleep_until(clock, wake) == CALL_OK;
}

int network_config_maintain(const struct network_config *config, struct network_runtime *runtime,
    struct session_wifi_supplicant *supplicant)
{
  while (runtime->waiting_link || session_wifi_active(supplicant)) {
    bool working;
    if ((runtime->waiting_link && !service(config, runtime, UINT64_MAX, &working)) ||
        !wait_maintenance(runtime, supplicant)) {
      network_config_stop(config, runtime);
      return EXIT_FAILURE;
    }
  }
  return EXIT_SUCCESS;
}

bool network_config_stop(const struct network_config *config, struct network_runtime *runtime)
{
  (void)config;
  bool success = true;
  for (size_t i = 0; runtime->interfaces && i < runtime->count; ++i) {
    struct interface_runtime *entry = &runtime->interfaces[i];
    success &= !entry->owns_dhcp_settings || clear_lease(entry);
    success &= dhcp_close(&entry->dhcp);
  }
  runtime->waiting_link = false;
  free(runtime->interfaces);
  free(runtime->ids);
  runtime->interfaces = NULL;
  runtime->ids = NULL;
  runtime->count = 0;
  return success;
}

bool network_runtime_release(struct network_runtime *runtime)
{
  size_t capacity = runtime->count * 3 + 9;
  handle_t *retained = calloc(capacity, sizeof(*retained));
  if (!retained) {
    return false;
  }
  size_t count = 0;
  retained[count++] = runtime->registry;
  retained[count++] = runtime->policy;
  retained[count++] = runtime->clock;
  retained[count++] = runtime->random;
  retained[count++] = bootstrap_capability("memory");
  retained[count++] = bootstrap_capability("output");
  retained[count++] = bootstrap_stdio(ENVIRONMENT_STDOUT).handle;
  retained[count++] = bootstrap_stdio(ENVIRONMENT_STDERR).handle;
  for (size_t i = 0; i < runtime->count; ++i) {
    retained[count++] = runtime->interfaces[i].authority;
    retained[count++] = runtime->interfaces[i].udp;
    retained[count++] = runtime->interfaces[i].dhcp.endpoint;
  }
  bool success = bootstrap_release_except(retained, count);
  free(retained);
  runtime->background = true;
  return success;
}
