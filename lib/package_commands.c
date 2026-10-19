#include <package_commands.h>
#include <abi/directory.h>
#include <abi/file.h>
#include <handle.h>
#include <string.h>
#include <syscall.h>

#define COMMAND_DIRECTORY_RIGHTS (DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_ENUMERATE | \
    DIRECTORY_RIGHT_READ_FILES)

static enum call_status native_directory(handle_t handle)
{
  struct handle_info info;
  enum call_status status = handle_query(handle, &info);
  if (status != CALL_OK) return status;
  if (info.protocol != PROTOCOL_DIRECTORY) return CALL_WRONG_TYPE;
  return info.kind == HANDLE_KIND_NATIVE && !info.transport ? CALL_OK : CALL_BAD_OPERATION;
}

static enum call_status result_status(struct syscall_result result, size_t size)
{
  if (result.status >= CALL_STATUS_COUNT ||
      result.reply_size != (result.status == CALL_OK ? size : 0)) {
    return CALL_BAD_REQUEST;
  }
  return result.status;
}

static bool info_valid(const struct package_command_info *info)
{
  if (info->limit != PACKAGE_COMMANDS_LIMIT || info->count > info->limit ||
      info->set > INT64_MAX || (info->flags & ~PACKAGE_COMMAND_VIEW_LIVE)) {
    return false;
  }
  return !(info->flags & PACKAGE_COMMAND_VIEW_LIVE) || (!info->set && !info->count);
}

static bool name_valid(const char *name)
{
  if (!name) return false;
  size_t size = strnlen(name, PACKAGE_COMMAND_NAME_MAX + 1);
  if (!size || size > PACKAGE_COMMAND_NAME_MAX || !strcmp(name, ".") || !strcmp(name, "..")) {
    return false;
  }
  for (size_t i = 0; i < size; ++i) {
    unsigned char byte = name[i];
    if (byte <= 0x20 || byte == 0x7f || byte == '/' || byte == ':' || byte == '\\') {
      return false;
    }
    if (byte < 0x80) continue;
    unsigned remaining;
    uint32_t value, minimum;
    if ((byte & 0xe0) == 0xc0) {
      remaining = 1;
      value = byte & 0x1f;
      minimum = 0x80;
    } else if ((byte & 0xf0) == 0xe0) {
      remaining = 2;
      value = byte & 0x0f;
      minimum = 0x800;
    } else if ((byte & 0xf8) == 0xf0) {
      remaining = 3;
      value = byte & 7;
      minimum = 0x10000;
    } else {
      return false;
    }
    if (remaining > size - i - 1) return false;
    while (remaining--) {
      byte = name[++i];
      if ((byte & 0xc0) != 0x80) return false;
      value = (value << 6) | (byte & 0x3f);
    }
    if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) {
      return false;
    }
  }
  return true;
}

static bool padded(const char *text, size_t capacity)
{
  const char *end = memchr(text, 0, capacity);
  if (!end) return false;
  for (; end < text + capacity; ++end) {
    if (*end) return false;
  }
  return true;
}

static bool record_valid(const struct package_command_record *record, const char *name,
    uint64_t protocol)
{
  if (!padded(record->name, sizeof(record->name)) || strcmp(record->name, name) ||
      !padded(record->id, sizeof(record->id)) || !record->id[0] ||
      !padded(record->version, sizeof(record->version)) || !record->version[0] ||
      !padded(record->path, sizeof(record->path)) || !record->path[0] ||
      !padded(record->command, sizeof(record->command)) ||
      record->reserved[0] || record->reserved[1] || record->reserved[2]) {
    return false;
  }
  if (protocol == PROTOCOL_FILE) {
    return record->kind == DIRECTORY_KIND_FILE && !record->command[0];
  }
  return protocol == PROTOCOL_DIRECTORY && record->kind == DIRECTORY_KIND_DIRECTORY &&
      name_valid(record->command);
}

static enum call_status child_call(handle_t source, handle_t previous,
    uint64_t operation, handle_t *output)
{
  if (!output) return CALL_BAD_REQUEST;
  *output = HANDLE_INVALID;
  enum call_status status = native_directory(source);
  if (status != CALL_OK) return status;
  if (previous != HANDLE_INVALID) {
    status = native_directory(previous);
    if (status != CALL_OK) return status;
  }
  struct directory_message message = {.header = {PROTOCOL_DIRECTORY, operation}};
  if (operation == DIRECTORY_COMMAND_CAPTURE) {
    struct package_command_capture_request request = {.previous = previous};
    memcpy(&message.body, &request, sizeof(request));
  }
  struct directory_child_reply reply = {0};
  status = result_status(syscall_call(source, &message, sizeof(message), &reply, sizeof(reply)),
      sizeof(reply));
  if (status == CALL_OK) {
    if (reply.handle == HANDLE_INVALID || reply.handle == source || reply.handle == previous) {
      status = CALL_BAD_REQUEST;
    } else {
      struct handle_info info;
      status = handle_query(reply.handle, &info);
      if (status == CALL_OK && (info.protocol != PROTOCOL_DIRECTORY ||
          info.kind != HANDLE_KIND_NATIVE || info.transport ||
          (info.rights & ~COMMAND_DIRECTORY_RIGHTS))) {
        status = CALL_BAD_REQUEST;
      }
    }
  }
  if (status == CALL_OK) {
    *output = reply.handle;
  } else if (reply.handle != HANDLE_INVALID && reply.handle != source && reply.handle != previous) {
    handle_close(reply.handle);
  }
  return status;
}

enum call_status package_commands_open(handle_t volume, handle_t *live)
{
  return child_call(volume, HANDLE_INVALID, DIRECTORY_PACKAGE_COMMANDS, live);
}

enum call_status package_commands_info(handle_t view, struct package_command_info *info)
{
  if (!info) return CALL_BAD_REQUEST;
  *info = (struct package_command_info){0};
  enum call_status status = native_directory(view);
  if (status != CALL_OK) return status;
  struct directory_message message = {.header = {PROTOCOL_DIRECTORY, DIRECTORY_COMMAND_INFO}};
  struct package_command_info reply = {0};
  status = result_status(syscall_call(view, &message, sizeof(message), &reply, sizeof(reply)),
      sizeof(reply));
  if (status == CALL_OK && !info_valid(&reply)) status = CALL_BAD_REQUEST;
  if (status == CALL_OK) *info = reply;
  return status;
}

enum call_status package_commands_capture(handle_t view, handle_t previous, handle_t *fixed)
{
  return child_call(view, previous, DIRECTORY_COMMAND_CAPTURE, fixed);
}

enum call_status package_commands_lookup(handle_t fixed, const char *name,
    uint64_t file_rights, uint64_t directory_rights,
    struct package_command_lookup_reply *reply)
{
  if (!reply) return CALL_BAD_REQUEST;
  *reply = (struct package_command_lookup_reply){0};
  if (!name_valid(name) || (file_rights & ~FILE_RIGHT_READ) ||
      (directory_rights & ~COMMAND_DIRECTORY_RIGHTS)) {
    return CALL_BAD_REQUEST;
  }
  enum call_status status = native_directory(fixed);
  if (status != CALL_OK) return status;
  struct directory_message message = {.header = {PROTOCOL_DIRECTORY, DIRECTORY_COMMAND_LOOKUP}};
  struct package_command_lookup_request request = {
    .name = (uintptr_t)name, .name_length = strlen(name),
    .file_rights = file_rights, .directory_rights = directory_rights,
  };
  memcpy(&message.body, &request, sizeof(request));
  struct package_command_lookup_reply child = {0};
  status = result_status(syscall_call(fixed, &message, sizeof(message), &child, sizeof(child)),
      sizeof(child));
  if (status == CALL_OK) {
    if (child.handle == HANDLE_INVALID || child.handle == fixed || !info_valid(&child.info) ||
        child.info.flags || !record_valid(&child.target, name, child.protocol)) {
      status = CALL_BAD_REQUEST;
    } else {
      struct handle_info info;
      status = handle_query(child.handle, &info);
      uint64_t requested = child.protocol == PROTOCOL_FILE ? file_rights : directory_rights;
      if (status == CALL_OK && (info.protocol != child.protocol ||
          info.kind != HANDLE_KIND_NATIVE || info.transport || (info.rights & ~requested))) {
        status = CALL_BAD_REQUEST;
      }
    }
  }
  if (status == CALL_OK) {
    *reply = child;
  } else if (child.handle != HANDLE_INVALID && child.handle != fixed) {
    handle_close(child.handle);
  }
  return status;
}
