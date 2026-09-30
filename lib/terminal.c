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
      response.input == response.attachment || response.output == response.attachment) {
    return CALL_BAD_REQUEST;
  }
  *reply = response;
  return CALL_OK;
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
