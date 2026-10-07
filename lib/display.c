#include <display.h>
#include <syscall.h>

static enum call_status display_call(handle_t display, uint64_t operation,
    void *reply, size_t size)
{
  struct message_header message = {PROTOCOL_DISPLAY, operation};
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
  enum call_status status = display_call(display, DISPLAY_ACQUIRE, &reply, sizeof(reply));
  if (status == CALL_OK) {
    *buffer = reply;
  }
  return status;
}

enum call_status display_present(handle_t display)
{
  return display_call(display, DISPLAY_PRESENT, NULL, 0);
}

enum call_status display_release(handle_t display)
{
  return display_call(display, DISPLAY_RELEASE, NULL, 0);
}

enum call_status display_size(handle_t display, struct display_size_reply *size)
{
  if (!size) {
    return CALL_BAD_REQUEST;
  }
  *size = (struct display_size_reply){0};
  struct display_size_reply reply;
  enum call_status status = display_call(display, DISPLAY_SIZE, &reply, sizeof(reply));
  if (status == CALL_OK) {
    if (!reply.width || !reply.height || !reply.generation || reply.reserved) {
      return CALL_BAD_REQUEST;
    }
    *size = reply;
  }
  return status;
}

enum call_status display_replace(handle_t display, uint64_t generation,
    struct display_buffer *buffer)
{
  if (!buffer || !generation) {
    return CALL_BAD_REQUEST;
  }
  struct display_replace_request request = {
    .header = {PROTOCOL_DISPLAY, DISPLAY_REPLACE},
    .generation = generation,
  };
  struct display_buffer reply;
  struct syscall_result result = syscall_call(display, &request, sizeof(request),
      &reply, sizeof(reply));
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  if (result.reply_size != (result.status == CALL_OK ? sizeof(reply) : 0)) {
    return CALL_BAD_REQUEST;
  }
  if (result.status == CALL_OK) {
    *buffer = reply;
  }
  return result.status;
}
