#include <abi/file.h>
#include <endpoint.h>
#include <handle.h>
#include <provider.h>
#include <string.h>

struct provider_open_payload {
  struct provider_open_request body;
  unsigned char uri[PROVIDER_URI_MAX_BYTES];
};

_Static_assert(offsetof(struct provider_open_payload, uri) ==
    sizeof(struct provider_open_request), "provider URI follows the request");

enum call_status provider_open(handle_t provider, const char *uri, uint64_t rights,
    struct provider_metadata *metadata, handle_t *file)
{
  if (!file) {
    return CALL_BAD_REQUEST;
  }
  *file = HANDLE_INVALID;
  if (metadata) {
    *metadata = (struct provider_metadata){0};
  }
  if (!uri || !*uri || !rights || (rights & ~FILE_RIGHTS)) {
    return CALL_BAD_REQUEST;
  }
  size_t uri_size = 0;
  while (uri[uri_size]) {
    if (uri_size == PROVIDER_URI_MAX_BYTES) {
      return CALL_LIMIT;
    }
    ++uri_size;
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
  if (rights & FILE_RIGHT_READ) {
    required |= PROVIDER_RIGHT_OPEN_READ;
  }
  if (rights & FILE_RIGHT_WRITE) {
    required |= PROVIDER_RIGHT_OPEN_WRITE;
  }
  if ((info.rights & required) != required ||
      (info.transport & HANDLE_TRANSPORT_CALL) != HANDLE_TRANSPORT_CALL) {
    return CALL_DENIED;
  }
  struct provider_open_payload request = {.body = {rights, uri_size}};
  memcpy(request.uri, uri, uri_size);
  struct endpoint_packet packet;
  status = endpoint_invoke(provider, PROTOCOL_PROVIDER, PROVIDER_OPEN,
      &request, sizeof(request.body) + uri_size, NULL, 0, 0, &packet);
  if (status != CALL_OK) {
    return status;
  }

  struct provider_open_reply reply = {0};
  if (packet.result >= CALL_STATUS_COUNT) {
    status = CALL_BAD_REQUEST;
  } else if (packet.result != CALL_OK) {
    status = packet.size || packet.grant_count ? CALL_BAD_REQUEST :
        (enum call_status)packet.result;
  } else if (packet.size < sizeof(reply) || packet.grant_count != 1) {
    status = CALL_BAD_REQUEST;
  } else {
    memcpy(&reply, packet.data, sizeof(reply));
    struct endpoint_grant *grant = &packet.grants[0];
    if (reply.protocol != PROTOCOL_FILE ||
        reply.representation != PROVIDER_REPRESENTATION_BYTES ||
        reply.media_type_size > PROVIDER_MEDIA_TYPE_MAX_BYTES ||
        packet.size != sizeof(reply) + reply.media_type_size ||
        grant->rights != rights || grant->transport != HANDLE_TRANSPORT_CALL ||
        handle_query(grant->handle, &info) != CALL_OK ||
        info.kind != HANDLE_KIND_EXPORTED || info.protocol != reply.protocol ||
        info.rights != rights || info.transport != HANDLE_TRANSPORT_CALL) {
      status = CALL_BAD_REQUEST;
    } else {
      for (size_t i = 0; i < reply.media_type_size; ++i) {
        unsigned char byte = packet.data[sizeof(reply) + i];
        if (byte < 0x20 || byte > 0x7e) {
          status = CALL_BAD_REQUEST;
          break;
        }
      }
    }
  }
  if (status == CALL_OK) {
    *file = packet.grants[0].handle;
    packet.grants[0].handle = HANDLE_INVALID;
    if (metadata) {
      metadata->protocol = reply.protocol;
      metadata->representation = reply.representation;
      metadata->media_type_size = reply.media_type_size;
      memcpy(metadata->media_type, packet.data + sizeof(reply), reply.media_type_size);
      metadata->media_type[reply.media_type_size] = '\0';
    }
  }
  for (size_t i = 0; i < packet.grant_count; ++i) {
    if (packet.grants[i].handle != HANDLE_INVALID) {
      handle_close(packet.grants[i].handle);
    }
  }
  return status;
}
