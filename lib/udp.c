#include <udp.h>
#include <syscall.h>

static enum call_status open_endpoint(handle_t service, uint64_t operation, uint32_t address, uint16_t port,
    struct udp_open_reply *reply)
{
  if (!reply) {
    return CALL_BAD_REQUEST;
  }
  *reply = (struct udp_open_reply){.handle = HANDLE_INVALID};
  struct udp_open_request request = {
    .header = {PROTOCOL_UDP_SERVICE, operation}, .address = address, .port = port,
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

enum call_status udp_open(handle_t service, uint32_t address, uint16_t port,
    struct udp_open_reply *reply)
{
  return open_endpoint(service, UDP_OPEN, address, port, reply);
}

enum call_status udp_open_route(handle_t service, uint32_t destination, uint16_t port,
    struct udp_open_reply *reply)
{
  return open_endpoint(service, UDP_OPEN_ROUTE, destination, port, reply);
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

enum call_status udp_send(handle_t endpoint, uint32_t address, uint16_t port,
    const void *data, size_t length, uint64_t deadline_ns)
{
  struct udp_send_request request = {
    .header = {PROTOCOL_UDP, UDP_SEND}, .address = address, .port = port,
    .buffer = (uintptr_t)data, .length = length, .deadline_ns = deadline_ns,
  };
  struct syscall_result result = syscall_call(endpoint, &request, sizeof(request), NULL, 0);
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  return result.reply_size ? CALL_BAD_REQUEST : result.status;
}

enum call_status udp_receive(handle_t endpoint, void *data, size_t capacity,
    uint64_t deadline_ns, struct udp_receive_reply *reply)
{
  if (!reply) {
    return CALL_BAD_REQUEST;
  }
  /* Reject overlap before clearing metadata; failure must preserve data. */
  size_t checked = capacity < UDP_MAX_PAYLOAD ? capacity : UDP_MAX_PAYLOAD;
  uintptr_t buffer = (uintptr_t)data, output = (uintptr_t)reply;
  if (checked && (buffer <= output ? output - buffer < checked : buffer - output < sizeof(*reply))) {
    return CALL_BAD_REQUEST;
  }
  *reply = (struct udp_receive_reply){0};
  struct udp_receive_request request = {
    .header = {PROTOCOL_UDP, UDP_RECEIVE}, .buffer = buffer,
    .capacity = capacity, .deadline_ns = deadline_ns,
  };
  struct udp_receive_reply response;
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
