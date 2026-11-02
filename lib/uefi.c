#include <uefi.h>
#include <syscall.h>
#include <string.h>

enum call_status uefi_boot_register(handle_t uefi,
    const struct uefi_boot_register_request *request)
{
  if (!request) return CALL_BAD_REQUEST;
  struct {
    struct message_header header;
    struct uefi_boot_register_request body;
  } message = {{PROTOCOL_UEFI, UEFI_BOOT_REGISTER}, *request};
  struct syscall_result result = syscall_call(uefi, &message, sizeof(message), NULL, 0);
  if (result.status >= CALL_STATUS_COUNT || result.reply_size) return CALL_OUTCOME_UNKNOWN;
  return result.status;
}

enum call_status uefi_boot_variable_get(handle_t uefi, uint32_t kind, uint32_t number,
    uint32_t *attributes, void *value, size_t capacity, size_t *size)
{
  if (!attributes || !size || (capacity && !value)) return CALL_BAD_REQUEST;
  struct {
    struct message_header header;
    struct uefi_boot_variable_name body;
  } message = {{PROTOCOL_UEFI, UEFI_BOOT_VARIABLE_GET}, {kind, number}};
  struct {
    struct uefi_boot_variable_reply header;
    uint8_t value[UEFI_BOOT_VARIABLE_BYTES];
  } reply;
  struct syscall_result result = syscall_call(uefi, &message, sizeof(message), &reply, sizeof(reply));
  if (result.status != CALL_OK) return result.status;
  if (result.reply_size < sizeof(reply.header) ||
      result.reply_size != sizeof(reply.header) + reply.header.size) {
    return CALL_OUTCOME_UNKNOWN;
  }
  if (reply.header.size > capacity) return CALL_LIMIT;
  memcpy(value, reply.value, reply.header.size);
  *attributes = reply.header.attributes;
  *size = reply.header.size;
  return CALL_OK;
}

enum call_status uefi_boot_variable_set(handle_t uefi, uint32_t kind, uint32_t number,
    const void *value, size_t size)
{
  if (!value || !size || size > UEFI_BOOT_VARIABLE_BYTES) return CALL_BAD_REQUEST;
  struct {
    struct message_header header;
    struct uefi_boot_variable_set_request body;
    uint8_t value[UEFI_BOOT_VARIABLE_BYTES];
  } message = {{PROTOCOL_UEFI, UEFI_BOOT_VARIABLE_SET}, {{kind, number}, size, 0}, {0}};
  memcpy(message.value, value, size);
  struct syscall_result result = syscall_call(uefi, &message,
      sizeof(message.header) + sizeof(message.body) + size, NULL, 0);
  return result.status >= CALL_STATUS_COUNT || result.reply_size ? CALL_OUTCOME_UNKNOWN : result.status;
}

enum call_status uefi_boot_variable_delete(handle_t uefi, uint32_t kind, uint32_t number)
{
  struct {
    struct message_header header;
    struct uefi_boot_variable_name body;
  } message = {{PROTOCOL_UEFI, UEFI_BOOT_VARIABLE_DELETE}, {kind, number}};
  struct syscall_result result = syscall_call(uefi, &message, sizeof(message), NULL, 0);
  return result.status >= CALL_STATUS_COUNT || result.reply_size ? CALL_OUTCOME_UNKNOWN : result.status;
}

enum call_status uefi_boot_entries(handle_t uefi, uint32_t first, uint32_t count,
    uint8_t bitmap[UEFI_BOOT_ENTRY_WINDOW / 8])
{
  struct {
    struct message_header header;
    struct uefi_boot_entries_request body;
  } message = {{PROTOCOL_UEFI, UEFI_BOOT_ENTRIES}, {first, count}};
  struct syscall_result result = syscall_call(uefi, &message, sizeof(message), bitmap,
      UEFI_BOOT_ENTRY_WINDOW / 8);
  if (result.status != CALL_OK) return result.status;
  return result.reply_size == (count + 7) / 8 ? CALL_OK : CALL_OUTCOME_UNKNOWN;
}

static uint16_t load16(const uint8_t *bytes)
{
  return (uint16_t)(bytes[0] | (bytes[1] << 8));
}

static uint32_t load32(const uint8_t *bytes)
{
  return (uint32_t)load16(bytes) | ((uint32_t)load16(bytes + 2) << 16);
}

static uint64_t load64(const uint8_t *bytes)
{
  return (uint64_t)load32(bytes) | ((uint64_t)load32(bytes + 4) << 32);
}

bool uefi_load_option_parse(const void *bytes, size_t size, struct uefi_load_option *option)
{
  const uint8_t *data = bytes;
  if (!data || !option || size < 6) return false;
  size_t path_size = load16(data + 4);
  size_t at = 6;
  while (at + 1 < size && load16(data + at)) at += 2;
  if (at + 2 > size) return false;
  option->attributes = load32(data);
  option->description = data + 6;
  option->description_units = (at - 6) / 2;
  at += 2;
  if (path_size > size - at) return false;
  option->path = data + at;
  option->path_size = path_size;
  option->data = data + at + path_size;
  option->data_size = size - at - path_size;
  return true;
}

size_t uefi_load_option_label(const struct uefi_load_option *option, char *label, size_t capacity)
{
  if (!capacity) return 0;
  size_t length = 0;
  for (; length < option->description_units && length + 1 < capacity; ++length) {
    uint16_t unit = load16(option->description + length * 2);
    label[length] = unit < 0x80 ? (char)unit : '?';
  }
  label[length] = '\0';
  return length;
}

bool uefi_path_next(const uint8_t *path, size_t size, size_t *offset, struct uefi_path_node *node)
{
  size_t at = *offset;
  if (at > size || size - at < 4) return false;
  size_t length = load16(path + at + 2);
  if (length < 4 || length > size - at || path[at] == UEFI_PATH_END) return false;
  node->type = path[at];
  node->subtype = path[at + 1];
  node->data = path + at + 4;
  node->data_size = length - 4;
  *offset = at + length;
  return true;
}

bool uefi_path_hard_drive(const struct uefi_path_node *node, struct uefi_hard_drive *drive)
{
  if (node->type != UEFI_PATH_MEDIA || node->subtype != UEFI_PATH_HARD_DRIVE ||
      node->data_size != 38) {
    return false;
  }
  drive->partition_number = load32(node->data);
  drive->first_lba = load64(node->data + 4);
  drive->block_count = load64(node->data + 12);
  memcpy(drive->signature, node->data + 20, sizeof(drive->signature));
  drive->signature_type = node->data[37];
  return true;
}
