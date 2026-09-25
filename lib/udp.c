#include <udp.h>
#include <syscall.h>

enum call_status udp_open(handle_t service, uint32_t address, uint16_t port,
    struct udp_open_reply *reply)
{
  if (!reply) {
    return CALL_BAD_REQUEST;
  }
  *reply = (struct udp_open_reply){.handle = HANDLE_INVALID};
  struct udp_open_request request = {
    .header = {PROTOCOL_UDP_SERVICE, UDP_OPEN}, .address = address, .port = port,
  };
  struct udp_open_reply response;
  struct syscall_result result = syscall_call(service, &request, sizeof(request),
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

enum call_status udp_inspect(handle_t endpoint, struct udp_endpoint_info *reply)
{
  if (!reply) {
    return CALL_BAD_REQUEST;
  }
  *reply = (struct udp_endpoint_info){0};
  struct message_header request = {PROTOCOL_UDP, UDP_INSPECT};
  struct udp_endpoint_info response;
  struct syscall_result result = syscall_call(endpoint, &request, sizeof(request),
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

enum call_status udp_shutdown(handle_t endpoint)
{
  struct message_header request = {PROTOCOL_UDP, UDP_SHUTDOWN};
  struct syscall_result result = syscall_call(endpoint, &request, sizeof(request), NULL, 0);
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  return result.reply_size ? CALL_BAD_REQUEST : result.status;
}
