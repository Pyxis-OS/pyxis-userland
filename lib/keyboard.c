#include <keyboard.h>
#include <syscall.h>

static enum call_status keyboard_command(handle_t keyboard, uint64_t operation)
{
  struct message_header message = {PROTOCOL_KEYBOARD, operation};
  struct syscall_result result = syscall_call(keyboard, &message, sizeof(message), NULL, 0);
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  if (result.reply_size) {
    return CALL_BAD_REQUEST;
  }
  return result.status;
}

enum call_status keyboard_acquire(handle_t keyboard)
{
  return keyboard_command(keyboard, KEYBOARD_ACQUIRE);
}

enum call_status keyboard_release(handle_t keyboard)
{
  return keyboard_command(keyboard, KEYBOARD_RELEASE);
}

enum call_status keyboard_read(handle_t keyboard, uint64_t flags,
                              struct keyboard_event *event)
{
  if (!event) {
    return CALL_BAD_REQUEST;
  }
  *event = (struct keyboard_event){0};
  struct keyboard_read_request message = {
    .header = {PROTOCOL_KEYBOARD, KEYBOARD_READ},
    .flags = flags,
  };
  struct keyboard_event reply;
  struct syscall_result result = syscall_call(keyboard, &message, sizeof(message),
      &reply, sizeof(reply));
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  if (result.reply_size != (result.status == CALL_OK ? sizeof(reply) : 0)) {
    return CALL_BAD_REQUEST;
  }
  if (result.status == CALL_OK) {
    *event = reply;
  }
  return result.status;
}
