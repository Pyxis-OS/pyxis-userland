#include <echo.h>
#include <tcp.h>
#include <udp.h>
#include <abi/net_factory.h>
#include <syscall.h>

static enum call_status next_interface(handle_t authority, uint64_t protocol,
    uint64_t operation, uint64_t after_id, uint64_t *interface_id)
{
  if (!interface_id) {
    return CALL_BAD_REQUEST;
  }
  *interface_id = 0;
  struct net_factory_next_interface_request request = {
    .header = {protocol, operation}, .after_id = after_id,
  };
  struct net_factory_next_interface_reply reply;
  struct syscall_result result = syscall_call(authority, &request, sizeof(request),
      &reply, sizeof(reply));
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  if (result.reply_size != (result.status == CALL_OK ? sizeof(reply) : 0)) {
    return CALL_BAD_REQUEST;
  }
  if (result.status == CALL_OK) {
    if (reply.interface_id && reply.interface_id <= after_id) {
      return CALL_BAD_REQUEST;
    }
    *interface_id = reply.interface_id;
  }
  return result.status;
}

static enum call_status derive(handle_t authority, uint64_t protocol,
    uint64_t operation, uint64_t interface_id, uint64_t rights, handle_t *derived)
{
  if (!derived) {
    return CALL_BAD_REQUEST;
  }
  *derived = HANDLE_INVALID;
  struct net_factory_derive_request request = {
    .header = {protocol, operation}, .interface_id = interface_id, .rights = rights,
  };
  struct net_factory_derive_reply reply;
  struct syscall_result result = syscall_call(authority, &request, sizeof(request),
      &reply, sizeof(reply));
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  if (result.reply_size != (result.status == CALL_OK ? sizeof(reply) : 0)) {
    return CALL_BAD_REQUEST;
  }
  if (result.status == CALL_OK) {
    if (reply.handle == HANDLE_INVALID) {
      return CALL_BAD_REQUEST;
    }
    *derived = reply.handle;
  }
  return result.status;
}

enum call_status tcp_factory_next_interface(handle_t authority, uint64_t after_id,
    uint64_t *interface_id)
{
  return next_interface(authority, PROTOCOL_TCP_SERVICE, TCP_SERVICE_NEXT_INTERFACE,
      after_id, interface_id);
}

enum call_status tcp_factory_derive(handle_t authority, uint64_t interface_id,
    uint64_t rights, handle_t *derived)
{
  return derive(authority, PROTOCOL_TCP_SERVICE, TCP_SERVICE_DERIVE, interface_id, rights, derived);
}

enum call_status udp_factory_next_interface(handle_t authority, uint64_t after_id,
    uint64_t *interface_id)
{
  return next_interface(authority, PROTOCOL_UDP_SERVICE, UDP_SERVICE_NEXT_INTERFACE,
      after_id, interface_id);
}

enum call_status udp_factory_derive(handle_t authority, uint64_t interface_id,
    uint64_t rights, handle_t *derived)
{
  return derive(authority, PROTOCOL_UDP_SERVICE, UDP_SERVICE_DERIVE, interface_id, rights, derived);
}

enum call_status echo_factory_next_interface(handle_t authority, uint64_t after_id,
    uint64_t *interface_id)
{
  return next_interface(authority, PROTOCOL_ECHO, ECHO_NEXT_INTERFACE,
      after_id, interface_id);
}

enum call_status echo_factory_derive(handle_t authority, uint64_t interface_id,
    uint64_t rights, handle_t *derived)
{
  return derive(authority, PROTOCOL_ECHO, ECHO_DERIVE, interface_id, rights, derived);
}
