#include <net_config.h>
#include <syscall.h>

enum call_status net_config_query(handle_t authority, struct net_config_reply *reply)
{
  if (!reply) {
    return CALL_BAD_REQUEST;
  }
  *reply = (struct net_config_reply){0};
  struct message_header request = {PROTOCOL_NET_CONFIG, NET_CONFIG_QUERY};
  struct net_config_reply response;
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

enum call_status net_config_replace(handle_t authority, uint32_t address,
    uint32_t prefix, uint32_t gateway)
{
  struct net_config_request request = {
    .header = {PROTOCOL_NET_CONFIG, NET_CONFIG_REPLACE},
    .address = address, .prefix = prefix, .gateway = gateway,
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
