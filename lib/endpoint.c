#include <endpoint.h>
#include <syscall.h>

static enum call_status exchange_packet(handle_t endpoint,
    const struct endpoint_message *message, struct endpoint_packet *result)
{
  if (!result) {
    return CALL_BAD_REQUEST;
  }
  *result = (struct endpoint_packet){0};
  struct endpoint_packet reply;
  struct syscall_result status = syscall_call(endpoint, message, sizeof(*message),
      &reply, sizeof(reply));
  if (status.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  if (status.status != CALL_OK) {
    return status.reply_size == 0 ? status.status : CALL_BAD_REQUEST;
  }
  if (status.reply_size != sizeof(reply) || !reply.id || reply.size > ENDPOINT_DATA_MAX) {
    return CALL_BAD_REQUEST;
  }
  *result = reply;
  return CALL_OK;
}

enum call_status endpoint_request(handle_t endpoint, const void *bytes, size_t size,
                                  const struct endpoint_grant *grant,
                                  struct endpoint_packet *reply)
{
  if (!reply || size > ENDPOINT_DATA_MAX || (size && !bytes)) {
    if (reply) {
      *reply = (struct endpoint_packet){0};
    }
    return CALL_BAD_REQUEST;
  }
  struct endpoint_message message = {
    .header = {PROTOCOL_ENDPOINT, ENDPOINT_CALL},
    .body.call.size = size,
  };
  /* Stage inputs before exchange_packet clears the reply: a caller may
   * forward data/grant fields from the packet it also uses for the reply. */
  if (grant) {
    message.body.call.grant = *grant;
  }
  const uint8_t *source = bytes;
  for (size_t i = 0; i < size; ++i) {
    message.body.call.data[i] = source[i];
  }
  return exchange_packet(endpoint, &message, reply);
}

enum call_status endpoint_receive(handle_t endpoint, struct endpoint_packet *request)
{
  struct endpoint_message message = {.header = {PROTOCOL_ENDPOINT, ENDPOINT_RECEIVE}};
  return exchange_packet(endpoint, &message, request);
}

enum call_status endpoint_reply(handle_t endpoint, uint64_t id,
                                const void *bytes, size_t size)
{
  if (!id || size > ENDPOINT_DATA_MAX || (size && !bytes)) {
    return CALL_BAD_REQUEST;
  }
  struct endpoint_message message = {
    .header = {PROTOCOL_ENDPOINT, ENDPOINT_REPLY},
    .body.reply.id = id,
    .body.reply.size = size,
  };
  const uint8_t *source = bytes;
  for (size_t i = 0; i < size; ++i) {
    message.body.reply.data[i] = source[i];
  }
  struct syscall_result result = syscall_call(endpoint, &message, sizeof(message), NULL, 0);
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  return result.reply_size == 0 ? result.status : CALL_BAD_REQUEST;
}
