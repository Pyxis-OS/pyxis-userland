#include <terminal.h>
#include <abi/console.h>
#include <stdbool.h>
#include <string.h>
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

enum call_status terminal_create(handle_t service, size_t columns, size_t rows,
    struct terminal_create_reply *reply)
{
  if (!reply) {
    return CALL_BAD_REQUEST;
  }
  struct terminal_create_request request = {
    .header = {PROTOCOL_TERMINAL_SERVICE, TERMINAL_CREATE},
    .columns = columns, .rows = rows,
  };
  struct terminal_create_reply response;
  enum call_status status = response_status(syscall_call(service, &request, sizeof(request),
      &response, sizeof(response)), sizeof(response));
  if (status != CALL_OK) {
    return status;
  }
  if (response.input == HANDLE_INVALID || response.output == HANDLE_INVALID ||
      response.attachment == HANDLE_INVALID || response.input == response.output ||
      response.input == response.attachment || response.output == response.attachment ||
      response.events == HANDLE_INVALID || response.events == response.input ||
      response.events == response.output || response.events == response.attachment) {
    return CALL_BAD_REQUEST;
  }
  *reply = response;
  return CALL_OK;
}

enum call_status terminal_resize(handle_t attachment, size_t columns, size_t rows)
{
  struct terminal_resize_request request = {
    .header = {PROTOCOL_TERMINAL_ATTACHMENT, TERMINAL_RESIZE},
    .columns = columns, .rows = rows,
  };
  return response_status(syscall_call(attachment, &request, sizeof(request), NULL, 0), 0);
}

enum call_status terminal_try_inject(handle_t attachment, const void *data, size_t length,
    struct terminal_transfer_reply *reply)
{
  if (!reply) {
    return CALL_BAD_REQUEST;
  }
  size_t limit = length < TERMINAL_TRANSFER_MAX ? length : TERMINAL_TRANSFER_MAX;
  uintptr_t buffer = (uintptr_t)data, output = (uintptr_t)reply;
  if (limit && (buffer <= output ? output - buffer < limit :
      buffer - output < sizeof(*reply))) {
    return CALL_BAD_REQUEST;
  }
  struct terminal_transfer_request request = {
    .header = {PROTOCOL_TERMINAL_ATTACHMENT, TERMINAL_TRY_INJECT},
    .buffer = buffer, .length = limit,
  };
  struct terminal_transfer_reply response;
  enum call_status status = response_status(syscall_call(attachment, &request, sizeof(request),
      &response, sizeof(response)), sizeof(response));
  if (status != CALL_OK) {
    return status;
  }
  if (response.length > limit || (limit && !response.length)) {
    return CALL_BAD_REQUEST;
  }
  *reply = response;
  return CALL_OK;
}

static bool valid_completion(uint64_t kind, int64_t status)
{
  switch (kind) {
  case TERMINAL_COMPLETION_EXITED:
    return status >= INT32_MIN && status <= INT32_MAX;
  case TERMINAL_COMPLETION_BUILTIN:
    return status == 0 || status == 1;
  case TERMINAL_COMPLETION_FAULTED:
  case TERMINAL_COMPLETION_TERMINATED:
  case TERMINAL_COMPLETION_LAUNCH_FAILED:
  case TERMINAL_COMPLETION_REJECTED:
  case TERMINAL_COMPLETION_LAUNCHED:
    return status == 0;
  default:
    return false;
  }
}

static bool valid_record(const void *data, size_t length)
{
  struct terminal_record record;
  if (length < sizeof(record)) {
    return false;
  }
  memcpy(&record, data, sizeof(record));
  if (record.length != length - sizeof(record)) {
    return false;
  }
  switch (record.type) {
  case TERMINAL_RECORD_DATA:
    return record.length && record.length <= TERMINAL_TRANSFER_MAX;
  case TERMINAL_RECORD_FRESH_LINE:
    return !record.length;
  case TERMINAL_RECORD_COMMAND_COMPLETE: {
    if (record.length != sizeof(struct terminal_command_complete)) {
      return false;
    }
    struct terminal_command_complete completion;
    memcpy(&completion, (const char *)data + sizeof(record), sizeof(completion));
    return completion.command && valid_completion(completion.kind, completion.status);
  }
  case TERMINAL_RECORD_TAB_WIDTH: {
    if (record.length != sizeof(uint64_t)) {
      return false;
    }
    uint64_t columns;
    memcpy(&columns, (const char *)data + sizeof(record), sizeof(columns));
    return columns >= CONSOLE_TAB_WIDTH_MIN && columns <= CONSOLE_TAB_WIDTH_MAX;
  }
  default:
    return false;
  }
}

enum call_status terminal_try_drain(handle_t attachment, void *data, size_t capacity,
    struct terminal_transfer_reply *reply)
{
  if (!reply || !data) {
    return CALL_BAD_REQUEST;
  }
  size_t limit = capacity < TERMINAL_RECORD_MAX ? capacity : TERMINAL_RECORD_MAX;
  uintptr_t buffer = (uintptr_t)data, output = (uintptr_t)reply;
  if (limit && (buffer <= output ? output - buffer < limit :
      buffer - output < sizeof(*reply))) {
    return CALL_BAD_REQUEST;
  }
  /* Stage the record so invalid replies also preserve the caller's buffer. */
  unsigned char record[TERMINAL_RECORD_MAX];
  struct terminal_transfer_request request = {
    .header = {PROTOCOL_TERMINAL_ATTACHMENT, TERMINAL_TRY_DRAIN},
    .buffer = (uintptr_t)record, .length = limit,
  };
  struct terminal_transfer_reply response;
  enum call_status status = response_status(syscall_call(attachment, &request, sizeof(request),
      &response, sizeof(response)), sizeof(response));
  if (status != CALL_OK) {
    return status;
  }
  if (response.length > limit ||
      (response.length && !valid_record(record, response.length))) {
    return CALL_BAD_REQUEST;
  }
  memcpy(data, record, response.length);
  *reply = response;
  return CALL_OK;
}

enum call_status terminal_end_input(handle_t attachment)
{
  struct message_header request = {PROTOCOL_TERMINAL_ATTACHMENT, TERMINAL_END_INPUT};
  return response_status(syscall_call(attachment, &request, sizeof(request), NULL, 0), 0);
}

enum call_status terminal_hangup(handle_t attachment)
{
  struct message_header request = {PROTOCOL_TERMINAL_ATTACHMENT, TERMINAL_HANGUP};
  return response_status(syscall_call(attachment, &request, sizeof(request), NULL, 0), 0);
}

enum call_status terminal_command_complete(handle_t events, uint64_t kind, int64_t status)
{
  if (!valid_completion(kind, status)) {
    return CALL_BAD_REQUEST;
  }
  struct terminal_command_complete_request request = {
    .header = {PROTOCOL_TERMINAL_EVENTS, TERMINAL_COMMAND_COMPLETE},
    .kind = kind, .status = status,
  };
  struct syscall_result result = syscall_call(events, &request, sizeof(request), NULL, 0);
  if (result.status >= CALL_STATUS_COUNT || result.reply_size) {
    return CALL_OUTCOME_UNKNOWN;
  }
  return result.status;
}
