#include "network.h"
#include <net_config.h>
#include <handle.h>
#include <abi/echo.h>
#include <abi/tcp.h>
#include <abi/udp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
      if (controller.interface_class != entry->interface_class) {
        continue;
      }
      bool matches_selector = entry->selector.kind == NET_SELECT_MAC ?
          !memcmp(entry->selector.mac, controller.mac, sizeof(controller.mac)) :
          entry->selector.kind == NET_SELECT_VIRTIO ? controller.driver == NET_DRIVER_VIRTIO :
          (controller.flags & (NET_CONTROLLER_PREPARED | NET_CONTROLLER_CARRIER_KNOWN | NET_CONTROLLER_LINK_UP)) ==
              (NET_CONTROLLER_PREPARED | NET_CONTROLLER_CARRIER_KNOWN | NET_CONTROLLER_LINK_UP);
      if (!matches_selector) {
        continue;
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
      if (complete && !entry->optional && entry->selector.kind != NET_SELECT_LINKED_CONTROLLER) {
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

bool boot_network_prepare(struct boot_network *network, handle_t registry, handle_t policy)
{
  *network = (struct boot_network){.policy = policy, .echo = HANDLE_INVALID,
    .udp = HANDLE_INVALID, .tcp = HANDLE_INVALID, .remote_interface = HANDLE_INVALID,
    .remote_tcp = HANDLE_INVALID, .remote_udp = HANDLE_INVALID};
  if (policy == HANDLE_INVALID || !network_config_read(&network->config)) {
    return false;
  }
  size_t count = network->config.count;
  network->ids = calloc(count + 1, sizeof(*network->ids));
  network->facets = calloc(count, sizeof(*network->facets));
  network->dhcp = calloc(count, sizeof(*network->dhcp));
  network->facet_names = calloc(count, sizeof(*network->facet_names));
  network->dhcp_names = calloc(count, sizeof(*network->dhcp_names));
  struct net_interface_definition *definitions = calloc(count, sizeof(*definitions));
  if (!network->ids || (count && (!network->facets || !network->dhcp ||
      !network->facet_names || !network->dhcp_names || !definitions))) {
    free(definitions);
    return false;
  }
  for (size_t i = 0; i < count; ++i) {
    memcpy(definitions[i].name, network->config.interfaces[i].name, sizeof(definitions[i].name));
    definitions[i].interface_class = network->config.interfaces[i].interface_class;
    network->facets[i] = network->dhcp[i] = HANDLE_INVALID;
    snprintf(network->facet_names[i], sizeof(*network->facet_names), "config.%s", definitions[i].name);
    snprintf(network->dhcp_names[i], sizeof(*network->dhcp_names), "dhcp.%s", definitions[i].name);
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
    .dns_mode = !network->config.dns_interface[0] ? NET_DNS_NONE :
        network->config.dns_explicit ? NET_DNS_NUMERIC : NET_DNS_DHCP,
  };
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
