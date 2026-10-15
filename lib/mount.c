#include <abi/directory.h>
#include <mount.h>
#include <syscall.h>

#define FAT32_DEVICE_BLOCK_SIZE_MIN UINT64_C(512)
#define FAT32_DEVICE_BLOCK_SIZE_MAX UINT64_C(4096)

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

enum call_status mount_check(handle_t control)
{
  struct message_header message = {PROTOCOL_MOUNT, MOUNT_CHECK};
  struct syscall_result result = syscall_call(control, &message, sizeof(message), NULL, 0);
  if (result.status >= CALL_STATUS_COUNT || result.reply_size != 0) {
    return CALL_OUTCOME_UNKNOWN;
  }
  return result.status;
}

static bool fat32_rights(uint64_t rights)
{
  return !(rights & ~(DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_ENUMERATE |
      DIRECTORY_RIGHT_READ_FILES));
}

enum call_status mount_open_fat32(handle_t issuer, uint64_t rights,
    struct mount_fat32_reply *mount)
{
  if (!mount) {
    return CALL_BAD_REQUEST;
  }
  *mount = (struct mount_fat32_reply){HANDLE_INVALID, HANDLE_INVALID};
  if (!fat32_rights(rights)) {
    return CALL_BAD_REQUEST;
  }
  struct {
    struct message_header header;
    struct mount_fat32_root_request body;
  } message = {{PROTOCOL_MOUNT, MOUNT_OPEN_FAT32}, {rights}};
  struct mount_fat32_reply reply = {HANDLE_INVALID, HANDLE_INVALID};
  struct syscall_result result = syscall_call(issuer, &message, sizeof(message),
      &reply, sizeof(reply));
  if (result.status >= CALL_STATUS_COUNT ||
      result.reply_size != (result.status == CALL_OK ? sizeof(reply) : 0)) {
    return CALL_OUTCOME_UNKNOWN;
  }
  if (result.status != CALL_OK) {
    return result.status;
  }
  if (reply.root == HANDLE_INVALID || reply.control == HANDLE_INVALID ||
      reply.root == reply.control) {
    return CALL_OUTCOME_UNKNOWN;
  }
  *mount = reply;
  return CALL_OK;
}

enum call_status mount_reopen_fat32(handle_t control, uint64_t rights, handle_t *root)
{
  if (!root) {
    return CALL_BAD_REQUEST;
  }
  *root = HANDLE_INVALID;
  if (rights & ~DIRECTORY_CONTENT_RIGHTS) {
    return CALL_BAD_REQUEST;
  }
  struct {
    struct message_header header;
    struct mount_fat32_root_request body;
  } message = {{PROTOCOL_MOUNT, MOUNT_OPEN_ROOT}, {rights}};
  struct mount_reply reply = {HANDLE_INVALID};
  return mount_reply_status(syscall_call(control, &message, sizeof(message),
      &reply, sizeof(reply)), &reply, root);
}

static bool guid_nonzero(const uint8_t guid[16])
{
  for (size_t i = 0; i < 16; ++i) {
    if (guid[i]) {
      return true;
    }
  }
  return false;
}

static bool mount_identity_valid(const struct mount_state_reply *reply)
{
  return reply->domain && reply->device && reply->partition &&
      reply->partition <= UINT32_MAX &&
      (reply->device_block_size == FAT32_DEVICE_BLOCK_SIZE_MIN ||
       reply->device_block_size == FAT32_DEVICE_BLOCK_SIZE_MAX) &&
      reply->first_block && reply->block_count &&
      reply->first_block < reply->device_block_count &&
      reply->block_count <= reply->device_block_count - reply->first_block &&
      guid_nonzero(reply->disk_guid) && guid_nonzero(reply->partition_guid) &&
      guid_nonzero(reply->partition_type);
}

enum call_status mount_observe(handle_t control, struct mount_state_reply *state)
{
  if (!state) {
    return CALL_BAD_REQUEST;
  }
  struct message_header message = {PROTOCOL_MOUNT, MOUNT_OBSERVE};
  struct mount_state_reply reply = {0};
  struct syscall_result result = syscall_call(control, &message, sizeof(message),
      &reply, sizeof(reply));
  if (result.status >= CALL_STATUS_COUNT ||
      result.reply_size != (result.status == CALL_OK ? sizeof(reply) : 0)) {
    return CALL_OUTCOME_UNKNOWN;
  }
  if (result.status != CALL_OK) {
    return result.status;
  }
  if ((reply.flags & MOUNT_STATE_IDENTITY_VALID) && !mount_identity_valid(&reply)) {
    return CALL_OUTCOME_UNKNOWN;
  }
  *state = reply;
  return CALL_OK;
}

enum call_status mount_retire(handle_t control)
{
  struct message_header message = {PROTOCOL_MOUNT, MOUNT_RETIRE};
  struct syscall_result result = syscall_call(control, &message, sizeof(message), NULL, 0);
  if (result.status >= CALL_STATUS_COUNT || result.reply_size != 0) {
    return CALL_OUTCOME_UNKNOWN;
  }
  return result.status;
}

enum call_status mount_begin(handle_t control, struct mount_interval_reply *interval)
{
  if (!interval) {
    return CALL_BAD_REQUEST;
  }
  struct message_header message = {PROTOCOL_MOUNT, MOUNT_BEGIN};
  struct mount_interval_reply reply = {0};
  struct syscall_result result = syscall_call(control, &message, sizeof(message),
      &reply, sizeof(reply));
  if (result.status >= CALL_STATUS_COUNT ||
      result.reply_size != (result.status == CALL_OK ? sizeof(reply) : 0)) {
    return CALL_OUTCOME_UNKNOWN;
  }
  if (result.status != CALL_OK) {
    return result.status;
  }
  if (!reply.id || !reply.deadline_ns) {
    return CALL_OUTCOME_UNKNOWN;
  }
  *interval = reply;
  return CALL_OK;
}

enum call_status mount_end(handle_t control, uint64_t id)
{
  if (!id) {
    return CALL_BAD_REQUEST;
  }
  struct {
    struct message_header header;
    struct mount_interval_request body;
  } message = {{PROTOCOL_MOUNT, MOUNT_END}, {id}};
  struct syscall_result result = syscall_call(control, &message, sizeof(message), NULL, 0);
  if (result.status >= CALL_STATUS_COUNT || result.reply_size != 0) {
    return CALL_OUTCOME_UNKNOWN;
  }
  return result.status;
}
