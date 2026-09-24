#include <clock.h>
#include <syscall.h>

enum call_status clock_now(handle_t clock, uint64_t *nanoseconds)
{
  if (!nanoseconds) {
    return CALL_BAD_REQUEST;
  }
  *nanoseconds = 0;
  struct message_header message = {PROTOCOL_CLOCK, CLOCK_NOW};
  struct clock_reading reply;
  struct syscall_result result = syscall_call(clock, &message, sizeof(message),
      &reply, sizeof(reply));
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  if (result.reply_size != (result.status == CALL_OK ? sizeof(reply) : 0)) {
    return CALL_BAD_REQUEST;
  }
  if (result.status == CALL_OK) {
    *nanoseconds = reply.nanoseconds;
  }
  return result.status;
}

enum call_status clock_sleep_until(handle_t clock, uint64_t deadline_ns)
{
  struct clock_sleep_request message = {
    .header = {PROTOCOL_CLOCK, CLOCK_SLEEP_UNTIL},
    .deadline_ns = deadline_ns,
  };
  struct syscall_result result = syscall_call(clock, &message, sizeof(message), NULL, 0);
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  if (result.reply_size) {
    return CALL_BAD_REQUEST;
  }
  return result.status;
}

enum call_status clock_sleep_for(handle_t clock, uint64_t duration_ns)
{
  uint64_t now;
  enum call_status status = clock_now(clock, &now);
  if (status != CALL_OK) {
    return status;
  }
  if (duration_ns > UINT64_MAX - now) {
    return CALL_LIMIT;
  }
  return clock_sleep_until(clock, now + duration_ns);
}
