#include "network.h"
#include "../common/bootstrap_environment.h"
#include <clock.h>
#include <net_config.h>
#include <handle.h>
#include <abi/echo.h>
#include <abi/tcp.h>
#include <abi/udp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INVENTORY_WAIT_NS UINT64_C(10000000000)
#define INVENTORY_POLL_NS UINT64_C(100000000)

static size_t rank(const struct network_interface_config *config,
    const struct net_controller_reply *controller)
{
  for (size_t i = 0; i < config->preference_count; ++i) {
    if (!memcmp(config->preferred_macs[i], controller->mac, sizeof(controller->mac))) {
      return i;
    }
  }
  return config->preference_count;
}

static bool controller_matches(const struct network_interface_config *config,
    const struct net_controller_reply *controller)
{
  if (controller->interface_class != config->interface_class ||
      (config->resolved_controller && config->resolved_controller != controller->controller_id)) {
    return false;
  }
  switch (config->selector.kind) {
    case NET_SELECT_MAC:
      return !memcmp(config->selector.mac, controller->mac, sizeof(controller->mac));
    case NET_SELECT_VIRTIO:
      return controller->driver == NET_DRIVER_VIRTIO;
    case NET_SELECT_CONTROLLER:
      return config->selector.controller_id == controller->controller_id &&
          (controller->flags & NET_CONTROLLER_PREPARED);
    default:
      return (controller->flags & (NET_CONTROLLER_PREPARED | NET_CONTROLLER_CARRIER_KNOWN | NET_CONTROLLER_LINK_UP)) ==
          (NET_CONTROLLER_PREPARED | NET_CONTROLLER_CARRIER_KNOWN | NET_CONTROLLER_LINK_UP);
  }
}

static bool excluded(const struct network_config *config, const struct net_controller_reply *controller)
{
  if (controller->interface_class != NET_INTERFACE_CLASS_ETHERNET) {
    return false;
  }
  for (size_t i = 0; i < config->exclusion_count; ++i) {
    if (!memcmp(config->excluded_macs[i], controller->mac, sizeof(controller->mac))) {
      return true;
    }
  }
  return false;
}

static bool name_used(const struct network_config *config, const char *name)
{
  for (size_t i = 0; i < config->count; ++i) {
    if (!strcmp(config->interfaces[i].name, name)) {
      return true;
    }
  }
  return false;
}

static bool append_automatic(struct network_config *config, const char *name, uint32_t controller)
{
  if (config->count == NETWORK_INTERFACE_LIMIT) {
    fputs("network: resolved plan exceeds 254 non-loopback interfaces\n", stderr);
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
    .resolved_controller = controller, .optional = true, .dhcp = true, .automatic = true,
    .selector = {.kind = NET_SELECT_CONTROLLER, .controller_id = controller}};
  memcpy(entry->name, name, strlen(name) + 1);
  return true;
}

/* Discovery, exclusions, explicit reservations and generated names are resolved
 * before DEFINE/BIND or packet scope publication. Carrier does not gate autos. */
static bool expand_wired(struct network_config *config, handle_t registry)
{
  if (!config->wired_auto && !config->exclusion_count) {
    return true;
  }
  handle_t clock = bootstrap_capability("clock");
  uint64_t now;
  if (clock == HANDLE_INVALID || clock_now(clock, &now) != CALL_OK || now > UINT64_MAX - INVENTORY_WAIT_NS) {
    return false;
  }
  uint64_t deadline = now + INVENTORY_WAIT_NS;
  struct net_controller_reply *controllers = NULL;
  size_t count = 0;
  for (;;) {
    count = 0;
    uint32_t after = 0;
    bool complete = true;
    for (;;) {
      struct net_controller_reply controller;
      if (net_registry_next_controller(registry, after, &controller) != CALL_OK) {
        free(controllers);
        return false;
      }
      complete &= (controller.flags & NET_CONTROLLER_INVENTORY_COMPLETE) != 0;
      if (!controller.controller_id) {
        break;
      }
      after = controller.controller_id;
      if (count == SIZE_MAX / sizeof(*controllers)) {
        free(controllers);
        return false;
      }
      struct net_controller_reply *entries = realloc(controllers, (count + 1) * sizeof(*entries));
      if (!entries) {
        free(controllers);
        return false;
      }
      controllers = entries;
      controllers[count++] = controller;
    }
    if (complete) {
      break;
    }
    if (clock_now(clock, &now) != CALL_OK || now >= deadline ||
        clock_sleep_until(clock, deadline - now < INVENTORY_POLL_NS ? deadline : now + INVENTORY_POLL_NS) != CALL_OK) {
      fputs("network: boot controller discovery did not complete\n", stderr);
      free(controllers);
      return false;
    }
  }
  size_t explicit_count = config->count;
  for (size_t i = 0; i < explicit_count; ++i) {
    struct network_interface_config *entry = &config->interfaces[i];
    struct net_controller_reply *selected = NULL;
    size_t matching = 0;
    bool linked = entry->selector.kind == NET_SELECT_LINKED_CONTROLLER;
    bool permitted_prepared = false;
    bool reserve_link = config->wired_auto &&
        entry->interface_class == NET_INTERFACE_CLASS_ETHERNET && linked;
    for (size_t j = 0; j < count; ++j) {
      if (linked) {
        if (excluded(config, &controllers[j])) {
          continue;
        }
        permitted_prepared |= controllers[j].interface_class == entry->interface_class &&
            (controllers[j].flags & NET_CONTROLLER_PREPARED);
      }
      /* Automatic members must not take a pending explicit role's controller.
       * Its actual linked binding still requires fresh known/up carrier. */
      bool matches = reserve_link ?
          controllers[j].interface_class == entry->interface_class &&
              (controllers[j].flags & NET_CONTROLLER_PREPARED) :
          controller_matches(entry, &controllers[j]);
      if (matches) {
        ++matching;
        if (!selected || rank(entry, &controllers[j]) < rank(entry, selected)) {
          selected = &controllers[j];
        }
      }
    }
    if ((matching > 1 && !linked) ||
        (!selected && !entry->optional && (!linked || !permitted_prepared))) {
      fprintf(stderr, "network: %s explicit controller is absent or ambiguous\n", entry->name);
      free(controllers);
      return false;
    }
    if (!selected) {
      continue;
    }
    if (excluded(config, selected)) {
      fprintf(stderr, "network: %s selects an excluded wired controller\n", entry->name);
      free(controllers);
      return false;
    }
    for (size_t j = 0; j < i; ++j) {
      if (config->interfaces[j].resolved_controller == selected->controller_id) {
        fputs("network: controller selected by multiple interfaces\n", stderr);
        free(controllers);
        return false;
      }
    }
    entry->resolved_controller = selected->controller_id;
  }
  for (size_t i = 0; config->wired_auto && i < count; ++i) {
    const struct net_controller_reply *controller = &controllers[i];
    if (controller->interface_class != NET_INTERFACE_CLASS_ETHERNET || excluded(config, controller)) {
      continue;
    }
    bool reserved = false;
    for (size_t j = 0; j < explicit_count; ++j) {
      reserved |= config->interfaces[j].resolved_controller == controller->controller_id;
    }
    if (reserved) {
      continue;
    }
    char name[16];
    unsigned number = 0;
    do {
      snprintf(name, sizeof(name), "eth%u", number++);
    } while (name_used(config, name));
    if (!append_automatic(config, name, controller->controller_id)) {
      free(controllers);
      return false;
    }
  }
  free(controllers);
  return true;
}

enum call_status boot_network_bind(const struct network_config *config, const uint64_t *ids,
    handle_t registry, handle_t policy)
{
  struct net_interface_binding *bindings = calloc(config->count, sizeof(*bindings));
  uint32_t *controllers = calloc(config->count, sizeof(*controllers));
  if (config->count && (!bindings || !controllers)) {
    free(bindings);
    free(controllers);
    return CALL_NO_MEMORY;
  }
  size_t count = 0;
  for (size_t i = 0; i < config->count; ++i) {
    const struct network_interface_config *entry = &config->interfaces[i];
    handle_t facet;
    struct net_interface_reply interface;
    enum call_status status = net_registry_lookup(registry, ids[i + 1], &facet);
    if (status == CALL_OK) {
      status = net_interface_query(facet, &interface);
      handle_close(facet);
    }
    if (status != CALL_OK) {
      free(bindings);
      free(controllers);
      return status;
    }
    if (interface.flags & NET_INTERFACE_BOUND) {
      continue;
    }
    struct net_controller_reply selected = {0};
    uint32_t after = 0;
    bool complete = false;
    size_t matches = 0;
    bool pending_wifi_identity = false;
    for (;;) {
      struct net_controller_reply controller;
      status = net_registry_next_controller(registry, after, &controller);
      if (status != CALL_OK) {
        free(bindings);
        free(controllers);
        return status;
      }
      complete = (controller.flags & NET_CONTROLLER_INVENTORY_COMPLETE) != 0;
      if (!controller.controller_id) {
        break;
      }
      after = controller.controller_id;
      if (entry->interface_class == NET_INTERFACE_CLASS_WIFI &&
          controller.interface_class == NET_INTERFACE_CLASS_WIFI &&
          entry->selector.kind == NET_SELECT_MAC && controller.driver == NET_DRIVER_AX200 &&
          !memcmp(controller.mac, (uint8_t[6]){0}, 6)) {
        pending_wifi_identity = true;
      }
      if (!controller_matches(entry, &controller)) {
        continue;
      }
      if (excluded(config, &controller)) {
        if (entry->selector.kind == NET_SELECT_LINKED_CONTROLLER) {
          continue;
        }
        fprintf(stderr, "network: %s selects an excluded wired controller\n", entry->name);
        free(bindings);
        free(controllers);
        return CALL_BAD_REQUEST;
      }
      if (controller.flags & NET_CONTROLLER_BOUND) {
        if (entry->selector.kind != NET_SELECT_LINKED_CONTROLLER) {
          fprintf(stderr, "network: %s controller is already bound\n", entry->name);
          free(bindings);
          free(controllers);
          return CALL_BUSY;
        }
        continue;
      }
      ++matches;
      if (!selected.controller_id || rank(entry, &controller) < rank(entry, &selected) ||
          (rank(entry, &controller) == rank(entry, &selected) && controller.controller_id < selected.controller_id)) {
        selected = controller;
      }
    }
    if (matches > 1 && entry->selector.kind != NET_SELECT_LINKED_CONTROLLER) {
      fprintf(stderr, "network: %s selector is ambiguous\n", entry->name);
      free(bindings);
      free(controllers);
      return CALL_BUSY;
    }
    if (!selected.controller_id) {
      if (complete && !pending_wifi_identity && !entry->optional && entry->selector.kind != NET_SELECT_LINKED_CONTROLLER) {
        fprintf(stderr, "network: required %s controller is absent\n", entry->name);
        free(bindings);
        free(controllers);
        return CALL_NOT_FOUND;
      }
      continue;
    }
    for (size_t j = 0; j < count; ++j) {
      if (controllers[j] == selected.controller_id) {
        fprintf(stderr, "network: controller selected by multiple interfaces\n");
        free(bindings);
        free(controllers);
        return CALL_BAD_REQUEST;
      }
    }
    controllers[count] = selected.controller_id;
    bindings[count++] = (struct net_interface_binding){
      .interface_id = ids[i + 1],
      .selector = entry->selector.kind == NET_SELECT_LINKED_CONTROLLER ?
          (struct net_selector){.kind = NET_SELECT_LINKED_CONTROLLER, .controller_id = selected.controller_id} :
          entry->selector,
    };
  }
  enum call_status status = count ? net_policy_bind(policy, bindings, count) : CALL_OK;
  free(bindings);
  free(controllers);
  return status;
}

uint64_t boot_network_id(const struct boot_network *network, const char *name)
{
  if (!strcmp(name, "lo")) {
    return network->ids[0];
  }
  for (size_t i = 0; i < network->config.count; ++i) {
    if (!strcmp(name, network->config.interfaces[i].name)) {
      return network->ids[i + 1];
    }
  }
  return 0;
}

bool boot_network_prepare(struct boot_network *network, handle_t registry, handle_t policy,
    const struct config_handoff_bytes *bytes)
{
  *network = (struct boot_network){.policy = policy, .echo = HANDLE_INVALID,
    .udp = HANDLE_INVALID, .tcp = HANDLE_INVALID, .remote_interface = HANDLE_INVALID,
    .remote_tcp = HANDLE_INVALID, .remote_udp = HANDLE_INVALID};
  bool read = policy != HANDLE_INVALID && (bytes ?
      network_config_read_literal_bytes("system://config/network.lua", bytes->bytes, bytes->size,
        &network->config) == CONFIG_OK :
      network_config_read(&network->config));
  if (!read || !expand_wired(&network->config, registry)) {
    return false;
  }
  size_t count = network->config.count;
  network->ids = calloc(count + 1, sizeof(*network->ids));
  network->facets = calloc(count, sizeof(*network->facets));
  network->dhcp = calloc(count, sizeof(*network->dhcp));
  network->facet_names = calloc(count, sizeof(*network->facet_names));
  network->dhcp_names = calloc(count, sizeof(*network->dhcp_names));
  network->controller_names = calloc(count, sizeof(*network->controller_names));
  network->controller_values = calloc(count, sizeof(*network->controller_values));
  struct net_interface_definition *definitions = calloc(count, sizeof(*definitions));
  if (!network->ids || (count && (!network->facets || !network->dhcp ||
      !network->facet_names || !network->dhcp_names || !network->controller_names ||
      !network->controller_values || !definitions))) {
    free(definitions);
    return false;
  }
  for (size_t i = 0; i < count; ++i) {
    memcpy(definitions[i].name, network->config.interfaces[i].name, sizeof(definitions[i].name));
    definitions[i].interface_class = network->config.interfaces[i].interface_class;
    network->facets[i] = network->dhcp[i] = HANDLE_INVALID;
    snprintf(network->facet_names[i], sizeof(*network->facet_names), "config.%s", definitions[i].name);
    snprintf(network->dhcp_names[i], sizeof(*network->dhcp_names), "dhcp.%s", definitions[i].name);
    if (network->config.interfaces[i].resolved_controller) {
      snprintf(network->controller_names[i], sizeof(*network->controller_names), "network.%s", definitions[i].name);
      snprintf(network->controller_values[i], sizeof(*network->controller_values), "%u",
          network->config.interfaces[i].resolved_controller);
    }
  }
  enum call_status status = count ? net_policy_define(policy, definitions, count) : CALL_OK;
  free(definitions);
  if (status != CALL_OK) {
    fprintf(stderr, "boot-init: interface definition failed (status %u)\n", status);
    return false;
  }
  for (uint64_t after = 0;;) {
    struct net_interface_reply interface;
    status = net_registry_next_interface(registry, after, &interface);
    if (status != CALL_OK) {
      return false;
    }
    if (!interface.interface_id) {
      break;
    }
    after = interface.interface_id;
    if (!strcmp(interface.name, "lo")) {
      network->ids[0] = interface.interface_id;
    }
    for (size_t i = 0; i < count; ++i) {
      if (!strcmp(interface.name, network->config.interfaces[i].name)) {
        network->ids[i + 1] = interface.interface_id;
      }
    }
  }
  for (size_t i = 0; i <= count; ++i) {
    if (!network->ids[i]) {
      return false;
    }
  }
  status = boot_network_bind(&network->config, network->ids, registry, policy);
  if (status != CALL_OK && status != CALL_UNAVAILABLE && status != CALL_QUEUE_FULL && status != CALL_NO_MEMORY) {
    fprintf(stderr, "boot-init: interface binding failed (status %u)\n", status);
    return false;
  }
  struct net_policy_request settings = {
    .default_interface_id = boot_network_id(network, network->config.default_interface),
    .dns_interface_id = boot_network_id(network, network->config.dns_interface),
    .remote_interface_id = boot_network_id(network, network->config.remote_interface),
    .log_interface_id = boot_network_id(network, network->config.log_interface),
    .debug_interface_id = boot_network_id(network, network->config.debug_interface),
    .dns_server = network->config.dns_address,
    .dns_mode = network->config.dns_explicit ? NET_DNS_NUMERIC :
        boot_network_id(network, network->config.dns_interface) ? NET_DNS_DHCP : NET_DNS_NONE,
    .route_mode = network->config.routing_auto ? NET_ROUTE_AUTOMATIC : NET_ROUTE_PINNED,
  };
  if (network->config.dns_interface[0] && !settings.dns_interface_id) {
    settings.dns_mode = NET_DNS_NONE;
    settings.dns_server = 0;
  }
  if (net_policy_replace(policy, &settings) != CALL_OK ||
      net_policy_factory(policy, PROTOCOL_ECHO, ECHO_RIGHT_SEND, network->ids, count + 1, &network->echo) != CALL_OK ||
      net_policy_factory(policy, PROTOCOL_UDP_SERVICE, UDP_SERVICE_RIGHT_OPEN, network->ids, count + 1, &network->udp) != CALL_OK ||
      net_policy_factory(policy, PROTOCOL_TCP_SERVICE, TCP_SERVICE_RIGHTS, network->ids, count + 1, &network->tcp) != CALL_OK) {
    return false;
  }
  for (size_t i = 0; i < count; ++i) {
    if (net_policy_interface(policy, network->ids[i + 1], NET_INTERFACE_RIGHTS, &network->facets[i]) != CALL_OK ||
        (network->config.interfaces[i].dhcp && net_policy_factory(policy, PROTOCOL_UDP_SERVICE,
          UDP_SERVICE_RIGHT_BROADCAST, &network->ids[i + 1], 1, &network->dhcp[i]) != CALL_OK)) {
      return false;
    }
  }
  uint64_t remote = settings.remote_interface_id;
  if (remote && (net_policy_interface(policy, remote, NET_INTERFACE_RIGHT_READ, &network->remote_interface) != CALL_OK ||
      net_policy_factory(policy, PROTOCOL_TCP_SERVICE, TCP_SERVICE_RIGHTS, &remote, 1, &network->remote_tcp) != CALL_OK ||
      net_policy_factory(policy, PROTOCOL_UDP_SERVICE, UDP_SERVICE_RIGHT_BROADCAST, &remote, 1, &network->remote_udp) != CALL_OK)) {
    return false;
  }
  return true;
}
