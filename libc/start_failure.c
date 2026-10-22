#include <syscall.h>
#include "runtime.h"

[[noreturn]] void libc_start_failed(unsigned step, enum call_status status)
{
  uint32_t code = status == CALL_UNKNOWN_SYSTEM_CALL ?
      PROCESS_START_UNKNOWN_CALL : (uint32_t)status;
  uint32_t reason = step << PROCESS_START_STEP_SHIFT | code;
  syscall1(SYSCALL_START_FAILED, reason);
  /* A returning terminal syscall violates the ABI. Do not fall through or
   * invoke stdio/errno on an incompletely initialized runtime. */
  __builtin_trap();
}

void libc_startup_close(handle_t handle, unsigned step)
{
  struct syscall_result result = syscall_close(handle);
  if (result.status == (uint64_t)CALL_UNKNOWN_SYSTEM_CALL) {
    libc_start_failed(step, CALL_UNKNOWN_SYSTEM_CALL);
  }
  if (result.status >= CALL_STATUS_COUNT || result.reply_size) {
    libc_start_failed(step, CALL_OUTCOME_UNKNOWN);
  }
  if (result.status != CALL_OK) {
    libc_start_failed(step, result.status);
  }
}
