#include <pointer.h>
#include <syscall.h>

static enum call_status pointer_command(handle_t pointer, uint64_t operation)
{
  struct message_header message = {PROTOCOL_POINTER, operation};
  struct syscall_result result = syscall_call(pointer, &message, sizeof(message), NULL, 0);
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  if (result.reply_size) {
    return CALL_BAD_REQUEST;
  }
  return result.status;
}

enum call_status pointer_acquire(handle_t pointer)
{
  return pointer_command(pointer, POINTER_ACQUIRE);
}

enum call_status pointer_release(handle_t pointer)
{
  return pointer_command(pointer, POINTER_RELEASE);
}

enum call_status pointer_read(handle_t pointer, uint64_t flags, struct pointer_event *event)
{
  if (!event) {
    return CALL_BAD_REQUEST;
  }
  *event = (struct pointer_event){0};
  struct pointer_read_request message = {
    .header = {PROTOCOL_POINTER, POINTER_READ},
    .flags = flags,
  };
  struct pointer_event reply;
  struct syscall_result result = syscall_call(pointer, &message, sizeof(message),
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
