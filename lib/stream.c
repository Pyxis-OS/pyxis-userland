#include <stream.h>
#include <endpoint.h>
#include <handle.h>
#include <string.h>

enum call_status stream_read(handle_t stream, void *bytes, size_t capacity,
    uint64_t deadline_ns, size_t *read)
{
  if (!read) {
    return CALL_BAD_REQUEST;
  }
  *read = 0;
  if (!capacity || capacity > STREAM_READ_MAX_BYTES || !bytes) {
    return CALL_BAD_REQUEST;
  }
  struct handle_info info;
  enum call_status status = handle_query(stream, &info);
  if (status != CALL_OK) {
    return status;
  }
  if (info.kind != HANDLE_KIND_EXPORTED || info.protocol != PROTOCOL_STREAM) {
    return CALL_WRONG_TYPE;
  }
  if (!(info.rights & STREAM_RIGHT_READ) || info.transport != HANDLE_TRANSPORT_CALL) {
    return CALL_DENIED;
  }
  struct stream_read_request request = {capacity};
  struct endpoint_packet packet = {0};
  status = endpoint_invoke(stream, PROTOCOL_STREAM, STREAM_READ, &request, sizeof(request),
      NULL, 0, deadline_ns, &packet);
  for (size_t i = 0; i < packet.grant_count; ++i) {
    handle_close(packet.grants[i].handle);
  }
  if (status != CALL_OK) {
    return status;
  }
  if (packet.grant_count || packet.result >= CALL_STATUS_COUNT) {
    return CALL_BAD_REQUEST;
  }
  if (packet.result != CALL_OK) {
    return packet.size ? CALL_BAD_REQUEST : packet.result;
  }
  struct stream_read_reply response;
  if (packet.size < sizeof(response)) {
    return CALL_BAD_REQUEST;
  }
  memcpy(&response, packet.data, sizeof(response));
  if (response.read > capacity || packet.size != sizeof(response) + response.read) {
    return CALL_BAD_REQUEST;
  }
  if (response.read) {
    memcpy(bytes, packet.data + sizeof(response), response.read);
  }
  *read = response.read;
  return CALL_OK;
}
