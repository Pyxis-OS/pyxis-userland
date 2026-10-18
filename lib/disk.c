#include <abi/directory.h>
#include <disk.h>
#include <string.h>
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

static bool guid_nonzero(const uint8_t guid[16])
{
  for (size_t i = 0; i < 16; ++i) {
    if (guid[i]) {
      return true;
    }
  }
  return false;
}

static bool selection_valid(const struct disk_partition_selection *selection)
{
  return selection && selection->id && selection->partition &&
      selection->partition <= UINT32_MAX && selection->first_block &&
      selection->block_count && selection->device_block_size &&
      selection->device_block_count &&
      selection->device_block_count <= UINT64_MAX / selection->device_block_size &&
      selection->first_block < selection->device_block_count &&
      selection->block_count <= selection->device_block_count - selection->first_block &&
      guid_nonzero(selection->disk_guid) && guid_nonzero(selection->partition_guid) &&
      guid_nonzero(selection->partition_type);
}

static enum call_status disks_review(handle_t disks, uint64_t operation,
    const struct disk_info *info, handle_t *review)
{
  if (!review) {
    return CALL_BAD_REQUEST;
  }
  *review = HANDLE_INVALID;
  if (!info || !info->id || !info->block_size || !info->block_count ||
      info->block_count > UINT64_MAX / info->block_size) {
    return CALL_BAD_REQUEST;
  }
  struct {
    struct message_header header;
    struct disks_review_request body;
  } message = {{PROTOCOL_DISKS, operation},
      {info->id, info->block_size, info->block_count, {0}}};
  memcpy(message.body.disk_guid, info->gpt_guid, sizeof(message.body.disk_guid));
  struct disk_open_reply reply = {HANDLE_INVALID};
  enum call_status status = reply_status(syscall_call(disks, &message, sizeof(message),
      &reply, sizeof(reply)), sizeof(reply));
  if (status != CALL_OK) {
    return status;
  }
  if (reply.disk == HANDLE_INVALID) {
    return CALL_OUTCOME_UNKNOWN;
  }
  *review = reply.disk;
  return CALL_OK;
}

enum call_status disks_review_erase(handle_t disks, const struct disk_info *info,
    handle_t *review)
{
  return disks_review(disks, DISKS_REVIEW_ERASE, info, review);
}

enum call_status disks_review_append(handle_t disks, const struct disk_info *info,
    handle_t *review)
{
  return disks_review(disks, DISKS_REVIEW_APPEND, info, review);
}

enum call_status disk_begin_erase(handle_t review, const char *confirmation,
    handle_t *writer)
{
  if (!writer) {
    return CALL_BAD_REQUEST;
  }
  *writer = HANDLE_INVALID;
  size_t length = sizeof(DISK_ERASE_CONFIRMATION) - 1;
  if (!confirmation || strnlen(confirmation, length + 1) != length ||
      memcmp(confirmation, DISK_ERASE_CONFIRMATION, length)) {
    return CALL_BAD_REQUEST;
  }
  struct {
    struct message_header header;
    struct disk_begin_erase_request body;
  } message = {{PROTOCOL_DISK, DISK_BEGIN_ERASE}, {(uintptr_t)confirmation, length}};
  struct disk_open_reply reply = {HANDLE_INVALID};
  enum call_status status = reply_status(syscall_call(review, &message, sizeof(message),
      &reply, sizeof(reply)), sizeof(reply));
  if (status != CALL_OK) {
    return status;
  }
  if (reply.disk == HANDLE_INVALID) {
    return CALL_OUTCOME_UNKNOWN;
  }
  *writer = reply.disk;
  return CALL_OK;
}

static enum call_status disks_open_selection(handle_t disks, uint64_t operation,
    const struct disk_partition_selection *selection, handle_t *disk)
{
  if (!disk) {
    return CALL_BAD_REQUEST;
  }
  *disk = HANDLE_INVALID;
  if (!selection_valid(selection)) {
    return CALL_BAD_REQUEST;
  }
  struct {
    struct message_header header;
    struct disk_partition_selection body;
  } message = {{PROTOCOL_DISKS, operation}, *selection};
  struct disk_open_reply reply = {HANDLE_INVALID};
  enum call_status status = reply_status(syscall_call(disks, &message, sizeof(message),
      &reply, sizeof(reply)), sizeof(reply));
  if (status != CALL_OK) {
    return status;
  }
  if (reply.disk == HANDLE_INVALID) {
    return CALL_OUTCOME_UNKNOWN;
  }
  *disk = reply.disk;
  return CALL_OK;
}

enum call_status disks_open_pool(handle_t disks,
    const struct disk_partition_selection *selection, handle_t *disk)
{
  return disks_open_selection(disks, DISKS_OPEN_POOL, selection, disk);
}

enum call_status disks_open_partition(handle_t disks,
    const struct disk_partition_selection *selection, handle_t *disk)
{
  return disks_open_selection(disks, DISKS_OPEN_PARTITION, selection, disk);
}

enum call_status disks_review_esp(handle_t disks,
    const struct disk_partition_selection *selection, handle_t *review)
{
  return disks_open_selection(disks, DISKS_REVIEW_ESP, selection, review);
}

enum call_status disk_begin_esp_rebuild(handle_t review, const char *confirmation,
    handle_t *writer)
{
  if (!writer) {
    return CALL_BAD_REQUEST;
  }
  *writer = HANDLE_INVALID;
  size_t length = sizeof(DISK_ESP_REBUILD_CONFIRMATION) - 1;
  if (!confirmation || strnlen(confirmation, length + 1) != length ||
      memcmp(confirmation, DISK_ESP_REBUILD_CONFIRMATION, length)) {
    return CALL_BAD_REQUEST;
  }
  struct {
    struct message_header header;
    struct disk_begin_esp_rebuild_request body;
  } message = {{PROTOCOL_DISK, DISK_BEGIN_ESP_REBUILD}, {(uintptr_t)confirmation, length}};
  struct disk_open_reply reply = {HANDLE_INVALID};
  enum call_status status = reply_status(syscall_call(review, &message, sizeof(message),
      &reply, sizeof(reply)), sizeof(reply));
  if (status != CALL_OK) {
    return status;
  }
  if (reply.disk == HANDLE_INVALID) {
    return CALL_OUTCOME_UNKNOWN;
  }
  *writer = reply.disk;
  return CALL_OK;
}

enum call_status disk_partition_info(handle_t disk, uint64_t entry,
    struct disk_partition_selection *selection)
{
  if (!selection) {
    return CALL_BAD_REQUEST;
  }
  *selection = (struct disk_partition_selection){0};
  if (!entry || entry > UINT32_MAX) {
    return CALL_BAD_REQUEST;
  }
  struct {
    struct message_header header;
    struct disk_partition_info_request body;
  } message = {{PROTOCOL_DISK, DISK_PARTITION_INFO}, {entry}};
  struct disk_partition_selection reply = {0};
  enum call_status status = reply_status(syscall_call(disk, &message, sizeof(message),
      &reply, sizeof(reply)), sizeof(reply));
  if (status != CALL_OK) {
    return status;
  }
  if (!selection_valid(&reply) || reply.partition != entry) {
    return CALL_OUTCOME_UNKNOWN;
  }
  *selection = reply;
  return CALL_OK;
}

enum call_status disk_append_pool(handle_t review,
    const struct disk_partition_selection *selection, struct disk_append_reply *reply)
{
  if (!reply) {
    return CALL_BAD_REQUEST;
  }
  *reply = (struct disk_append_reply){.disk = HANDLE_INVALID,
      .phase = DISK_APPEND_PREPARE, .failed_block = UINT64_MAX};
  if (!selection_valid(selection)) {
    return CALL_BAD_REQUEST;
  }
  struct {
    struct message_header header;
    struct disk_partition_selection body;
  } message = {{PROTOCOL_DISK, DISK_APPEND_POOL}, *selection};
  struct disk_append_reply received = {.disk = HANDLE_INVALID,
      .phase = DISK_APPEND_PREPARE, .failed_block = UINT64_MAX};
  struct syscall_result result = syscall_call(review, &message, sizeof(message),
      &received, sizeof(received));
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_OUTCOME_UNKNOWN;
  }
  if (result.status != CALL_OK && !result.reply_size) {
    return result.status;
  }
  if (result.reply_size != sizeof(received) ||
      received.phase > DISK_APPEND_COMPLETE ||
      (received.flags & ~DISK_APPEND_EFFECTS_POSSIBLE) ||
      (received.phase <= DISK_APPEND_MATCH && received.flags) ||
      memcmp(&received.partition, selection, sizeof(*selection)) ||
      (received.failed_block != UINT64_MAX &&
          received.failed_block >= selection->device_block_count) ||
      (result.status == CALL_OK &&
          (received.disk == HANDLE_INVALID || received.phase != DISK_APPEND_COMPLETE ||
              received.flags != DISK_APPEND_EFFECTS_POSSIBLE ||
              received.failed_block != UINT64_MAX)) ||
      (result.status != CALL_OK && received.disk != HANDLE_INVALID)) {
    return CALL_OUTCOME_UNKNOWN;
  }
  *reply = received;
  return result.status;
}

enum call_status disks_open_fat32(handle_t disks, uint64_t id, uint64_t partition,
    uint64_t access, uint64_t rights, const uint8_t expected_disk_guid[16],
    struct mount_fat32_reply *mount)
{
  if (!mount) {
    return CALL_BAD_REQUEST;
  }
  *mount = (struct mount_fat32_reply){0};
  if (!id || !partition || partition > UINT32_MAX ||
      (access != MOUNT_ACCESS_READ_ONLY && access != MOUNT_ACCESS_READ_WRITE) ||
      (rights & ~DIRECTORY_CONTENT_RIGHTS)) {
    return CALL_BAD_REQUEST;
  }
  struct {
    struct message_header header;
    struct disks_fat32_request body;
  } message = {{PROTOCOL_DISKS, DISKS_OPEN_FAT32}, {id, partition, access, rights, {0}}};
  if (expected_disk_guid) {
    memcpy(message.body.expected_disk_guid, expected_disk_guid, 16);
  }
  struct mount_fat32_reply reply = {0};
  struct syscall_result result = syscall_call(disks, &message, sizeof(message),
      &reply, sizeof(reply));
  enum call_status status = reply_status(result, sizeof(reply));
  if (status != CALL_OK) {
    return status;
  }
  if (reply.root == HANDLE_INVALID || reply.control == HANDLE_INVALID ||
      reply.root == reply.control) {
    return CALL_OUTCOME_UNKNOWN;
  }
  *mount = reply;
  return CALL_OK;
}

enum call_status disks_open_owned_esp(handle_t disks,
    const struct disk_partition_selection *selection, uint64_t access, uint64_t rights,
    struct mount_fat32_reply *mount)
{
  if (!mount) return CALL_BAD_REQUEST;
  *mount = (struct mount_fat32_reply){0};
  if (!selection || !selection->id || !selection->partition ||
      (access != MOUNT_ACCESS_READ_ONLY && access != MOUNT_ACCESS_READ_WRITE) ||
      (rights & ~DIRECTORY_CONTENT_RIGHTS)) return CALL_BAD_REQUEST;
  struct {
    struct message_header header;
    struct disks_owned_esp_request body;
  } message = {{PROTOCOL_DISKS, DISKS_OPEN_OWNED_ESP}, {*selection, access, rights}};
  struct mount_fat32_reply reply = {0};
  enum call_status status = reply_status(syscall_call(disks, &message, sizeof(message),
      &reply, sizeof(reply)), sizeof(reply));
  if (status != CALL_OK) return status;
  if (!reply.root || !reply.control || reply.root == reply.control) return CALL_OUTCOME_UNKNOWN;
  *mount = reply;
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
  if (!partition || partition > UINT32_MAX || !name ||
      !(rights & DIRECTORY_RIGHT_LOOKUP) || (rights & ~DIRECTORY_RIGHTS)) {
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

enum call_status disk_open_fat32(handle_t disk, uint64_t partition,
    uint64_t access, uint64_t rights, struct mount_fat32_reply *mount)
{
  if (!mount) {
    return CALL_BAD_REQUEST;
  }
  *mount = (struct mount_fat32_reply){HANDLE_INVALID, HANDLE_INVALID};
  if (!partition || partition > UINT32_MAX ||
      (access != MOUNT_ACCESS_READ_ONLY && access != MOUNT_ACCESS_READ_WRITE) ||
      (rights & ~DIRECTORY_CONTENT_RIGHTS) ||
      (access == MOUNT_ACCESS_READ_ONLY &&
          (rights & ~(DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_ENUMERATE |
              DIRECTORY_RIGHT_READ_FILES)))) {
    return CALL_BAD_REQUEST;
  }
  struct {
    struct message_header header;
    struct disk_fat32_request body;
  } message = {{PROTOCOL_DISK, DISK_OPEN_FAT32}, {partition, access, rights}};
  struct mount_fat32_reply reply = {HANDLE_INVALID, HANDLE_INVALID};
  enum call_status status = reply_status(syscall_call(disk, &message, sizeof(message),
      &reply, sizeof(reply)), sizeof(reply));
  if (status != CALL_OK) {
    return status;
  }
  if (reply.root == HANDLE_INVALID || reply.control == HANDLE_INVALID ||
      reply.root == reply.control) {
    return CALL_OUTCOME_UNKNOWN;
  }
  *mount = reply;
  return CALL_OK;
}

enum call_status disk_create_volume(handle_t disk, uint64_t partition, const char *name)
{
  if (!partition || partition > UINT32_MAX || !name) {
    return CALL_BAD_REQUEST;
  }
  size_t length = strnlen(name, MOUNT_VOLUME_NAME_MAX + 1);
  if (!length || length > MOUNT_VOLUME_NAME_MAX) {
    return CALL_BAD_REQUEST;
  }
  struct {
    struct message_header header;
    struct disk_create_volume_request body;
  } message = {{PROTOCOL_DISK, DISK_CREATE_VOLUME}, {partition, (uintptr_t)name, length}};
  return reply_status(syscall_call(disk, &message, sizeof(message), NULL, 0), 0);
}

enum call_status disk_claim(handle_t disk, uint64_t partition)
{
  if (partition > UINT32_MAX) {
    return CALL_BAD_REQUEST;
  }
  struct {
    struct message_header header;
    struct disk_claim_request body;
  } message = {{PROTOCOL_DISK, DISK_CLAIM}, {partition}};
  return reply_status(syscall_call(disk, &message, sizeof(message), NULL, 0), 0);
}
