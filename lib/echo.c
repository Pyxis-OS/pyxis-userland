#include <echo.h>
#include <syscall.h>

enum call_status echo_exchange(handle_t echo, uint32_t destination,
    uint64_t deadline_ns, struct echo_reply *reply)
{
  if (!reply) {
    return CALL_BAD_REQUEST;
  }
  *reply = (struct echo_reply){0};
  struct echo_request request = {
    .header = {PROTOCOL_ECHO, ECHO_EXCHANGE},
    .destination = destination,
    .deadline_ns = deadline_ns,
  };
  struct echo_reply response;
  struct syscall_result result = syscall_call(echo, &request, sizeof(request),
      &response, sizeof(response));
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  if (result.reply_size != (result.status == CALL_OK ? sizeof(response) : 0)) {
    return CALL_BAD_REQUEST;
  }
  if (result.status == CALL_OK) {
    if (response.address != destination) {
      return CALL_BAD_REQUEST;
    }
    *reply = response;
  }
  return result.status;
}
