#include <wait.h>
#include <syscall.h>
#include <string.h>

enum call_status wait_many(const struct wait_interest *interests, size_t count,
    uint64_t deadline_ns, uint64_t *events)
{
  if (!interests || !events || !count || count > WAIT_MAX_INTERESTS) {
    return CALL_BAD_REQUEST;
  }
  uint64_t response[WAIT_MAX_INTERESTS];
  struct syscall_result result = syscall_wait_many(interests, count, deadline_ns, response);
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  size_t size = count * sizeof(*events);
  if (result.reply_size != (result.status == CALL_OK ? size : 0)) {
    return CALL_BAD_REQUEST;
  }
  if (result.status == CALL_OK) {
    memcpy(events, response, size);
  }
  return result.status;
}
