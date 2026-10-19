#include "../common/bootstrap_environment.h"
#include "../boot-init/network.h"
#include "network.h"
#include <clock.h>
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
  struct net_interface_reply snapshot;
  enum call_status status = net_interface_query(runtime->authority, &snapshot);
  if (status != CALL_OK) {
    return false;
  }
  if (runtime->reported_phase == phase && runtime->reported_epoch == snapshot.address_epoch) {
    return true;
  }
  status = net_interface_report_dhcp(runtime->authority, snapshot.address_epoch, phase);
  if (status == CALL_OK) {
    runtime->reported_phase = phase;
    runtime->reported_epoch = snapshot.address_epoch;
  }
  return status == CALL_OK || status == CALL_UNAVAILABLE;
}

static bool clear_lease(struct interface_runtime *runtime)
{
  if (net_interface_clear(runtime->authority) != CALL_OK) {
    return false;
  }
  runtime->lease = (struct dhcp_lease){0};
  runtime->configured = false;
  runtime->reported_epoch = 0;
  dhcp_discover(&runtime->dhcp);
  return true;
}

static bool apply_lease(const struct network_interface_config *config,
    struct interface_runtime *runtime, const struct dhcp_lease *lease)
{
  uint64_t now;
  if (clock_now(runtime->dhcp.clock, &now) != CALL_OK || now >= lease->expires_ns) {
    return false;
  }
  enum call_status status = net_interface_replace(runtime->authority, lease->address,
      lease->prefix, lease->gateway, lease->dns);
  if (status != CALL_OK) {
    fprintf(stderr, "session: %s lease rejected (status %u)\n", config->name, status);
    return false;
  }
  runtime->lease = *lease;
  runtime->configured = true;
  runtime->dhcp.refresh_started = false;
  printf("%s: DHCP %u.%u.%u.%u/%u\n", config->name, lease->address >> 24,
      lease->address >> 16 & 255, lease->address >> 8 & 255, lease->address & 255, lease->prefix);
  return report(runtime, NET_DHCP_BOUND);
}

static bool service_interface(const struct network_interface_config *config,
    struct interface_runtime *runtime, handle_t clock, handle_t random, bool *working)
{
  if (runtime->failed) {
    return true;
  }
  struct net_interface_reply snapshot;
  enum call_status status = net_interface_query(runtime->authority, &snapshot);
  if (status == CALL_UNAVAILABLE || status == CALL_QUEUE_FULL) {
    return true;
  }
  if (status != CALL_OK || snapshot.interface_id != runtime->id) {
    return false;
  }
  bool ready = (snapshot.flags & (NET_INTERFACE_BOUND | NET_INTERFACE_READY)) ==
      (NET_INTERFACE_BOUND | NET_INTERFACE_READY);
  bool usable = ready && (snapshot.flags & NET_INTERFACE_LINK_UP) &&
      (!(snapshot.flags & NET_INTERFACE_AUTHENTICATION_VALID) ||
       (snapshot.flags & NET_INTERFACE_AUTHENTICATED));
  if (!config->dhcp) {
    if (!runtime->configured && ready) {
      status = net_interface_replace(runtime->authority, config->address, config->prefix,
          config->gateway, 0);
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
    if (!dhcp_open(&runtime->dhcp, runtime->udp, clock, random, snapshot.mac)) {
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
  uint32_t phase = !runtime->lease.address ? NET_DHCP_SELECTING :
      now < runtime->lease.rebind_ns ? NET_DHCP_RENEWING : NET_DHCP_REBINDING;
  if (!report(runtime, phase) || now > UINT64_MAX - CLIENT_SLICE_NS) {
    return false;
  }
  struct dhcp_lease lease;
  enum dhcp_result result = runtime->lease.address ?
      dhcp_refresh(&runtime->dhcp, &runtime->lease, now + CLIENT_SLICE_NS, &lease) :
      dhcp_acquire(&runtime->dhcp, now + CLIENT_SLICE_NS, &lease);
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
    if (entry->lease.address && now >= entry->lease.expires_ns) {
      if (!clear_lease(entry)) {
        return false;
      }
      report(entry, NET_DHCP_EXPIRED);
    }
  }
  return true;
}

static bool service(const struct network_config *config, struct network_runtime *runtime,
    bool *working)
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
        runtime->random, working)) {
      fprintf(stderr, "session: %s network maintainer failed\n", config->interfaces[i].name);
      struct interface_runtime *entry = &runtime->interfaces[i];
      if (entry->owns_dhcp_settings && !clear_lease(entry)) {
        return false;
      }
      dhcp_close(&entry->dhcp);
      report(entry, NET_DHCP_FAILED);
      entry->failed = true;
    }
    runtime->waiting_link |= !runtime->interfaces[i].failed &&
        (config->interfaces[i].dhcp || !runtime->interfaces[i].configured);
  }
  return true;
}

bool network_config_apply(const struct network_config *config, struct network_runtime *runtime)
{
  *runtime = (struct network_runtime){.count = config->count,
    .registry = bootstrap_capability("net_registry"), .policy = bootstrap_capability("net_policy"),
    .clock = bootstrap_capability("clock"), .random = bootstrap_capability("random")};
  runtime->interfaces = calloc(config->count, sizeof(*runtime->interfaces));
  runtime->ids = calloc(config->count + 1, sizeof(*runtime->ids));
  if ((config->count && !runtime->interfaces) || !runtime->ids ||
      runtime->registry == HANDLE_INVALID || runtime->clock == HANDLE_INVALID) {
    return false;
  }
  for (size_t i = 0; i < config->count; ++i) {
    struct interface_runtime *entry = &runtime->interfaces[i];
    char name[32];
    snprintf(name, sizeof(name), "config.%s", config->interfaces[i].name);
    entry->authority = bootstrap_capability(name);
    snprintf(name, sizeof(name), "dhcp.%s", config->interfaces[i].name);
    entry->udp = config->interfaces[i].dhcp ? bootstrap_capability(name) : HANDLE_INVALID;
    entry->dhcp.endpoint = HANDLE_INVALID;
    struct net_interface_reply snapshot;
    if (entry->authority == HANDLE_INVALID || net_interface_query(entry->authority, &snapshot) != CALL_OK ||
        strcmp(snapshot.name, config->interfaces[i].name) ||
        (config->interfaces[i].dhcp && entry->udp == HANDLE_INVALID)) {
      fprintf(stderr, "session: missing declared network facets for %s\n", config->interfaces[i].name);
      return false;
    }
    runtime->ids[i + 1] = entry->id = snapshot.interface_id;
  }
  uint64_t now;
  if (clock_now(runtime->clock, &now) != CALL_OK || now > UINT64_MAX - INITIAL_ACQUIRE_NS) {
    return false;
  }
  uint64_t deadline = now + INITIAL_ACQUIRE_NS;
  for (;;) {
    bool working;
    if (!service(config, runtime, &working)) {
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
    if (!service(config, runtime, &working) || clock_sleep_for(runtime->clock, NETWORK_POLL_NS) != CALL_OK) {
      return false;
    }
  }
}

int network_config_maintain(const struct network_config *config, struct network_runtime *runtime)
{
  for (;;) {
    bool working;
    if (!service(config, runtime, &working) || clock_sleep_for(runtime->clock, NETWORK_POLL_NS) != CALL_OK) {
      network_config_stop(config, runtime);
      return EXIT_FAILURE;
    }
  }
}

bool network_config_stop(const struct network_config *config, struct network_runtime *runtime)
{
  (void)config;
  bool success = true;
  for (size_t i = 0; i < runtime->count; ++i) {
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
