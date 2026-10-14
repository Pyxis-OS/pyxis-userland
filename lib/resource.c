#include <resource.h>
#include <endpoint.h>
#include <file.h>
#include <handle.h>
#include <provider.h>
#include <stddef.h>

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
  if (info.protocol != PROTOCOL_FILE) {
    return CALL_BAD_OPERATION;
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
  status = resource_result_adopt(resource, result);
  if (status != CALL_OK) {
    handle_close(resource);
    return status;
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
