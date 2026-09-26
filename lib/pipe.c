#include <pipe.h>
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

static enum call_status mutation_status(struct syscall_result result, size_t size)
{
  if (result.status >= CALL_STATUS_COUNT ||
      result.reply_size != (result.status == CALL_OK ? size : 0)) {
    return CALL_OUTCOME_UNKNOWN;
  }
  return result.status;
}

enum call_status pipe_create(handle_t service, struct pipe_create_reply *reply)
{
  if (!reply) {
    return CALL_BAD_REQUEST;
  }
  struct message_header request = {PROTOCOL_PIPE_SERVICE, PIPE_CREATE};
  struct pipe_create_reply response;
  enum call_status status = response_status(syscall_call(service, &request, sizeof(request),
      &response, sizeof(response)), sizeof(response));
  if (status == CALL_OK) {
    *reply = response;
  }
  return status;
}

enum call_status pipe_read(handle_t reader, void *bytes, size_t capacity, size_t *read)
{
  if (!read) {
    return CALL_BAD_REQUEST;
  }
  *read = 0;
  size_t limit = capacity < PIPE_READ_MAX_BYTES ? capacity : PIPE_READ_MAX_BYTES;
  struct pipe_read_request request = {
    .header = {PROTOCOL_PIPE, PIPE_READ},
    .buffer = (uintptr_t)bytes, .capacity = limit,
  };
  struct pipe_read_reply response;
  enum call_status status = response_status(syscall_call(reader, &request, sizeof(request),
      &response, sizeof(response)), sizeof(response));
  if (status != CALL_OK) {
    return status;
  }
  if (response.length > limit) {
    return CALL_BAD_REQUEST;
  }
  *read = response.length;
  return CALL_OK;
}

enum call_status pipe_write(handle_t writer, const void *bytes, size_t length, size_t *written)
{
  if (!written) {
    return CALL_BAD_REQUEST;
  }
  *written = 0;
  size_t limit = length < PIPE_WRITE_MAX_BYTES ? length : PIPE_WRITE_MAX_BYTES;
  struct pipe_write_request request = {
    .header = {PROTOCOL_PIPE, PIPE_WRITE},
    .buffer = (uintptr_t)bytes, .length = limit,
  };
  struct pipe_write_reply response;
  enum call_status status = mutation_status(syscall_call(writer, &request, sizeof(request),
      &response, sizeof(response)), sizeof(response));
  if (status != CALL_OK) {
    return status;
  }
  if (response.length > limit || (limit && !response.length)) {
    return CALL_OUTCOME_UNKNOWN;
  }
  *written = response.length;
  return CALL_OK;
}
