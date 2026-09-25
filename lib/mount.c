#include <mount.h>
#include <syscall.h>

enum call_status mount_open_root(handle_t mount, uint64_t access, handle_t *root)
{
  if (!root) {
    return CALL_BAD_REQUEST;
  }
  *root = HANDLE_INVALID;
  struct mount_message message = {{PROTOCOL_MOUNT, MOUNT_OPEN_ROOT}, {access}};
  struct mount_reply reply;
  struct syscall_result result = syscall_call(mount, &message, sizeof(message),
      &reply, sizeof(reply));
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  if (result.reply_size != (result.status == CALL_OK ? sizeof(reply) : 0)) {
    return CALL_BAD_REQUEST;
  }
  if (result.status == CALL_OK) {
    *root = reply.root;
  }
  return result.status;
}
