#include <endpoint.h>
#include <handle.h>
#include <provider.h>
#include <string.h>

struct provider_open_payload {
  struct provider_open_request body;
  unsigned char target[PROVIDER_TARGET_MAX_BYTES];
};

_Static_assert(offsetof(struct provider_open_payload, target) ==
    sizeof(struct provider_open_request), "provider target follows the request");

static enum call_status capture_reply(struct endpoint_packet *packet,
    const struct provider_open_reply *reply, struct resource_result *result)
{
  if (!resource_description_valid(&reply->description) ||
      packet->grant_count != (reply->protocol ? 1u : 0u) ||
      (!reply->protocol && (reply->description.outcome == RESOURCE_OK ||
      (reply->description.flags & RESOURCE_ERROR_BODY)))) {
    return CALL_BAD_REQUEST;
  }
  if (!reply->protocol) {
    result->info.description = reply->description;
    return CALL_OK;
  }
  const struct endpoint_grant *grant = &packet->grants[0];
  struct handle_info queried;
  enum call_status status = handle_query(grant->handle, &queried);
  if (status != CALL_OK) {
    return status;
  }
  if (queried.protocol != reply->protocol || queried.rights != grant->rights ||
      queried.transport != grant->transport) {
    return CALL_BAD_REQUEST;
  }
  uint64_t flags = (reply->description.flags & RESOURCE_ERROR_BODY) ?
      DESCRIBE_INCLUDE_ERROR_BODY : 0;
  struct describe_reply info;
  status = handle_describe(grant->handle, flags, &info);
  if (status != CALL_OK) {
    return status;
  }
  if (info.protocol != queried.protocol || info.kind != queried.kind ||
      info.rights != queried.rights || info.transport != queried.transport ||
      info.description.outcome != reply->description.outcome ||
      ((reply->description.flags & RESOURCE_ERROR_BODY) &&
      info.error_body == HANDLE_INVALID)) {
    if (info.error_body != HANDLE_INVALID) {
      handle_close(info.error_body);
    }
    return CALL_BAD_REQUEST;
  }
  info.description = reply->description;
  result->resource = grant->handle;
  result->info = info;
  packet->grants[0].handle = HANDLE_INVALID;
  return CALL_OK;
}

enum call_status provider_open_typed(handle_t provider, const char *target,
    uint64_t intent, uint64_t flags, uint64_t deadline_ns,
    struct resource_result *result)
{
  if (!result) {
    return CALL_BAD_REQUEST;
  }
  *result = (struct resource_result){0};
  if (!target || !intent || (intent & ~PROVIDER_INTENTS) ||
      (flags & ~PROVIDER_OPEN_FLAGS) ||
      ((flags & PROVIDER_OPEN_EXCLUSIVE) && !(flags & PROVIDER_OPEN_CREATE)) ||
      ((flags & (PROVIDER_OPEN_TRUNCATE | PROVIDER_OPEN_APPEND)) &&
      !(intent & PROVIDER_INTENT_WRITE))) {
    return CALL_BAD_REQUEST;
  }
  size_t target_size = 0;
  while (target[target_size]) {
    if (target_size == PROVIDER_TARGET_MAX_BYTES) {
      return CALL_FILE_TOO_LARGE;
    }
    ++target_size;
  }
  struct handle_info info;
  enum call_status status = handle_query(provider, &info);
  if (status != CALL_OK) {
    return status;
  }
  if (info.kind != HANDLE_KIND_EXPORTED || info.protocol != PROTOCOL_PROVIDER) {
    return CALL_WRONG_TYPE;
  }
  uint64_t required = 0;
  if (intent & PROVIDER_INTENT_READ) {
    required |= PROVIDER_RIGHT_OPEN_READ;
  }
  if ((intent & PROVIDER_INTENT_WRITE) || (flags & PROVIDER_OPEN_CREATE)) {
    required |= PROVIDER_RIGHT_OPEN_WRITE;
  }
  if ((info.rights & required) != required ||
      (info.transport & HANDLE_TRANSPORT_CALL) != HANDLE_TRANSPORT_CALL) {
    return CALL_DENIED;
  }
  struct provider_open_payload request = {
    .body = {intent, flags, target_size, deadline_ns},
  };
  memcpy(request.target, target, target_size);
  struct endpoint_packet packet = {0};
  status = endpoint_invoke(provider, PROTOCOL_PROVIDER, PROVIDER_OPEN,
      &request, sizeof(request.body) + target_size, NULL, 0, deadline_ns, &packet);
  result->delivery = packet.delivery;
  if (status != CALL_OK) {
    return status;
  }
  if (packet.result >= CALL_STATUS_COUNT) {
    status = CALL_BAD_REQUEST;
  } else if (packet.result != CALL_OK) {
    status = packet.size || packet.grant_count ? CALL_BAD_REQUEST :
        (enum call_status)packet.result;
  } else if (packet.size != sizeof(struct provider_open_reply)) {
    status = CALL_BAD_REQUEST;
  } else {
    struct provider_open_reply reply;
    memcpy(&reply, packet.data, sizeof(reply));
    status = capture_reply(&packet, &reply, result);
  }
  for (size_t i = 0; i < packet.grant_count; ++i) {
    if (packet.grants[i].handle != HANDLE_INVALID) {
      handle_close(packet.grants[i].handle);
    }
  }
  return status;
}

enum call_status provider_open(handle_t provider, const char *target, uint64_t intent,
    uint64_t flags, uint64_t deadline_ns, struct provider_result *result, handle_t *file)
{
  if (file) {
    *file = HANDLE_INVALID;
  }
  if (result) {
    *result = (struct provider_result){0};
  }
  if (!result || !file) {
    return CALL_BAD_REQUEST;
  }
  struct resource_result typed;
  enum call_status status = provider_open_typed(provider, target, intent, flags,
      deadline_ns, &typed);
  result->delivery = typed.delivery;
  if (status != CALL_OK) {
    return status;
  }
  result->status = resource_outcome_status(typed.info.description.outcome);
  if (result->status == CALL_OK) {
    uint64_t rights = ((intent & PROVIDER_INTENT_READ) ? FILE_RIGHT_READ : 0) |
        ((intent & PROVIDER_INTENT_WRITE) ? FILE_RIGHT_WRITE : 0);
    if (typed.info.protocol != PROTOCOL_FILE) {
      status = CALL_BAD_OPERATION;
    } else if ((typed.info.rights & rights) != rights ||
        (typed.info.kind == HANDLE_KIND_EXPORTED &&
        (typed.info.transport & HANDLE_TRANSPORT_CALL) != HANDLE_TRANSPORT_CALL)) {
      status = CALL_DENIED;
    } else if (typed.info.kind == HANDLE_KIND_NATIVE && typed.info.transport) {
      status = CALL_BAD_REQUEST;
    } else {
      *file = typed.resource;
      typed.resource = HANDLE_INVALID;
      result->metadata.protocol = typed.info.protocol;
      result->metadata.media_type_size = typed.info.description.media_type_size;
      memcpy(result->metadata.media_type, typed.info.description.media_type,
          typed.info.description.media_type_size + 1);
    }
  }
  resource_result_close(&typed);
  return status;
}
