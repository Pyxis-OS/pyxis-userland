#include <log.h>
#include <string.h>
#include <syscall.h>

static bool cursor_after(struct log_cursor left, struct log_cursor right)
{
  return left.line > right.line ||
      (left.line == right.line && left.offset > right.offset);
}

enum call_status log_get_snapshot(handle_t log, struct log_snapshot *snapshot)
{
  if (!snapshot) {
    return CALL_BAD_REQUEST;
  }
  struct message_header message = {PROTOCOL_LOG, LOG_SNAPSHOT};
  struct log_snapshot reply;
  struct syscall_result result = syscall_call(log, &message, sizeof(message),
      &reply, sizeof(reply));
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  if (result.reply_size != (result.status == CALL_OK ? sizeof(reply) : 0)) {
    return CALL_BAD_REQUEST;
  }
  if (result.status == CALL_OK) {
    if (cursor_after(reply.first, reply.end) || reply.first.offset != 0 ||
        !reply.capacity_bytes || reply.end.offset > reply.capacity_bytes ||
        reply.dropped_lines > reply.first.line) {
      return CALL_BAD_REQUEST;
    }
    *snapshot = reply;
  }
  return result.status;
}

enum call_status log_read(handle_t log, struct log_cursor cursor,
    struct log_cursor end, void *bytes, size_t capacity, struct log_read_reply *reply)
{
  if (!bytes || !reply || !capacity || capacity > LOG_READ_MAX ||
      cursor_after(cursor, end)) {
    return CALL_BAD_REQUEST;
  }
  struct log_read_request request = {
    .header = {PROTOCOL_LOG, LOG_READ}, .cursor = cursor, .end = end,
  };
  struct {
    struct log_read_reply header;
    unsigned char bytes[LOG_READ_MAX];
  } response;
  struct syscall_result result = syscall_call(log, &request, sizeof(request),
      &response, sizeof(response.header) + capacity);
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  if (result.status != CALL_OK) {
    return result.reply_size ? CALL_BAD_REQUEST : result.status;
  }
  if (result.reply_size < sizeof(response.header) ||
      result.reply_size > sizeof(response.header) + capacity ||
      response.header.size != result.reply_size - sizeof(response.header) ||
      cursor_after(cursor, response.header.next) ||
      cursor_after(response.header.next, end)) {
    return CALL_BAD_REQUEST;
  }
  uint64_t advanced_lines = response.header.next.line - cursor.line;
  /* An overwritten snapshot ending mid-line also loses its final partial line. */
  if (response.header.dropped_lines > advanced_lines &&
      (response.header.size || !response.header.next.offset ||
       advanced_lines == UINT64_MAX || response.header.dropped_lines != advanced_lines + 1)) {
    return CALL_BAD_REQUEST;
  }
  if (response.header.size) {
    struct log_cursor expected = cursor;
    if (response.header.dropped_lines) {
      expected.line += response.header.dropped_lines;
      expected.offset = 0;
    }
    for (size_t i = 0; i < response.header.size; ++i) {
      if (response.bytes[i] == '\n') {
        if (expected.line == UINT64_MAX) {
          return CALL_BAD_REQUEST;
        }
        ++expected.line;
        expected.offset = 0;
      } else {
        if (expected.offset == UINT64_MAX) {
          return CALL_BAD_REQUEST;
        }
        ++expected.offset;
      }
    }
    if (expected.line != response.header.next.line ||
        expected.offset != response.header.next.offset) {
      return CALL_BAD_REQUEST;
    }
  } else if (!response.header.dropped_lines &&
      (cursor.line != response.header.next.line ||
       cursor.offset != response.header.next.offset)) {
    return CALL_BAD_REQUEST;
  }
  memcpy(bytes, response.bytes, response.header.size);
  *reply = response.header;
  return CALL_OK;
}
