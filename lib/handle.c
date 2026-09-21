#include <handle.h>
#include <syscall.h>

int handle_close(handle_t handle)
{
  struct syscall_result result = syscall_close(handle);
  return result.status == CALL_OK && result.reply_size == 0 ? 0 : -1;
}
