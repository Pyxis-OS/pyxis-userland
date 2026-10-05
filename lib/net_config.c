#include <net_config.h>
#include <syscall.h>

static enum call_status snapshot_call(handle_t authority, const void *request,
    size_t request_size, struct net_config_reply *reply)
{
  if (!reply) {
    return CALL_BAD_REQUEST;
  }
  *reply = (struct net_config_reply){0};
  struct net_config_reply response;
  struct syscall_result result = syscall_call(authority, request, request_size,
      &response, sizeof(response));
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  if (result.reply_size != (result.status == CALL_OK ? sizeof(response) : 0)) {
    return CALL_BAD_REQUEST;
  }
  if (result.status == CALL_OK) {
    *reply = response;
  }
  return result.status;
}

enum call_status net_config_query(handle_t authority, struct net_config_reply *reply)
{
  struct message_header request = {PROTOCOL_NET_CONFIG, NET_CONFIG_QUERY};
  return snapshot_call(authority, &request, sizeof(request), reply);
}

enum call_status net_config_next_controller(handle_t authority, uint32_t after_id,
    struct net_controller_reply *reply)
{
  if (!reply) {
    return CALL_BAD_REQUEST;
  }
  *reply = (struct net_controller_reply){0};
  struct net_controller_request request = {
    .header = {PROTOCOL_NET_CONFIG, NET_CONFIG_NEXT_CONTROLLER}, .after_id = after_id,
  };
  struct net_controller_reply response;
  struct syscall_result result = syscall_call(authority, &request, sizeof(request),
      &response, sizeof(response));
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  if (result.reply_size != (result.status == CALL_OK ? sizeof(response) : 0)) {
    return CALL_BAD_REQUEST;
  }
  if (result.status == CALL_OK) {
    *reply = response;
  }
  return result.status;
}

static enum call_status select_call(handle_t authority, uint64_t operation,
    const struct net_selector *selector, struct net_config_reply *reply)
{
  if (!selector) {
    if (reply) {
      *reply = (struct net_config_reply){0};
    }
    return CALL_BAD_REQUEST;
  }
  struct net_select_request request = {
    .header = {PROTOCOL_NET_CONFIG, operation},
    .selector = *selector,
  };
  return snapshot_call(authority, &request, sizeof(request), reply);
}

enum call_status net_config_bind(handle_t authority, const struct net_selector *selector,
    struct net_config_reply *reply)
{
  return select_call(authority, NET_CONFIG_BIND, selector, reply);
}

enum call_status net_config_lookup(handle_t authority, const struct net_selector *selector,
    struct net_config_reply *reply)
{
  return select_call(authority, NET_CONFIG_LOOKUP, selector, reply);
}

enum call_status net_config_replace(handle_t authority, uint32_t address,
    uint32_t prefix, uint32_t gateway, uint32_t dns_server)
{
  struct net_config_request request = {
    .header = {PROTOCOL_NET_CONFIG, NET_CONFIG_REPLACE},
    .address = address, .prefix = prefix, .gateway = gateway, .dns_server = dns_server,
  };
  struct syscall_result result = syscall_call(authority, &request, sizeof(request), NULL, 0);
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  return result.reply_size ? CALL_BAD_REQUEST : result.status;
}

enum call_status net_config_set_dns(handle_t authority, uint32_t dns_server)
{
  struct net_dns_request request = {
    .header = {PROTOCOL_NET_CONFIG, NET_CONFIG_SET_DNS}, .dns_server = dns_server,
  };
  struct syscall_result result = syscall_call(authority, &request, sizeof(request), NULL, 0);
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  return result.reply_size ? CALL_BAD_REQUEST : result.status;
}

enum call_status net_config_clear(handle_t authority)
{
  struct message_header request = {PROTOCOL_NET_CONFIG, NET_CONFIG_CLEAR};
  struct syscall_result result = syscall_call(authority, &request, sizeof(request), NULL, 0);
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  return result.reply_size ? CALL_BAD_REQUEST : result.status;
}
