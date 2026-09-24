#include <directory.h>
#include <syscall.h>

static enum call_status call_status(struct syscall_result result, size_t reply_size)
{
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  if (result.reply_size != (result.status == CALL_OK ? reply_size : 0)) {
    return CALL_BAD_REQUEST;
  }
  return result.status;
}

static enum call_status child_call(handle_t directory, uint64_t operation, const char *name,
    uint64_t kind, uint64_t rights, handle_t *handle)
{
  if (!handle) {
    return CALL_BAD_REQUEST;
  }
  if (!name) {
    *handle = HANDLE_INVALID;
    return CALL_BAD_REQUEST;
  }
  size_t length = 0;
  while (name[length]) {
    ++length;
  }
  struct directory_message message = {
    .header = {PROTOCOL_DIRECTORY, operation},
  };
  struct directory_child_request request = {(uintptr_t)name, length, kind, rights};
  if (operation == DIRECTORY_LOOKUP) {
    message.body.lookup = request;
  } else {
    message.body.create = request;
  }
  struct directory_child_reply reply;
  enum call_status status = call_status(syscall_call(directory, &message, sizeof(message),
      &reply, sizeof(reply)), sizeof(reply));
  *handle = HANDLE_INVALID;
  if (status != CALL_OK) {
    return status;
  }
  if (reply.handle == HANDLE_INVALID) {
    return CALL_BAD_REQUEST;
  }
  *handle = reply.handle;
  return CALL_OK;
}

enum call_status directory_lookup(handle_t directory, const char *name,
    uint64_t kind, uint64_t rights, handle_t *handle)
{
  return child_call(directory, DIRECTORY_LOOKUP, name, kind, rights, handle);
}

enum call_status directory_create(handle_t directory, const char *name,
    uint64_t kind, uint64_t rights, handle_t *handle)
{
  return child_call(directory, DIRECTORY_CREATE, name, kind, rights, handle);
}

enum call_status directory_remove(handle_t directory, const char *name, uint64_t kind)
{
  if (!name) {
    return CALL_BAD_REQUEST;
  }
  size_t length = 0;
  while (name[length]) {
    ++length;
  }
  struct directory_message message = {
    .header = {PROTOCOL_DIRECTORY, DIRECTORY_REMOVE},
    .body.remove = {(uintptr_t)name, length, kind},
  };
  return call_status(syscall_call(directory, &message, sizeof(message), NULL, 0), 0);
}

enum call_status directory_rename(handle_t source, const char *source_name,
    handle_t destination, const char *destination_name, uint64_t policy)
{
  if (!source_name || !destination_name) {
    return CALL_BAD_REQUEST;
  }
  size_t source_length = 0, destination_length = 0;
  while (source_name[source_length]) {
    ++source_length;
  }
  while (destination_name[destination_length]) {
    ++destination_length;
  }
  struct directory_message message = {
    .header = {PROTOCOL_DIRECTORY, DIRECTORY_RENAME},
    .body.rename = {(uintptr_t)source_name, source_length, destination,
                   (uintptr_t)destination_name, destination_length, policy},
  };
  return call_status(syscall_call(source, &message, sizeof(message), NULL, 0), 0);
}

enum call_status directory_enumerate(handle_t directory, const struct directory_cursor *cursor,
    char *name, size_t capacity, struct directory_enumerate_reply *reply)
{
  if (!reply) {
    return CALL_BAD_REQUEST;
  }
  if (!cursor) {
    *reply = (struct directory_enumerate_reply){0};
    return CALL_BAD_REQUEST;
  }
  struct directory_cursor previous = *cursor;
  *reply = (struct directory_enumerate_reply){0};
  if (capacity && !name) {
    return CALL_BAD_REQUEST;
  }
  uintptr_t name_address = (uintptr_t)name;
  uintptr_t reply_address = (uintptr_t)reply;
  if (capacity > UINTPTR_MAX - name_address ||
      (capacity && name_address < reply_address + sizeof(*reply) &&
       reply_address < name_address + capacity)) {
    return CALL_BAD_REQUEST;
  }

  struct directory_message message = {
    .header = {PROTOCOL_DIRECTORY, DIRECTORY_ENUMERATE},
    .body.enumerate = {previous, name_address, capacity},
  };
  struct directory_enumerate_reply result;
  enum call_status status = call_status(syscall_call(directory, &message, sizeof(message),
      &result, sizeof(result)), sizeof(result));
  if (status != CALL_OK) {
    return status;
  }
  bool same_cursor = result.cursor.generation == previous.generation &&
                     result.cursor.position == previous.position;
  switch (result.outcome) {
  case DIRECTORY_ENTRY:
    if (!result.name_size || result.name_size > capacity || !result.cursor.generation ||
        previous.position == UINT64_MAX || result.cursor.position != previous.position + 1 ||
        (previous.generation && result.cursor.generation != previous.generation) ||
        name[result.name_size - 1]) {
      return CALL_BAD_REQUEST;
    }
    break;
  case DIRECTORY_BUFFER_TOO_SMALL:
    if (!same_cursor || result.name_size <= capacity) {
      return CALL_BAD_REQUEST;
    }
    break;
  case DIRECTORY_END:
    if (result.name_size || result.kind || !result.cursor.generation ||
        result.cursor.position != previous.position ||
        (previous.generation && result.cursor.generation != previous.generation)) {
      return CALL_BAD_REQUEST;
    }
    break;
  case DIRECTORY_CHANGED:
    if (result.name_size || result.kind || !same_cursor) {
      return CALL_BAD_REQUEST;
    }
    break;
  default:
    return CALL_BAD_REQUEST;
  }
  if ((result.outcome == DIRECTORY_ENTRY || result.outcome == DIRECTORY_BUFFER_TOO_SMALL) &&
      result.kind != DIRECTORY_KIND_FILE && result.kind != DIRECTORY_KIND_DIRECTORY) {
    return CALL_BAD_REQUEST;
  }
  *reply = result;
  return CALL_OK;
}
