#include <environment.h>
#include <file.h>
#include <syscall.h>
#include <abi/console.h>
#include <abi/file.h>
#include <abi/pipe.h>
#include <abi/stream.h>
#include <abi/http_transaction.h>

static enum call_status checked_status(struct syscall_result result)
{
  return result.status < CALL_STATUS_COUNT ? result.status : CALL_UNAVAILABLE;
}

bool environment_name(char target[ENVIRONMENT_NAME_MAX + 1], const char *name)
{
  if (!name) {
    return false;
  }
  size_t length = 0;
  while (name[length]) {
    char byte = name[length];
    if (length == ENVIRONMENT_NAME_MAX ||
        !((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
          (byte >= '0' && byte <= '9') || byte == '_' || byte == '-' ||
          byte == '.' || byte == '+')) {
      return false;
    }
    target[length++] = byte;
  }
  for (size_t i = length; i <= ENVIRONMENT_NAME_MAX; ++i) {
    target[i] = 0;
  }
  return length != 0;
}

static enum call_status read_entry(handle_t table, const void *request,
    size_t size, struct environment_entry_reply *entry)
{
  if (!entry) {
    return CALL_BAD_REQUEST;
  }
  *entry = (struct environment_entry_reply){0};
  struct environment_entry_reply reply = {0};
  struct syscall_result result = syscall_call(table, request, size, &reply, sizeof(reply));
  enum call_status status = checked_status(result);
  if (status == CALL_OK || status == CALL_BUFFER_TOO_SMALL) {
    if (result.reply_size != sizeof(reply) ||
        (reply.kind != ENVIRONMENT_CAP && reply.kind != ENVIRONMENT_TEXT) ||
        (status == CALL_OK && reply.kind == ENVIRONMENT_CAP && !reply.handle)) {
      return CALL_OUTCOME_UNKNOWN;
    }
    *entry = reply;
  } else if (result.reply_size) {
    return CALL_OUTCOME_UNKNOWN;
  }
  return status;
}

enum call_status environment_lookup(handle_t table, const char *name,
    struct environment_entry_reply *entry, char *text, size_t capacity)
{
  struct environment_lookup_message request = {
    .header = {PROTOCOL_ENVIRONMENT, ENVIRONMENT_LOOKUP},
    .text_address = (uintptr_t)text, .text_capacity = capacity,
  };
  if (!environment_name(request.name, name) || (capacity && !text)) {
    if (entry) {
      *entry = (struct environment_entry_reply){0};
    }
    return CALL_BAD_REQUEST;
  }
  return read_entry(table, &request, sizeof(request), entry);
}

enum call_status environment_lookup_file(handle_t table, const char *name, handle_t *handle)
{
  if (!handle) {
    return CALL_BAD_REQUEST;
  }
  *handle = HANDLE_INVALID;
  struct environment_lookup_message request = {
    .header = {PROTOCOL_ENVIRONMENT, ENVIRONMENT_LOOKUP_FILE},
  };
  if (!environment_name(request.name, name)) {
    return CALL_BAD_REQUEST;
  }
  struct environment_entry_reply entry;
  enum call_status status = read_entry(table, &request, sizeof(request), &entry);
  if (status != CALL_OK) {
    return status;
  }
  if (!entry.handle) {
    return CALL_OUTCOME_UNKNOWN;
  }
  *handle = entry.handle;
  return CALL_OK;
}

enum call_status environment_enumerate(handle_t snapshot, size_t index,
    struct environment_entry_reply *entry, char *text, size_t capacity)
{
  struct environment_enumerate_message request = {
    .header = {PROTOCOL_ENVIRONMENT_SNAPSHOT, ENVIRONMENT_ENUMERATE},
    .index = index, .text_address = (uintptr_t)text, .text_capacity = capacity,
  };
  return capacity && !text ? CALL_BAD_REQUEST : read_entry(snapshot, &request, sizeof(request), entry);
}

enum call_status environment_set_batch(handle_t table,
    const struct environment_set_entry *entries, size_t count)
{
  if (count && !entries) {
    return CALL_BAD_REQUEST;
  }
  struct environment_set_message request = {
    .header = {PROTOCOL_ENVIRONMENT, ENVIRONMENT_SET},
    .entries_address = (uintptr_t)entries, .count = count,
  };
  struct syscall_result result = syscall_call(table, &request, sizeof(request), NULL, 0);
  return result.reply_size ? CALL_OUTCOME_UNKNOWN : checked_status(result);
}

enum call_status environment_unset_batch(handle_t table,
    const struct environment_unset_entry *entries, size_t count)
{
  if (count && !entries) {
    return CALL_BAD_REQUEST;
  }
  struct environment_unset_message request = {
    .header = {PROTOCOL_ENVIRONMENT, ENVIRONMENT_UNSET},
    .entries_address = (uintptr_t)entries, .count = count,
  };
  struct syscall_result result = syscall_call(table, &request, sizeof(request), NULL, 0);
  return result.reply_size ? CALL_OUTCOME_UNKNOWN : checked_status(result);
}

enum call_status environment_set_capability(handle_t table, const char *name,
    handle_t source, uint64_t rights, uint64_t transport, uint64_t flags)
{
  struct environment_set_entry entry = {
    .kind = ENVIRONMENT_CAP, .flags = flags, .handle = source,
    .rights = rights, .transport = transport,
  };
  return environment_name(entry.name, name) ? environment_set_batch(table, &entry, 1) : CALL_BAD_REQUEST;
}

enum call_status environment_set_text(handle_t table, const char *name,
    const char *text, uint64_t flags, uint64_t set_flags)
{
  if (!text) {
    return CALL_BAD_REQUEST;
  }
  size_t length = 0;
  while (text[length]) {
    ++length;
  }
  struct environment_set_entry entry = {
    .kind = ENVIRONMENT_TEXT, .flags = flags, .set_flags = set_flags,
    .text_address = (uintptr_t)text, .text_length = length,
  };
  return environment_name(entry.name, name) ? environment_set_batch(table, &entry, 1) : CALL_BAD_REQUEST;
}

enum call_status environment_unset(handle_t table, const char *name)
{
  struct environment_unset_entry entry = {0};
  return environment_name(entry.name, name) ? environment_unset_batch(table, &entry, 1) : CALL_BAD_REQUEST;
}

enum call_status environment_info(handle_t table, struct environment_info_reply *info)
{
  if (!info) {
    return CALL_BAD_REQUEST;
  }
  *info = (struct environment_info_reply){0};
  struct message_header request = {PROTOCOL_ENVIRONMENT, ENVIRONMENT_INFO};
  struct syscall_result result = syscall_call(table, &request, sizeof(request), info, sizeof(*info));
  enum call_status status = checked_status(result);
  if ((status == CALL_OK && result.reply_size != sizeof(*info)) ||
      (status != CALL_OK && result.reply_size)) {
    status = CALL_OUTCOME_UNKNOWN;
  }
  if (status != CALL_OK) {
    *info = (struct environment_info_reply){0};
  }
  return status;
}

enum call_status environment_snapshot(handle_t table, uint64_t flags,
    handle_t *snapshot, struct environment_info_reply *info)
{
  if (!snapshot) {
    return CALL_BAD_REQUEST;
  }
  *snapshot = HANDLE_INVALID;
  if (info) {
    *info = (struct environment_info_reply){0};
  }
  struct environment_snapshot_message request = {
    .header = {PROTOCOL_ENVIRONMENT, ENVIRONMENT_SNAPSHOT}, .flags = flags,
  };
  struct environment_snapshot_reply reply = {0};
  struct syscall_result result = syscall_call(table, &request, sizeof(request), &reply, sizeof(reply));
  enum call_status status = checked_status(result);
  if (status != CALL_OK) {
    return result.reply_size ? CALL_OUTCOME_UNKNOWN : status;
  }
  if (result.reply_size != sizeof(reply) || !reply.handle) {
    return CALL_OUTCOME_UNKNOWN;
  }
  *snapshot = reply.handle;
  if (info) {
    *info = reply.info;
  }
  return CALL_OK;
}

#include <handle.h>
#include <stdlib.h>
#include <string.h>

void environment_capture_close(struct environment_capture *capture)
{
  if (!capture) {
    return;
  }
  for (size_t i = 0; i < capture->count; ++i) {
    if (capture->values[i].handle) {
      handle_close(capture->values[i].handle);
    }
    free(capture->values[i].text);
  }
  free(capture->values);
  *capture = (struct environment_capture){0};
}

enum call_status environment_capture_init(handle_t table, uint64_t flags,
    struct environment_capture *capture)
{
  if (!capture) {
    return CALL_BAD_REQUEST;
  }
  *capture = (struct environment_capture){0};
  handle_t snapshot;
  struct environment_info_reply info;
  enum call_status status = environment_snapshot(table, flags, &snapshot, &info);
  if (status != CALL_OK) {
    return status;
  }
  if (info.count > SIZE_MAX / sizeof(struct environment_value)) {
    status = CALL_LIMIT;
    goto done;
  }
  capture->values = info.count ? calloc(info.count, sizeof(*capture->values)) : NULL;
  if (info.count && !capture->values) {
    status = CALL_NO_MEMORY;
    goto done;
  }
  capture->generation = info.generation;
  for (size_t i = 0; i < info.count; ++i) {
    struct environment_entry_reply entry;
    status = environment_enumerate(snapshot, i, &entry, NULL, 0);
    char *text = NULL;
    if (status == CALL_BUFFER_TOO_SMALL && entry.kind == ENVIRONMENT_TEXT) {
      if (entry.text_length == SIZE_MAX) {
        status = CALL_LIMIT;
        break;
      }
      text = malloc(entry.text_length + 1);
      if (!text) {
        status = CALL_NO_MEMORY;
        break;
      }
      status = environment_enumerate(snapshot, i, &entry, text, entry.text_length + 1);
    }
    if (status != CALL_OK) {
      free(text);
      break;
    }
    struct environment_value *value = &capture->values[capture->count++];
    memcpy(value->name, entry.name, sizeof(value->name));
    value->kind = entry.kind;
    value->flags = entry.flags;
    value->identity = entry.identity;
    value->handle = entry.handle;
    value->rights = entry.rights;
    value->transport = entry.transport;
    value->text = text;
    value->length = entry.text_length;
  }
done:
  if (handle_close(snapshot) && status == CALL_OK) {
    status = CALL_OUTCOME_UNKNOWN;
  }
  if (status != CALL_OK) {
    environment_capture_close(capture);
  }
  return status;
}

const struct environment_value *environment_capture_find(
    const struct environment_capture *capture, const char *name)
{
  if (!capture || !name) {
    return NULL;
  }
  for (size_t i = 0; i < capture->count; ++i) {
    if (!strcmp(capture->values[i].name, name)) {
      return &capture->values[i];
    }
  }
  return NULL;
}

enum call_status environment_capability(handle_t table, const char *name, handle_t *handle)
{
  if (!handle) {
    return CALL_BAD_REQUEST;
  }
  *handle = HANDLE_INVALID;
  struct environment_entry_reply entry;
  enum call_status status = environment_lookup(table, name, &entry, NULL, 0);
  if (status != CALL_OK) {
    return status;
  }
  if (entry.kind != ENVIRONMENT_CAP) {
    return CALL_WRONG_TYPE;
  }
  *handle = entry.handle;
  return CALL_OK;
}

const char *environment_stdio_append_name(enum environment_stream_index index)
{
  static const char *const names[ENVIRONMENT_STREAM_COUNT] = {
    "stdin.append", "stdout.append", "stderr.append",
  };
  return (unsigned)index < ENVIRONMENT_STREAM_COUNT ? names[index] : NULL;
}

struct launch_environment_entry environment_stdio_append_entry(enum environment_stream_index index)
{
  return (struct launch_environment_entry){
    .name = (uintptr_t)environment_stdio_append_name(index), .kind = ENVIRONMENT_TEXT,
    .flags = ENVIRONMENT_EXPORTED, .value = (uintptr_t)"1", .length = 1,
  };
}

static enum call_status stdio_append(handle_t snapshot, enum environment_stream_index index,
    bool *append)
{
  struct environment_entry_reply entry;
  char text[2];
  enum call_status status = environment_lookup(snapshot, environment_stdio_append_name(index),
      &entry, text, sizeof(text));
  *append = false;
  if (status == CALL_NOT_FOUND) {
    return CALL_OK;
  }
  if (status == CALL_OK && entry.kind == ENVIRONMENT_CAP) {
    handle_close(entry.handle);
    return CALL_WRONG_TYPE;
  }
  if (status == CALL_BUFFER_TOO_SMALL) {
    return CALL_BAD_REQUEST;
  }
  if (status != CALL_OK) {
    return status;
  }
  if (index == ENVIRONMENT_STDIN || entry.kind != ENVIRONMENT_TEXT ||
      entry.text_length != 1 || text[0] != '1') {
    return CALL_BAD_REQUEST;
  }
  *append = true;
  return CALL_OK;
}

enum call_status environment_stdio(handle_t snapshot, enum environment_stream_index index,
    struct pyxis_stdio_binding *binding, uint64_t *identity, uint64_t *observation_identity)
{
  static const char *const names[ENVIRONMENT_STREAM_COUNT] = {"stdin", "stdout", "stderr"};
  if (!binding || (unsigned)index >= ENVIRONMENT_STREAM_COUNT) {
    return CALL_BAD_REQUEST;
  }
  *binding = (struct pyxis_stdio_binding){0};
  if (identity) {
    *identity = 0;
  }
  if (observation_identity) {
    *observation_identity = 0;
  }
  struct environment_entry_reply entry;
  enum call_status status = environment_lookup(snapshot, names[index], &entry, NULL, 0);
  if (status != CALL_OK) {
    return status;
  }
  if (entry.kind != ENVIRONMENT_CAP) {
    return CALL_WRONG_TYPE;
  }
  struct handle_info info;
  status = handle_query(entry.handle, &info);
  if (status != CALL_OK) {
    handle_close(entry.handle);
    return status;
  }
  bool append;
  status = stdio_append(snapshot, index, &append);
  if (status != CALL_OK) {
    handle_close(entry.handle);
    return status;
  }
  bool input = index == ENVIRONMENT_STDIN;
  uint64_t required = 0;
  bool valid = false;
  if (info.protocol == PROTOCOL_CONSOLE && info.kind == HANDLE_KIND_NATIVE) {
    required = input ? CONSOLE_RIGHT_READ : CONSOLE_RIGHT_WRITE;
    valid = !info.transport;
  } else if (info.protocol == PROTOCOL_PIPE && info.kind == HANDLE_KIND_NATIVE) {
    required = input ? PIPE_RIGHT_READ : PIPE_RIGHT_WRITE;
    valid = !info.transport;
  } else if (info.protocol == PROTOCOL_FILE) {
    required = input ? FILE_RIGHT_READ : FILE_RIGHT_WRITE;
    valid = (info.kind == HANDLE_KIND_NATIVE && !info.transport) ||
        (info.kind == HANDLE_KIND_EXPORTED && (info.transport & HANDLE_TRANSPORT_CALL));
  } else if (info.protocol == PROTOCOL_STREAM && input && info.kind == HANDLE_KIND_EXPORTED) {
    required = STREAM_RIGHT_READ;
    valid = info.transport & HANDLE_TRANSPORT_CALL;
  }
  if (append) {
    valid = valid && info.protocol == PROTOCOL_FILE;
  }
  if (!valid || (info.rights & required) != required) {
    handle_close(entry.handle);
    return valid ? CALL_DENIED : CALL_WRONG_TYPE;
  }
  if (append) {
    uint64_t size;
    status = file_size(entry.handle, &size);
    if (status != CALL_OK) {
      handle_close(entry.handle);
      return status;
    }
  }
  binding->handle = entry.handle;
  binding->protocol = info.protocol;
  binding->append = append;
  if (identity) {
    *identity = entry.identity;
  }
  if (index == ENVIRONMENT_STDIN && info.protocol == PROTOCOL_STREAM) {
    status = environment_lookup(snapshot, "stdin.status", &entry, NULL, 0);
    if (status == CALL_OK && entry.kind == ENVIRONMENT_CAP) {
      binding->observation = entry.handle;
      status = handle_query(entry.handle, &info);
      if (status == CALL_OK &&
          (info.protocol != PROTOCOL_HTTP_TRANSACTION || info.kind != HANDLE_KIND_EXPORTED ||
           (info.rights & HTTP_TRANSACTION_RIGHT_STATUS) != HTTP_TRANSACTION_RIGHT_STATUS ||
           (info.transport & HANDLE_TRANSPORT_CALL) != HANDLE_TRANSPORT_CALL)) {
        status = CALL_WRONG_TYPE;
      }
      if (status == CALL_OK) {
        binding->observation_protocol = info.protocol;
        if (observation_identity) {
          *observation_identity = entry.identity;
        }
      }
    } else if (status == CALL_OK) {
      status = CALL_WRONG_TYPE;
    }
  }
  if (status != CALL_OK) {
    handle_close(binding->handle);
    if (binding->observation) {
      handle_close(binding->observation);
    }
    *binding = (struct pyxis_stdio_binding){0};
  }
  return status;
}

enum call_status environment_capture_stage(const struct environment_capture *capture,
    struct launch_grant *grants, size_t grant_capacity, size_t *grant_count,
    struct launch_environment_entry *entries, size_t entry_capacity, size_t *entry_count)
{
  if (!capture || !grant_count || !entry_count ||
      *grant_count > grant_capacity || *entry_count > entry_capacity ||
      capture->count > entry_capacity - *entry_count ||
      (capture->count && !entries)) {
    return CALL_BAD_REQUEST;
  }
  size_t capabilities = 0;
  for (size_t i = 0; i < capture->count; ++i) {
    const struct environment_value *value = &capture->values[i];
    if (value->kind == ENVIRONMENT_CAP) {
      ++capabilities;
    } else if (value->kind != ENVIRONMENT_TEXT) {
      return CALL_BAD_REQUEST;
    }
  }
  if (capabilities > grant_capacity - *grant_count || (capabilities && !grants)) {
    return CALL_LIMIT;
  }
  for (size_t i = 0; i < capture->count; ++i) {
    const struct environment_value *value = &capture->values[i];
    struct launch_environment_entry entry = {
      .name = (uintptr_t)value->name, .kind = value->kind, .flags = value->flags,
      .value = (uintptr_t)value->text, .length = value->length,
    };
    if (value->kind == ENVIRONMENT_CAP) {
      entry.value = *grant_count;
      entry.length = 0;
      grants[(*grant_count)++] = (struct launch_grant){value->handle, value->rights, value->transport};
    }
    entries[(*entry_count)++] = entry;
  }
  return CALL_OK;
}
