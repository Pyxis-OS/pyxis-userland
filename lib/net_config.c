#include <net_config.h>
#include <syscall.h>
#include <string.h>

static enum call_status exchange(handle_t authority, const void *request, size_t request_size,
    void *reply, size_t reply_size)
{
  struct syscall_result result = syscall_call(authority, request, request_size, reply, reply_size);
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  return result.reply_size == (result.status == CALL_OK ? reply_size : 0) ?
      result.status : CALL_BAD_REQUEST;
}

static enum call_status returned_handle(handle_t authority, const void *request, size_t size,
    handle_t *handle)
{
  if (!handle) {
    return CALL_BAD_REQUEST;
  }
  *handle = HANDLE_INVALID;
  struct net_handle_reply reply;
  enum call_status status = exchange(authority, request, size, &reply, sizeof(reply));
  if (status == CALL_OK) {
    if (reply.handle == HANDLE_INVALID) {
      return CALL_BAD_REQUEST;
    }
    *handle = reply.handle;
  }
  return status;
}

enum call_status net_registry_next_controller(handle_t registry, uint32_t after_id,
    struct net_controller_reply *reply)
{
  if (!reply) {
    return CALL_BAD_REQUEST;
  }
  *reply = (struct net_controller_reply){0};
  struct net_controller_request request = {
    .header = {PROTOCOL_NET_REGISTRY, NET_REGISTRY_NEXT_CONTROLLER}, .after_id = after_id,
  };
  struct net_controller_reply response;
  enum call_status status = exchange(registry, &request, sizeof(request), &response, sizeof(response));
  if (status == CALL_OK) {
    const uint32_t flags = NET_CONTROLLER_INVENTORY_COMPLETE | NET_CONTROLLER_PREPARED |
        NET_CONTROLLER_CARRIER_KNOWN | NET_CONTROLLER_LINK_UP | NET_CONTROLLER_BOUND |
        NET_CONTROLLER_SPEED_VALID;
    if (response.flags & ~flags || response.reserved_mac[0] || response.reserved_mac[1]) {
      return CALL_BAD_REQUEST;
    }
    if (!response.controller_id) {
      const struct net_controller_reply end = {.flags = response.flags};
      if ((response.flags & ~NET_CONTROLLER_INVENTORY_COMPLETE) || memcmp(&response, &end, sizeof(end))) {
        return CALL_BAD_REQUEST;
      }
    } else if (response.controller_id <= after_id) {
      return CALL_BAD_REQUEST;
    }
    if (response.flags & NET_CONTROLLER_SPEED_VALID) {
      if (!response.tx_bits_per_second || !response.rx_bits_per_second) {
        return CALL_BAD_REQUEST;
      }
    } else if (response.tx_bits_per_second || response.rx_bits_per_second) {
      return CALL_BAD_REQUEST;
    }
    *reply = response;
  }
  return status;
}

static bool interface_valid(const struct net_interface_reply *reply)
{
  if (!reply->interface_id || reply->flags & ~NET_INTERFACE_FLAGS ||
      reply->reserved[0] || reply->reserved[1] || !(reply->flags & NET_INTERFACE_DEFINED)) {
    return false;
  }
  if (reply->flags & NET_INTERFACE_SPEED_VALID) {
    if (!reply->tx_bits_per_second || !reply->rx_bits_per_second) {
      return false;
    }
  } else if (reply->tx_bits_per_second || reply->rx_bits_per_second) {
    return false;
  }
  const char *end = memchr(reply->name, 0, sizeof(reply->name));
  if (!end || end == reply->name) {
    return false;
  }
  for (const char *tail = end + 1; tail < reply->name + sizeof(reply->name); ++tail) {
    if (*tail) {
      return false;
    }
  }
  return true;
}

enum call_status net_registry_next_interface(handle_t registry, uint64_t after_id,
    struct net_interface_reply *reply)
{
  if (!reply) {
    return CALL_BAD_REQUEST;
  }
  *reply = (struct net_interface_reply){0};
  struct net_interface_request request = {
    .header = {PROTOCOL_NET_REGISTRY, NET_REGISTRY_NEXT_INTERFACE}, .after_id = after_id,
  };
  struct net_interface_reply response;
  enum call_status status = exchange(registry, &request, sizeof(request), &response, sizeof(response));
  if (status == CALL_OK) {
    if (!response.interface_id) {
      const struct net_interface_reply end = {.flags = response.flags};
      if ((response.flags & ~NET_INTERFACE_INVENTORY_COMPLETE) || memcmp(&response, &end, sizeof(end))) {
        return CALL_BAD_REQUEST;
      }
    } else if (response.interface_id <= after_id || !interface_valid(&response)) {
      return CALL_BAD_REQUEST;
    }
    *reply = response;
  }
  return status;
}

enum call_status net_registry_lookup(handle_t registry, uint64_t interface_id, handle_t *facet)
{
  struct net_lookup_request request = {
    .header = {PROTOCOL_NET_REGISTRY, NET_REGISTRY_LOOKUP}, .interface_id = interface_id,
  };
  return returned_handle(registry, &request, sizeof(request), facet);
}

enum call_status net_registry_policy(handle_t registry, struct net_policy_reply *reply)
{
  if (!reply) {
    return CALL_BAD_REQUEST;
  }
  *reply = (struct net_policy_reply){0};
  struct message_header request = {PROTOCOL_NET_REGISTRY, NET_REGISTRY_POLICY};
  struct net_policy_reply response;
  enum call_status status = exchange(registry, &request, sizeof(request), &response, sizeof(response));
  if (status == CALL_OK) {
    if (response.flags & ~NET_POLICY_DNS_VALID || response.dns_mode > NET_DNS_DHCP ||
        response.route_mode > NET_ROUTE_AUTOMATIC ||
        !!(response.flags & NET_POLICY_DNS_VALID) != !!response.dns_server) {
      return CALL_BAD_REQUEST;
    }
    if ((response.dns_mode == NET_DNS_NONE && (response.dns_interface_id || response.dns_server)) ||
        (response.dns_mode == NET_DNS_NUMERIC && !response.dns_server) ||
        (response.dns_mode == NET_DNS_DHCP && !response.dns_interface_id)) {
      return CALL_BAD_REQUEST;
    }
    *reply = response;
  }
  return status;
}

enum call_status net_interface_query(handle_t facet, struct net_interface_reply *reply)
{
  if (!reply) {
    return CALL_BAD_REQUEST;
  }
  *reply = (struct net_interface_reply){0};
  struct message_header request = {PROTOCOL_NET_INTERFACE, NET_INTERFACE_QUERY};
  struct net_interface_reply response;
  enum call_status status = exchange(facet, &request, sizeof(request), &response, sizeof(response));
  if (status == CALL_OK) {
    if (!interface_valid(&response)) {
      return CALL_BAD_REQUEST;
    }
    *reply = response;
  }
  return status;
}

enum call_status net_interface_replace(handle_t facet, uint32_t address, uint32_t prefix,
    uint32_t gateway, uint32_t dns_server)
{
  struct net_config_request request = {
    .header = {PROTOCOL_NET_INTERFACE, NET_INTERFACE_REPLACE},
    .address = address, .prefix = prefix, .gateway = gateway, .dns_server = dns_server,
  };
  return exchange(facet, &request, sizeof(request), NULL, 0);
}

enum call_status net_interface_clear(handle_t facet)
{
  struct message_header request = {PROTOCOL_NET_INTERFACE, NET_INTERFACE_CLEAR};
  return exchange(facet, &request, sizeof(request), NULL, 0);
}

enum call_status net_interface_report_dhcp(handle_t facet, uint64_t address_epoch, uint32_t phase)
{
  struct net_dhcp_report_request request = {
    .header = {PROTOCOL_NET_INTERFACE, NET_INTERFACE_REPORT_DHCP},
    .address_epoch = address_epoch, .phase = phase,
  };
  return exchange(facet, &request, sizeof(request), NULL, 0);
}

static enum call_status batch(handle_t policy, uint64_t operation, const void *entries, size_t count)
{
  if (count && !entries) {
    return CALL_BAD_REQUEST;
  }
  struct net_policy_batch_request request = {
    .header = {PROTOCOL_NET_POLICY, operation}, .entries = (uintptr_t)entries, .count = count,
  };
  return exchange(policy, &request, sizeof(request), NULL, 0);
}

enum call_status net_policy_define(handle_t policy, const struct net_interface_definition *entries, size_t count)
{
  return batch(policy, NET_POLICY_DEFINE, entries, count);
}

enum call_status net_policy_bind(handle_t policy, const struct net_interface_binding *entries, size_t count)
{
  return batch(policy, NET_POLICY_BIND, entries, count);
}

enum call_status net_policy_interface(handle_t policy, uint64_t interface_id, uint64_t rights, handle_t *facet)
{
  struct net_policy_interface_request request = {
    .header = {PROTOCOL_NET_POLICY, NET_POLICY_INTERFACE}, .interface_id = interface_id, .rights = rights,
  };
  return returned_handle(policy, &request, sizeof(request), facet);
}

enum call_status net_policy_factory(handle_t policy, uint64_t protocol, uint64_t rights,
    const uint64_t *interface_ids, size_t count, handle_t *factory)
{
  if (!interface_ids || !count) {
    if (factory) {
      *factory = HANDLE_INVALID;
    }
    return CALL_BAD_REQUEST;
  }
  struct net_policy_factory_request request = {
    .header = {PROTOCOL_NET_POLICY, NET_POLICY_FACTORY}, .protocol = protocol, .rights = rights,
    .interface_ids = (uintptr_t)interface_ids, .count = count,
  };
  return returned_handle(policy, &request, sizeof(request), factory);
}

enum call_status net_policy_replace(handle_t policy, const struct net_policy_request *settings)
{
  if (!settings) {
    return CALL_BAD_REQUEST;
  }
  struct net_policy_request request = *settings;
  request.header = (struct message_header){PROTOCOL_NET_POLICY, NET_POLICY_REPLACE};
  return exchange(policy, &request, sizeof(request), NULL, 0);
}
