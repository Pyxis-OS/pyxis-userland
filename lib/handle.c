#include <handle.h>
#include <syscall.h>

int handle_close(handle_t handle)
{
  struct syscall_result result = syscall_close(handle);
  return result.status == CALL_OK && result.reply_size == 0 ? 0 : -1;
}

static enum call_status copy(handle_t source, uint64_t rights, uint64_t flags,
                              handle_t *destination)
{
  if (!destination) {
    return CALL_BAD_REQUEST;
  }
  *destination = HANDLE_INVALID;
  struct syscall_result result = syscall_copy(source, rights, flags, destination);
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  if (result.reply_size != (result.status == CALL_OK ? sizeof(*destination) : 0) ||
      (result.status == CALL_OK && *destination == HANDLE_INVALID)) {
    return CALL_BAD_REQUEST;
  }
  return result.status;
}

enum call_status handle_copy(handle_t source, handle_t *destination)
{
  return copy(source, 0, HANDLE_COPY_SAME_RIGHTS, destination);
}

enum call_status handle_copy_restricted(handle_t source, uint64_t rights,
                                         handle_t *destination)
{
  return copy(source, rights, 0, destination);
}
