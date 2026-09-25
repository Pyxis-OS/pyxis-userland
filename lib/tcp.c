#include <tcp.h>
#include <syscall.h>

static enum call_status response_status(struct syscall_result result, size_t size)
{
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  if (result.reply_size != (result.status == CALL_OK ? size : 0)) {
    return CALL_BAD_REQUEST;
  }
  return result.status;
}

enum call_status tcp_connect(handle_t service, uint32_t address, uint16_t port,
    uint64_t deadline_ns, struct tcp_connect_reply *reply)
{
  if (!reply) {
    return CALL_BAD_REQUEST;
  }
  struct tcp_connect_request request = {
    .header = {PROTOCOL_TCP_SERVICE, TCP_CONNECT},
    .address = address, .port = port, .deadline_ns = deadline_ns,
  };
  struct tcp_connect_reply response;
  struct syscall_result result = syscall_call(service, &request, sizeof(request),
      &response, sizeof(response));
  enum call_status status = response_status(result, sizeof(response));
  if (status == CALL_OK) {
    *reply = response;
  }
  return status;
}

enum call_status tcp_inspect(handle_t stream, struct tcp_connection_info *reply)
{
  if (!reply) {
    return CALL_BAD_REQUEST;
  }
  struct message_header request = {PROTOCOL_TCP, TCP_INSPECT};
  struct tcp_connection_info response;
  struct syscall_result result = syscall_call(stream, &request, sizeof(request),
      &response, sizeof(response));
  enum call_status status = response_status(result, sizeof(response));
  if (status == CALL_OK) {
    *reply = response;
  }
  return status;
}

enum call_status tcp_abort(handle_t stream)
{
  struct message_header request = {PROTOCOL_TCP, TCP_ABORT};
  return response_status(syscall_call(stream, &request, sizeof(request), NULL, 0), 0);
}

enum call_status tcp_read(handle_t stream, void *data, size_t capacity,
    uint64_t deadline_ns, struct tcp_read_reply *reply)
{
  if (!reply) {
    return CALL_BAD_REQUEST;
  }
  size_t checked = capacity < TCP_READ_MAX_BYTES ? capacity : TCP_READ_MAX_BYTES;
  uintptr_t buffer = (uintptr_t)data, output = (uintptr_t)reply;
  if (checked && (buffer <= output ? output - buffer < checked : buffer - output < sizeof(*reply))) {
    return CALL_BAD_REQUEST;
  }
  struct tcp_read_request request = {
    .header = {PROTOCOL_TCP, TCP_READ}, .buffer = buffer,
    .capacity = capacity, .deadline_ns = deadline_ns,
  };
  struct tcp_read_reply response;
  struct syscall_result result = syscall_call(stream, &request, sizeof(request),
      &response, sizeof(response));
  enum call_status status = response_status(result, sizeof(response));
  if (status == CALL_OK) {
    if (response.length > checked) {
      return CALL_BAD_REQUEST;
    }
    *reply = response;
  }
  return status;
}

enum call_status tcp_write(handle_t stream, const void *data, size_t length,
    uint64_t deadline_ns, struct tcp_write_reply *reply)
{
  if (!reply) {
    return CALL_BAD_REQUEST;
  }
  struct tcp_write_request request = {
    .header = {PROTOCOL_TCP, TCP_WRITE}, .buffer = (uintptr_t)data,
    .length = length, .deadline_ns = deadline_ns,
  };
  struct tcp_write_reply response;
  struct syscall_result result = syscall_call(stream, &request, sizeof(request),
      &response, sizeof(response));
  enum call_status status = response_status(result, sizeof(response));
  if (status == CALL_OK) {
    size_t limit = length < TCP_WRITE_MAX_BYTES ? length : TCP_WRITE_MAX_BYTES;
    if (response.length > limit || (length && !response.length)) {
      return CALL_BAD_REQUEST;
    }
    *reply = response;
  }
  return status;
}
