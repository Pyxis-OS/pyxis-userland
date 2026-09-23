#include <display.h>
#include <syscall.h>

static enum call_status display_call(handle_t display, uint64_t operation,
    struct display_buffer *reply)
{
  struct message_header message = {PROTOCOL_DISPLAY, operation};
  size_t size = reply ? sizeof(*reply) : 0;
  struct syscall_result result = syscall_call(display, &message, sizeof(message),
      reply, size);
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  if (result.reply_size != (result.status == CALL_OK ? size : 0)) {
    return CALL_BAD_REQUEST;
  }
  return result.status;
}

enum call_status display_acquire(handle_t display, struct display_buffer *buffer)
{
  if (!buffer) {
    return CALL_BAD_REQUEST;
  }
  *buffer = (struct display_buffer){0};
  struct display_buffer reply;
  enum call_status status = display_call(display, DISPLAY_ACQUIRE, &reply);
  if (status == CALL_OK) {
    *buffer = reply;
  }
  return status;
}

enum call_status display_present(handle_t display)
{
  return display_call(display, DISPLAY_PRESENT, NULL);
}

enum call_status display_release(handle_t display)
{
  return display_call(display, DISPLAY_RELEASE, NULL);
}
