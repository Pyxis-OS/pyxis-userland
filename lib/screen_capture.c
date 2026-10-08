#include <screen_capture.h>
#include <syscall.h>

enum call_status screen_capture_frame(handle_t capture, struct screen_capture_reply *reply)
{
  if (!reply) {
    return CALL_BAD_REQUEST;
  }
  *reply = (struct screen_capture_reply){0};
  struct message_header request = {PROTOCOL_SCREEN_CAPTURE, SCREEN_CAPTURE_FRAME};
  struct screen_capture_reply response;
  struct syscall_result result = syscall_call(capture, &request, sizeof(request),
      &response, sizeof(response));
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  if (result.reply_size != (result.status == CALL_OK ? sizeof(response) : 0)) {
    return CALL_BAD_REQUEST;
  }
  if (result.status == CALL_OK) {
    *reply = response;
  }
  return result.status;
}
