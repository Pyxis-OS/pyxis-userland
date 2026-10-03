#include <abi/directory.h>
#include <disk.h>
#include <syscall.h>

static enum call_status reply_status(struct syscall_result result, size_t expected)
{
  if (result.status >= CALL_STATUS_COUNT ||
      result.reply_size != (result.status == CALL_OK ? expected : 0)) {
    return CALL_OUTCOME_UNKNOWN;
  }
  return result.status;
}

static enum call_status info_reply(struct syscall_result result,
    const struct disk_info *reply, struct disk_info *info)
{
  enum call_status status = reply_status(result, sizeof(*reply));
  if (status != CALL_OK) {
    return status;
  }
  uint64_t known_flags = DISK_FLAG_WRITABLE | DISK_FLAG_FLUSH_SUPPORTED |
      DISK_FLAG_WRITE_FAILED | DISK_FLAG_MOUNTED | DISK_FLAG_CLAIMED;
  if (!reply->id || reply->preparation > DISK_SETUP_FAILED ||
      (reply->flags & ~known_flags) || reply->gpt_status > DISK_GPT_TIMED_OUT) {
    return CALL_OUTCOME_UNKNOWN;
  }
  *info = *reply;
  return CALL_OK;
}

enum call_status disks_enumerate(handle_t disks, uint64_t index, struct disk_info *info)
{
  if (!info) {
    return CALL_BAD_REQUEST;
  }
  *info = (struct disk_info){0};
  struct {
    struct message_header header;
    struct disks_enumerate_request body;
  } message = {{PROTOCOL_DISKS, DISKS_ENUMERATE}, {index}};
  struct disk_info reply = {0};
  struct syscall_result result = syscall_call(disks, &message, sizeof(message),
      &reply, sizeof(reply));
  return info_reply(result, &reply, info);
}

enum call_status disk_get_info(handle_t disk, struct disk_info *info)
{
  if (!info) {
    return CALL_BAD_REQUEST;
  }
  *info = (struct disk_info){0};
  struct message_header message = {PROTOCOL_DISK, DISK_INFO};
  struct disk_info reply = {0};
  struct syscall_result result = syscall_call(disk, &message, sizeof(message),
      &reply, sizeof(reply));
  return info_reply(result, &reply, info);
}

enum call_status disks_open(handle_t disks, uint64_t id, uint64_t access, handle_t *disk)
{
  if (!disk) {
    return CALL_BAD_REQUEST;
  }
  *disk = HANDLE_INVALID;
  if (!id || (access != DISK_ACCESS_READ_ONLY && access != DISK_ACCESS_READ_WRITE)) {
    return CALL_BAD_REQUEST;
  }
  struct {
    struct message_header header;
    struct disks_open_request body;
  } message = {{PROTOCOL_DISKS, DISKS_OPEN}, {id, access}};
  struct disk_open_reply reply = {0};
  struct syscall_result result = syscall_call(disks, &message, sizeof(message),
      &reply, sizeof(reply));
  enum call_status status = reply_status(result, sizeof(reply));
  if (status != CALL_OK) {
    return status;
  }
  if (reply.disk == HANDLE_INVALID) {
    return CALL_OUTCOME_UNKNOWN;
  }
  *disk = reply.disk;
  return CALL_OK;
}

enum call_status disk_read(handle_t disk, uint64_t offset, void *bytes, size_t length)
{
  if (!bytes || !length || length > DISK_IO_MAX_BYTES || length > UINT64_MAX - offset) {
    return CALL_BAD_REQUEST;
  }
  struct {
    struct message_header header;
    struct disk_read_request body;
  } message = {{PROTOCOL_DISK, DISK_READ}, {offset, length}};
  struct syscall_result result = syscall_call(disk, &message, sizeof(message), bytes, length);
  return reply_status(result, length);
}

enum call_status disk_write(handle_t disk, uint64_t offset, const void *bytes, size_t length)
{
  if (!bytes || !length || length > DISK_IO_MAX_BYTES || length > UINT64_MAX - offset) {
    return CALL_BAD_REQUEST;
  }
  struct {
    struct message_header header;
    struct disk_write_request body;
  } message = {{PROTOCOL_DISK, DISK_WRITE}, {offset, length, (uintptr_t)bytes}};
  struct syscall_result result = syscall_call(disk, &message, sizeof(message), NULL, 0);
  return reply_status(result, 0);
}

enum call_status disk_flush(handle_t disk)
{
  struct message_header message = {PROTOCOL_DISK, DISK_FLUSH};
  return reply_status(syscall_call(disk, &message, sizeof(message), NULL, 0), 0);
}

enum call_status disk_release(handle_t disk)
{
  struct message_header message = {PROTOCOL_DISK, DISK_RELEASE};
  return reply_status(syscall_call(disk, &message, sizeof(message), NULL, 0), 0);
}

enum call_status disk_open_volume(handle_t disk, uint64_t partition,
    const char *name, uint64_t rights, handle_t *root)
{
  if (!root) {
    return CALL_BAD_REQUEST;
  }
  *root = HANDLE_INVALID;
  uint64_t read_rights = DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_ENUMERATE |
      DIRECTORY_RIGHT_READ_FILES | DIRECTORY_RIGHT_FILESYSTEM_INFO;
  if (!partition || partition > UINT32_MAX || !name ||
      !(rights & DIRECTORY_RIGHT_LOOKUP) || (rights & ~read_rights)) {
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
  struct {
    struct message_header header;
    struct mount_volume_request body;
  } message = {{PROTOCOL_DISK, DISK_OPEN_VOLUME}, {partition, (uintptr_t)name, length, rights}};
  struct mount_reply reply = {0};
  struct syscall_result result = syscall_call(disk, &message, sizeof(message), &reply, sizeof(reply));
  enum call_status status = reply_status(result, sizeof(reply));
  if (status != CALL_OK) {
    return status;
  }
  if (reply.root == HANDLE_INVALID) {
    return CALL_OUTCOME_UNKNOWN;
  }
  *root = reply.root;
  return CALL_OK;
}
