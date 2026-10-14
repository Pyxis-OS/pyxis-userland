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

enum call_status provider_open(handle_t provider, const char *target, uint64_t intent,
    uint64_t flags, uint64_t deadline_ns, struct provider_result *result, handle_t *file)
{
  if (file) {
    *file = HANDLE_INVALID;
  }
  if (result) {
    *result = (struct provider_result){0};
  }
  if (!file || !result || !target || !intent || (intent & ~PROVIDER_INTENTS) ||
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
  uint64_t rights = 0;
  if (intent & PROVIDER_INTENT_READ) {
    required |= PROVIDER_RIGHT_OPEN_READ;
    rights |= FILE_RIGHT_READ;
  }
  if (intent & PROVIDER_INTENT_WRITE) {
    required |= PROVIDER_RIGHT_OPEN_WRITE;
    rights |= FILE_RIGHT_WRITE;
  }
  if (flags & PROVIDER_OPEN_CREATE) {
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

  struct provider_open_reply reply = {0};
  if (packet.result >= CALL_STATUS_COUNT || packet.size < sizeof(reply)) {
    status = CALL_BAD_REQUEST;
  } else {
    memcpy(&reply, packet.data, sizeof(reply));
    if (packet.result != CALL_OK) {
      if (packet.size != sizeof(reply) || packet.grant_count ||
          reply.protocol || reply.representation || reply.media_type_size) {
        status = CALL_BAD_REQUEST;
      }
    } else if (packet.grant_count != 1 || reply.protocol != PROTOCOL_FILE ||
        reply.representation != PROVIDER_REPRESENTATION_BYTES ||
        reply.media_type_size > PROVIDER_MEDIA_TYPE_MAX_BYTES ||
        packet.size != sizeof(reply) + reply.media_type_size ||
        packet.grants[0].rights != rights ||
        packet.grants[0].transport != HANDLE_TRANSPORT_CALL) {
      status = CALL_BAD_REQUEST;
    } else {
      status = handle_query(packet.grants[0].handle, &info);
      if (status == CALL_OK && (info.kind != HANDLE_KIND_EXPORTED ||
          info.protocol != reply.protocol || info.rights != rights ||
          info.transport != HANDLE_TRANSPORT_CALL)) {
        status = CALL_BAD_REQUEST;
      }
      for (size_t i = 0; status == CALL_OK && i < reply.media_type_size; ++i) {
        unsigned char byte = packet.data[sizeof(reply) + i];
        if (byte < 0x20 || byte > 0x7e) {
          status = CALL_BAD_REQUEST;
        }
      }
    }
  }
  if (status == CALL_OK) {
    result->status = packet.result;
    if (result->status == CALL_OK) {
      *file = packet.grants[0].handle;
      packet.grants[0].handle = HANDLE_INVALID;
      result->metadata.protocol = reply.protocol;
      result->metadata.representation = reply.representation;
      result->metadata.media_type_size = reply.media_type_size;
      memcpy(result->metadata.media_type, packet.data + sizeof(reply), reply.media_type_size);
      result->metadata.media_type[reply.media_type_size] = '\0';
    }
  }
  for (size_t i = 0; i < packet.grant_count; ++i) {
    if (packet.grants[i].handle != HANDLE_INVALID) {
      handle_close(packet.grants[i].handle);
    }
  }
  return status;
}
