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

enum call_status tcp_listen(handle_t service, uint32_t address, uint16_t port,
    struct tcp_listen_reply *reply)
{
  if (!reply) {
    return CALL_BAD_REQUEST;
  }
  struct tcp_listen_request request = {
    .header = {PROTOCOL_TCP_SERVICE, TCP_LISTEN},
    .address = address, .port = port,
  };
  struct tcp_listen_reply response;
  struct syscall_result result = syscall_call(service, &request, sizeof(request),
      &response, sizeof(response));
  enum call_status status = response_status(result, sizeof(response));
  if (status == CALL_OK) {
    *reply = response;
  }
  return status;
}

enum call_status tcp_listener_inspect(handle_t listener, struct tcp_listener_info *reply)
{
  if (!reply) {
    return CALL_BAD_REQUEST;
  }
  struct message_header request = {PROTOCOL_TCP_LISTENER, TCP_LISTENER_INSPECT};
  struct tcp_listener_info response;
  struct syscall_result result = syscall_call(listener, &request, sizeof(request),
      &response, sizeof(response));
  enum call_status status = response_status(result, sizeof(response));
  if (status == CALL_OK) {
    *reply = response;
  }
  return status;
}

enum call_status tcp_accept(handle_t listener, uint64_t deadline_ns,
    struct tcp_accept_reply *reply)
{
  if (!reply) {
    return CALL_BAD_REQUEST;
  }
  struct tcp_accept_request request = {
    .header = {PROTOCOL_TCP_LISTENER, TCP_ACCEPT},
    .deadline_ns = deadline_ns,
  };
  struct tcp_accept_reply response;
  struct syscall_result result = syscall_call(listener, &request, sizeof(request),
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

enum call_status tcp_try_accept(handle_t listener, struct tcp_accept_reply *reply)
{
  if (!reply) {
    return CALL_BAD_REQUEST;
  }
  struct message_header request = {PROTOCOL_TCP_LISTENER, TCP_TRY_ACCEPT};
  struct tcp_accept_reply response;
  struct syscall_result result = syscall_call(listener, &request, sizeof(request),
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

enum call_status tcp_shutdown_write(handle_t stream)
{
  struct message_header request = {PROTOCOL_TCP, TCP_SHUTDOWN_WRITE};
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

enum call_status tcp_try_read(handle_t stream, void *data, size_t capacity,
    struct tcp_read_reply *reply)
{
  if (!reply) {
    return CALL_BAD_REQUEST;
  }
  size_t checked = capacity < TCP_READ_MAX_BYTES ? capacity : TCP_READ_MAX_BYTES;
  uintptr_t buffer = (uintptr_t)data, output = (uintptr_t)reply;
  if (checked && (buffer <= output ? output - buffer < checked : buffer - output < sizeof(*reply))) {
    return CALL_BAD_REQUEST;
  }
  struct tcp_try_read_request request = {
    .header = {PROTOCOL_TCP, TCP_TRY_READ}, .buffer = buffer, .capacity = capacity,
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

enum call_status tcp_try_write(handle_t stream, const void *data, size_t length,
    struct tcp_write_reply *reply)
{
  if (!reply) {
    return CALL_BAD_REQUEST;
  }
  struct tcp_try_write_request request = {
    .header = {PROTOCOL_TCP, TCP_TRY_WRITE}, .buffer = (uintptr_t)data, .length = length,
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
