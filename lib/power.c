#include <power.h>
#include <syscall.h>

static enum call_status power_call(handle_t power, uint64_t operation)
{
  struct message_header request = {PROTOCOL_POWER, operation};
  struct syscall_result result = syscall_call(power, &request, sizeof(request), NULL, 0);
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  if (result.status == CALL_OK || result.reply_size) {
    return CALL_BAD_REQUEST;
  }
  return result.status;
}

enum call_status power_off(handle_t power)
{
  return power_call(power, POWER_OFF);
}

enum call_status power_restart(handle_t power)
{
  return power_call(power, POWER_RESTART);
}
