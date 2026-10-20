#include <pipe.h>
#include <stdbool.h>
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

enum call_status pipe_create_values(handle_t service, struct pipe_create_reply *reply)
{
  if (!reply) {
    return CALL_BAD_REQUEST;
  }
  struct pipe_create_options request = {{PROTOCOL_PIPE_SERVICE, PIPE_CREATE}, PIPE_CREATE_VALUES};
  struct pipe_create_reply response;
  enum call_status status = response_status(syscall_call(service, &request, sizeof(request),
      &response, sizeof(response)), sizeof(response));
  if (status == CALL_OK) {
    *reply = response;
  }
  return status;
}

static enum call_status pipe_read_call(handle_t reader, uint64_t operation,
    void *bytes, size_t capacity, size_t *read)
{
  if (!read) {
    return CALL_BAD_REQUEST;
  }
  *read = 0;
  size_t limit = capacity < PIPE_READ_MAX_BYTES ? capacity : PIPE_READ_MAX_BYTES;
  struct pipe_read_request request = {
    .header = {PROTOCOL_PIPE, operation},
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

enum call_status pipe_read(handle_t reader, void *bytes, size_t capacity, size_t *read)
{
  return pipe_read_call(reader, PIPE_READ, bytes, capacity, read);
}

enum call_status pipe_try_read(handle_t reader, void *bytes, size_t capacity, size_t *read)
{
  return pipe_read_call(reader, PIPE_TRY_READ, bytes, capacity, read);
}

static enum call_status pipe_write_call(handle_t writer, uint64_t operation,
    const void *bytes, size_t length, size_t *written)
{
  if (!written) {
    return CALL_BAD_REQUEST;
  }
  *written = 0;
  size_t limit = length < PIPE_WRITE_MAX_BYTES ? length : PIPE_WRITE_MAX_BYTES;
  struct pipe_write_request request = {
    .header = {PROTOCOL_PIPE, operation},
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

enum call_status pipe_write(handle_t writer, const void *bytes, size_t length, size_t *written)
{
  return pipe_write_call(writer, PIPE_WRITE, bytes, length, written);
}

enum call_status pipe_try_write(handle_t writer, const void *bytes, size_t length, size_t *written)
{
  return pipe_write_call(writer, PIPE_TRY_WRITE, bytes, length, written);
}

static enum call_status pipe_handoff_call(handle_t writer, uint64_t operation, handle_t *value)
{
  if (!value || !*value) {
    return CALL_BAD_REQUEST;
  }
  struct pipe_handoff_request request = {{PROTOCOL_PIPE, operation}, *value};
  enum call_status status = mutation_status(syscall_call(writer, &request, sizeof(request), NULL, 0), 0);
  if (status == CALL_OK) {
    *value = HANDLE_INVALID;
  }
  return status;
}

enum call_status pipe_handoff(handle_t writer, handle_t *value)
{
  return pipe_handoff_call(writer, PIPE_HANDOFF, value);
}

enum call_status pipe_try_handoff(handle_t writer, handle_t *value)
{
  return pipe_handoff_call(writer, PIPE_TRY_HANDOFF, value);
}

static enum call_status pipe_receive_call(handle_t reader, uint64_t operation,
    uint64_t *head, handle_t *value)
{
  if (!head || !value) {
    return CALL_BAD_REQUEST;
  }
  *head = 0;
  *value = HANDLE_INVALID;
  struct message_header request = {PROTOCOL_PIPE, operation};
  struct pipe_receive_reply response;
  enum call_status status = response_status(syscall_call(reader, &request, sizeof(request),
      &response, sizeof(response)), sizeof(response));
  if (status == CALL_BAD_BUFFER) {
    return CALL_OUTCOME_UNKNOWN;
  }
  if (status != CALL_OK) {
    return status;
  }
  bool valued = response.head == PIPE_HEAD_VALUE;
  if ((valued != (response.value != HANDLE_INVALID)) ||
      (!valued && response.head != PIPE_HEAD_BYTES && response.head != PIPE_HEAD_END)) {
    return CALL_OUTCOME_UNKNOWN;
  }
  *head = response.head;
  *value = response.value;
  return CALL_OK;
}

enum call_status pipe_receive(handle_t reader, uint64_t *head, handle_t *value)
{
  return pipe_receive_call(reader, PIPE_RECEIVE, head, value);
}

enum call_status pipe_try_receive(handle_t reader, uint64_t *head, handle_t *value)
{
  return pipe_receive_call(reader, PIPE_TRY_RECEIVE, head, value);
}
