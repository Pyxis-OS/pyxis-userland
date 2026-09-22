#include <launcher.h>
#include <syscall.h>

enum call_status launcher_launch(handle_t launcher, const struct launch_request *request,
                                  handle_t *child)
{
  if (!child || !request) {
    if (child) {
      *child = HANDLE_INVALID;
    }
    return CALL_BAD_REQUEST;
  }
  struct launch_message message = {
    .header = {PROTOCOL_LAUNCHER, LAUNCHER_LAUNCH},
    .body = *request,
  };
  *child = HANDLE_INVALID;
  handle_t reply;
  struct syscall_result result = syscall_call(launcher, &message, sizeof(message),
      &reply, sizeof(reply));
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  if (result.reply_size != (result.status == CALL_OK ? sizeof(reply) : 0)) {
    return CALL_BAD_REQUEST;
  }
  if (result.status != CALL_OK) {
    return result.status;
  }
  if (reply == HANDLE_INVALID) {
    return CALL_BAD_REQUEST;
  }
  *child = reply;
  return CALL_OK;
}
