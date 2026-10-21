#include <resource.h>
#include <abi/transaction.h>
#include <abi/stream.h>
#include <abi/pipe.h>
#include <abi/console.h>
#include <endpoint.h>
#include <environment.h>
#include <path.h>
#include <startup.h>
#include <directory.h>
#include <file.h>
#include <handle.h>
#include <provider.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static bool counted_utf8(const char *text, size_t size, size_t capacity)
{
  if (size >= capacity) {
    return false;
  }
  size_t offset = 0;
  while (offset < size) {
    unsigned char first = text[offset++];
    if (!first) {
      return false;
    }
    if (first < 0x80) {
      continue;
    }
    size_t following;
    unsigned char lower = 0x80, upper = 0xbf;
    if (first >= 0xc2 && first <= 0xdf) {
      following = 1;
    } else if (first >= 0xe0 && first <= 0xef) {
      following = 2;
      if (first == 0xe0) {
        lower = 0xa0;
      } else if (first == 0xed) {
        upper = 0x9f;
      }
    } else if (first >= 0xf0 && first <= 0xf4) {
      following = 3;
      if (first == 0xf0) {
        lower = 0x90;
      } else if (first == 0xf4) {
        upper = 0x8f;
      }
    } else {
      return false;
    }
    if (following > size - offset) {
      return false;
    }
    unsigned char second = text[offset++];
    if (second < lower || second > upper) {
      return false;
    }
    for (size_t i = 1; i < following; ++i) {
      unsigned char byte = text[offset++];
      if (byte < 0x80 || byte > 0xbf) {
        return false;
      }
    }
  }
  for (size_t i = size; i < capacity; ++i) {
    if (text[i]) {
      return false;
    }
  }
  return true;
}

bool resource_description_valid(const struct resource_description *description)
{
  return description && description->outcome < RESOURCE_OUTCOME_COUNT &&
      !(description->flags & ~RESOURCE_FLAGS) &&
      ((description->flags & RESOURCE_LENGTH_KNOWN) || !description->length) &&
      counted_utf8(description->detail, description->detail_size,
          sizeof(description->detail)) &&
      counted_utf8(description->media_type, description->media_type_size,
          sizeof(description->media_type));
}

enum call_status resource_outcome_status(uint64_t outcome)
{
  switch (outcome) {
  case RESOURCE_OK: return CALL_OK;
  case RESOURCE_NOT_FOUND: return CALL_NOT_FOUND;
  case RESOURCE_DENIED: return CALL_DENIED;
  case RESOURCE_IO_ERROR: return CALL_IO;
  case RESOURCE_TOO_LARGE: return CALL_FILE_TOO_LARGE;
  case RESOURCE_ABORTED: return CALL_ABANDONED;
  case RESOURCE_UNCERTAIN: return CALL_OUTCOME_UNKNOWN;
  default: return CALL_BAD_REQUEST;
  }
}

enum call_status resource_result_close(struct resource_result *result)
{
  if (!result) {
    return CALL_BAD_REQUEST;
  }
  enum call_status status = CALL_OK;
  if (result->info.error_body != HANDLE_INVALID &&
      result->info.error_body != result->resource && handle_close(result->info.error_body) < 0) {
    status = CALL_BAD_HANDLE;
  }
  if (result->resource != HANDLE_INVALID && handle_close(result->resource) < 0) {
    status = CALL_BAD_HANDLE;
  }
  *result = (struct resource_result){0};
  return status;
}

enum call_status resource_result_adopt(handle_t resource, struct resource_result *result)
{
  if (!result) {
    return CALL_BAD_REQUEST;
  }
  *result = (struct resource_result){0};
  struct describe_reply info;
  enum call_status status = handle_describe(resource, DESCRIBE_INCLUDE_ERROR_BODY, &info);
  if (status == CALL_OK) {
    result->resource = resource;
    result->info = info;
  }
  return status;
}

static enum call_status truncate_file(handle_t file, const struct describe_reply *info,
    uint64_t deadline_ns, uint64_t *delivery)
{
  if (info->kind == HANDLE_KIND_NATIVE) {
    /* Native resize retains its existing synchronous wait/cancellation contract. */
    return file_resize(file, 0);
  }
  struct file_resize_request request = {.size = 0};
  struct endpoint_packet packet = {0};
  enum call_status status = endpoint_invoke(file, PROTOCOL_FILE, FILE_RESIZE,
      &request, sizeof(request), NULL, 0, deadline_ns, &packet);
  *delivery = packet.delivery;
  if (status != CALL_OK) {
    return packet.delivery == ENDPOINT_DELIVERED ? CALL_OUTCOME_UNKNOWN : status;
  }
  bool malformed = packet.result >= CALL_STATUS_COUNT || packet.size || packet.grant_count;
  for (size_t i = 0; i < packet.grant_count; ++i) {
    handle_close(packet.grants[i].handle);
  }
  return malformed ? CALL_OUTCOME_UNKNOWN : (enum call_status)packet.result;
}

static enum call_status finish_file(handle_t resource, uint64_t intent, uint64_t flags,
    uint64_t deadline_ns, struct resource_result *result)
{
  enum call_status status = resource_result_adopt(resource, result);
  if (status != CALL_OK) {
    handle_close(resource);
    return status;
  }
  if (result->info.protocol != PROTOCOL_FILE) {
    resource_result_close(result);
    return CALL_BAD_OPERATION;
  }
  uint64_t rights = ((intent & PROVIDER_INTENT_READ) ? FILE_RIGHT_READ : 0) |
      ((intent & PROVIDER_INTENT_WRITE) ? FILE_RIGHT_WRITE : 0);
  if ((result->info.rights & rights) != rights ||
      (result->info.kind == HANDLE_KIND_EXPORTED &&
      (result->info.transport & HANDLE_TRANSPORT_CALL) != HANDLE_TRANSPORT_CALL)) {
    resource_result_close(result);
    return CALL_DENIED;
  }
  if (result->info.kind == HANDLE_KIND_NATIVE && result->info.transport) {
    resource_result_close(result);
    return CALL_BAD_REQUEST;
  }
  /* APPEND remains consumer intent over positioned FILE operations. This copy
   * establishes no shared offset or atomic append guarantee. */
  if ((flags & PROVIDER_OPEN_TRUNCATE) && result->info.description.outcome == RESOURCE_OK) {
    uint64_t delivery = 0;
    status = truncate_file(resource, &result->info, deadline_ns, &delivery);
    result->delivery = delivery;
    if (status != CALL_OK) {
      resource_result_close(result);
      result->delivery = delivery;
      return status;
    }
    result->info.description.flags |= RESOURCE_LENGTH_KNOWN;
    result->info.description.length = 0;
  }
  return CALL_OK;
}

static enum call_status open_directory(const handle_t *initial, size_t initial_count,
    const char *remainder,
    uint64_t intent, uint64_t flags, uint64_t deadline_ns,
    struct resource_result *result)
{
  size_t length = strlen(remainder);
  if (!initial_count || length == SIZE_MAX || initial_count > SIZE_MAX - length ||
      initial_count + length > SIZE_MAX / sizeof(handle_t)) {
    return CALL_LIMIT;
  }
  handle_t *chain = malloc((initial_count + length) * sizeof(*chain));
  char *component = malloc(length + 1);
  if (!chain || !component) {
    free(component);
    free(chain);
    return CALL_NO_MEMORY;
  }
  size_t count = 0;
  enum call_status status = CALL_OK;
  while (count < initial_count) {
    status = handle_copy(initial[count], &chain[count]);
    if (status != CALL_OK) {
      goto done;
    }
    ++count;
  }
  const char *path = remainder;
  uint64_t file_rights = ((intent & PROVIDER_INTENT_READ) ? FILE_RIGHT_READ : 0) |
      ((intent & PROVIDER_INTENT_WRITE) ? FILE_RIGHT_WRITE : 0);
  for (;;) {
    while (*path == '/') {
      ++path;
    }
    if (!*path) {
      if (flags) {
        status = CALL_BAD_OPERATION;
      } else {
        status = resource_result_adopt(chain[count - 1], result);
        if (status == CALL_OK) {
          --count;
        }
      }
      break;
    }
    const char *start = path;
    while (*path && *path != '/') {
      ++path;
    }
    size_t size = path - start;
    bool separator = *path != 0;
    while (*path == '/') {
      ++path;
    }
    if (size == 1 && start[0] == '.') {
      continue;
    }
    if (size == 2 && start[0] == '.' && start[1] == '.') {
      if (count == 1) {
        status = CALL_DENIED;
        break;
      }
      handle_close(chain[--count]);
      continue;
    }
    memcpy(component, start, size);
    component[size] = 0;
    handle_t parent = chain[count - 1];
    struct handle_info parent_info;
    status = handle_query(parent, &parent_info);
    if (status != CALL_OK) {
      break;
    }
    if (*path) {
      status = directory_lookup(parent, component, DIRECTORY_KIND_DIRECTORY,
          parent_info.rights & DIRECTORY_RIGHTS, &chain[count]);
      if (status != CALL_OK) {
        break;
      }
      ++count;
      continue;
    }
    if (separator && flags) {
      status = CALL_BAD_OPERATION;
      break;
    }
    if ((flags & PROVIDER_OPEN_CREATE) && !(parent_info.rights & DIRECTORY_RIGHT_CREATE)) {
      status = CALL_DENIED;
      break;
    }
    struct directory_typed_lookup_reply child = {0};
    if (flags & PROVIDER_OPEN_EXCLUSIVE) {
      status = directory_create(parent, component, DIRECTORY_KIND_FILE, file_rights,
          &child.handle);
      child.protocol = PROTOCOL_FILE;
    } else {
      status = directory_lookup_typed(parent, component, file_rights,
          parent_info.rights & DIRECTORY_RIGHTS, &child);
      if (status == CALL_NOT_FOUND && (flags & PROVIDER_OPEN_CREATE)) {
        status = directory_create(parent, component, DIRECTORY_KIND_FILE, file_rights,
            &child.handle);
        child.protocol = PROTOCOL_FILE;
        /* A concurrent creator's ALREADY_EXISTS is final; do not re-open. */
      }
    }
    if (status != CALL_OK) {
      break;
    }
    if (child.protocol == PROTOCOL_FILE) {
      if (separator) {
        handle_close(child.handle);
        status = CALL_WRONG_TYPE;
      } else {
        status = finish_file(child.handle, intent, flags, deadline_ns, result);
      }
    } else if (flags) {
      handle_close(child.handle);
      status = CALL_BAD_OPERATION;
    } else {
      status = resource_result_adopt(child.handle, result);
      if (status != CALL_OK) {
        handle_close(child.handle);
      }
    }
    break;
  }
done:
  while (count) {
    handle_close(chain[--count]);
  }
  free(component);
  free(chain);
  return status;
}

enum call_status resource_open_held(handle_t target, const char *remainder,
    uint64_t intent, uint64_t flags, uint64_t deadline_ns,
    struct resource_result *result)
{
  if (!result) {
    return CALL_BAD_REQUEST;
  }
  *result = (struct resource_result){0};
  if (!remainder || !intent || (intent & ~PROVIDER_INTENTS) ||
      (flags & ~PROVIDER_OPEN_FLAGS) ||
      ((flags & PROVIDER_OPEN_EXCLUSIVE) && !(flags & PROVIDER_OPEN_CREATE)) ||
      ((flags & (PROVIDER_OPEN_TRUNCATE | PROVIDER_OPEN_APPEND)) &&
      !(intent & PROVIDER_INTENT_WRITE))) {
    return CALL_BAD_REQUEST;
  }
  struct handle_info info;
  enum call_status status = handle_query(target, &info);
  if (status != CALL_OK) {
    return status;
  }
  if (info.protocol == PROTOCOL_PROVIDER) {
    return provider_open_typed(target, remainder, intent, flags, deadline_ns, result);
  }
  if (info.protocol == PROTOCOL_DIRECTORY) {
    return info.kind == HANDLE_KIND_NATIVE && !info.transport ?
        open_directory(&target, 1, remainder, intent, flags, deadline_ns, result) :
        CALL_BAD_OPERATION;
  }
  if (info.protocol != PROTOCOL_FILE) {
    if (*remainder || flags) {
      return CALL_BAD_OPERATION;
    }
    uint64_t rights = 0;
    if (info.protocol == PROTOCOL_TRANSACTION && info.kind == HANDLE_KIND_EXPORTED) {
      if (!(intent & PROVIDER_INTENT_WRITE)) {
        return CALL_BAD_OPERATION;
      }
      rights = TRANSACTION_RIGHT_STAGE | TRANSACTION_RIGHT_COMMIT;
      if ((info.rights & rights) != rights) {
        return CALL_DENIED;
      }
      /* ABORT is independently delegated; preserve only authority already held. */
      rights = info.rights;
    } else if (info.protocol == PROTOCOL_STREAM && info.kind == HANDLE_KIND_EXPORTED) {
      if (intent & PROVIDER_INTENT_WRITE) {
        return CALL_BAD_OPERATION;
      }
      rights = STREAM_RIGHT_READ;
    } else if ((info.protocol == PROTOCOL_PIPE || info.protocol == PROTOCOL_CONSOLE) &&
        info.kind == HANDLE_KIND_NATIVE) {
      uint64_t read = info.protocol == PROTOCOL_PIPE ? PIPE_RIGHT_READ : CONSOLE_RIGHT_READ;
      uint64_t write = info.protocol == PROTOCOL_PIPE ? PIPE_RIGHT_WRITE : CONSOLE_RIGHT_WRITE;
      rights = ((intent & PROVIDER_INTENT_READ) ? read : 0) |
          ((intent & PROVIDER_INTENT_WRITE) ? write : 0);
    } else {
      return CALL_BAD_OPERATION;
    }
    uint64_t transport = info.kind == HANDLE_KIND_EXPORTED ? HANDLE_TRANSPORT_CALL : 0;
    if ((info.rights & rights) != rights || (info.transport & transport) != transport) {
      return CALL_DENIED;
    }
    handle_t resource = HANDLE_INVALID;
    status = handle_copy_restricted(target, rights, transport, &resource);
    if (status == CALL_OK) {
      status = resource_result_adopt(resource, result);
      if (status != CALL_OK) {
        handle_close(resource);
      }
    }
    return status;
  }
  if (*remainder || (flags & (PROVIDER_OPEN_CREATE | PROVIDER_OPEN_EXCLUSIVE))) {
    return CALL_BAD_OPERATION;
  }
  uint64_t rights = ((intent & PROVIDER_INTENT_READ) ? FILE_RIGHT_READ : 0) |
      ((intent & PROVIDER_INTENT_WRITE) ? FILE_RIGHT_WRITE : 0);
  uint64_t transport = info.kind == HANDLE_KIND_EXPORTED ? HANDLE_TRANSPORT_CALL : 0;
  if ((info.rights & rights) != rights || (info.transport & transport) != transport) {
    return CALL_DENIED;
  }
  handle_t resource = HANDLE_INVALID;
  status = handle_copy_restricted(target, rights, transport, &resource);
  if (status != CALL_OK) {
    return status;
  }
  return finish_file(resource, intent, flags, deadline_ns, result);
}

enum call_status resource_open_path(const struct path_context *context, const char *path,
    uint64_t intent, uint64_t flags, uint64_t deadline_ns,
    struct resource_result *result)
{
  if (!result) {
    return CALL_BAD_REQUEST;
  }
  *result = (struct resource_result){0};
  if (!path || !*path || *path == '/' || !intent || (intent & ~PROVIDER_INTENTS) ||
      (flags & ~PROVIDER_OPEN_FLAGS) ||
      ((flags & PROVIDER_OPEN_EXCLUSIVE) && !(flags & PROVIDER_OPEN_CREATE)) ||
      ((flags & (PROVIDER_OPEN_TRUNCATE | PROVIDER_OPEN_APPEND)) &&
      !(intent & PROVIDER_INTENT_WRITE))) {
    return CALL_BAD_REQUEST;
  }
  const char *end = path;
  while (*end && *end != '/' && *end != ':') {
    ++end;
  }
  if (*end == ':' && end[1] == '/' && end[2] == '/') {
    size_t length = end - path;
    if (!length || length > ENVIRONMENT_NAME_MAX) {
      return CALL_BAD_REQUEST;
    }
    char name[ENVIRONMENT_NAME_MAX + 1];
    memcpy(name, path, length);
    name[length] = 0;
    handle_t target = HANDLE_INVALID;
    enum call_status status = path_root(name, &target);
    if (status != CALL_OK) {
      return status;
    }
    status = resource_open_held(target, end + 3, intent, flags, deadline_ns, result);
    handle_close(target);
    return status;
  }
  if (!context || !context->count) {
    return CALL_UNAVAILABLE;
  }
  return open_directory(context->directories, context->count, path, intent, flags,
      deadline_ns, result);
}
