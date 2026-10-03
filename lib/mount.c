#include <abi/directory.h>
#include <mount.h>
#include <syscall.h>

static enum call_status mount_reply_status(struct syscall_result result,
    const struct mount_reply *reply, handle_t *root)
{
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_OUTCOME_UNKNOWN;
  }
  if (result.status != CALL_OK) {
    return result.reply_size == 0 ? result.status : CALL_OUTCOME_UNKNOWN;
  }
  if (result.reply_size != sizeof(*reply) || reply->root == HANDLE_INVALID) {
    return CALL_OUTCOME_UNKNOWN;
  }
  *root = reply->root;
  return CALL_OK;
}

enum call_status mount_open_root(handle_t mount, uint64_t access, handle_t *root)
{
  if (!root) {
    return CALL_BAD_REQUEST;
  }
  *root = HANDLE_INVALID;
  if (access != MOUNT_ACCESS_READ_ONLY && access != MOUNT_ACCESS_READ_WRITE) {
    return CALL_BAD_REQUEST;
  }
  struct mount_message message = {{PROTOCOL_MOUNT, MOUNT_OPEN_ROOT}, {access}};
  struct mount_reply reply = {0};
  struct syscall_result result = syscall_call(mount, &message, sizeof(message),
      &reply, sizeof(reply));
  return mount_reply_status(result, &reply, root);
}

enum call_status mount_open_volume(handle_t mount, uint64_t partition,
    const char *name, uint64_t rights, handle_t *root)
{
  if (!root) {
    return CALL_BAD_REQUEST;
  }
  *root = HANDLE_INVALID;
  if (!partition || partition > UINT32_MAX || !name ||
      !(rights & DIRECTORY_RIGHT_LOOKUP) ||
      (rights & ~DIRECTORY_RIGHTS)) {
    return CALL_BAD_REQUEST;
  }
  size_t length = 0;
  while (name[length]) {
    if (length == MOUNT_VOLUME_NAME_MAX) {
      return CALL_BAD_REQUEST;
    }
    ++length;
  }
  if (!length) {
    return CALL_BAD_REQUEST;
  }
  struct mount_volume_message message = {
    .header = {PROTOCOL_MOUNT, MOUNT_OPEN_VOLUME},
    .body = {partition, (uintptr_t)name, length, rights},
  };
  struct mount_reply reply = {0};
  struct syscall_result result = syscall_call(mount, &message, sizeof(message),
      &reply, sizeof(reply));
  return mount_reply_status(result, &reply, root);
}

enum call_status mount_sync(handle_t mount)
{
  struct message_header message = {PROTOCOL_MOUNT, MOUNT_SYNC};
  struct syscall_result result = syscall_call(mount, &message, sizeof(message), NULL, 0);
  if (result.status >= CALL_STATUS_COUNT || result.reply_size != 0) {
    return CALL_OUTCOME_UNKNOWN;
  }
  return result.status;
}
