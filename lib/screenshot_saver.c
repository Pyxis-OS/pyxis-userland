#include <screenshot_saver.h>
#include <syscall.h>
#include <string.h>

static enum call_status reply_status(struct syscall_result result, size_t size)
{
  if (result.status >= CALL_STATUS_COUNT ||
      result.reply_size != (result.status == CALL_OK ? size : 0)) {
    return CALL_BAD_REQUEST;
  }
  return result.status;
}

enum call_status screenshot_saver_next(handle_t saver,
    struct screenshot_saver_pending *pending)
{
  if (!pending) {
    return CALL_BAD_REQUEST;
  }
  *pending = (struct screenshot_saver_pending){0};
  struct message_header message = {PROTOCOL_SCREENSHOT_SAVER, SCREENSHOT_SAVER_NEXT};
  return reply_status(syscall_call(saver, &message, sizeof(message), pending,
      sizeof(*pending)), sizeof(*pending));
}

enum call_status screenshot_saver_capture(handle_t saver, uint64_t request,
    handle_t capture, struct screen_capture_reply *reply)
{
  if (!reply) {
    return CALL_BAD_REQUEST;
  }
  *reply = (struct screen_capture_reply){0};
  struct screenshot_saver_capture_request message = {
    .header = {PROTOCOL_SCREENSHOT_SAVER, SCREENSHOT_SAVER_CAPTURE},
    .request = request, .capture = capture,
  };
  return reply_status(syscall_call(saver, &message, sizeof(message), reply,
      sizeof(*reply)), sizeof(*reply));
}

enum call_status screenshot_saver_complete(handle_t saver, uint64_t request,
    enum call_status status, const char *filename)
{
  if (status >= CALL_STATUS_COUNT || (status == CALL_OK && !filename)) {
    return CALL_BAD_REQUEST;
  }
  struct screenshot_saver_complete_request message = {
    .header = {PROTOCOL_SCREENSHOT_SAVER, SCREENSHOT_SAVER_COMPLETE},
    .request = request, .status = status,
    .filename = (uintptr_t)filename,
    .filename_length = filename ? strlen(filename) : 0,
  };
  return reply_status(syscall_call(saver, &message, sizeof(message), NULL, 0), 0);
}
