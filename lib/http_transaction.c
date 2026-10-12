#include <http_transaction.h>
#include <endpoint.h>
#include <handle.h>
#include <string.h>

static void close_grants(struct endpoint_packet *packet)
{
  for (size_t i = 0; i < packet->grant_count; ++i) {
    if (packet->grants[i].handle != HANDLE_INVALID) {
      handle_close(packet->grants[i].handle);
    }
  }
}

static bool grant_valid(const struct endpoint_grant *grant, uint64_t protocol,
    uint64_t rights)
{
  struct handle_info info;
  return grant->handle != HANDLE_INVALID && grant->rights == rights &&
      grant->transport == HANDLE_TRANSPORT_CALL &&
      handle_query(grant->handle, &info) == CALL_OK &&
      info.kind == HANDLE_KIND_EXPORTED && info.protocol == protocol &&
      info.rights == rights && info.transport == HANDLE_TRANSPORT_CALL;
}

enum call_status http_transaction_prepare(handle_t provider, const char *uri,
    uint64_t method, uint64_t flags, const char *headers, size_t headers_size,
    uint64_t deadline_ns, struct http_prepared *prepared)
{
  if (!prepared) {
    return CALL_BAD_REQUEST;
  }
  *prepared = (struct http_prepared){0};
  if (!uri || method > HTTP_PREPARE_POST || (flags & ~HTTP_PREPARE_FLAGS) ||
      !deadline_ns || (headers_size && !headers)) {
    return CALL_BAD_REQUEST;
  }
  size_t length = strnlen(uri, PROVIDER_HTTP_URI_MAX_BYTES + 6);
  if (!length || length > PROVIDER_HTTP_URI_MAX_BYTES + 5 ||
      length > ENDPOINT_DATA_MAX - sizeof(struct http_prepare_request) ||
      headers_size > ENDPOINT_DATA_MAX - sizeof(struct http_prepare_request) - length) {
    return CALL_FILE_TOO_LARGE;
  }
  uint8_t bytes[ENDPOINT_DATA_MAX];
  struct http_prepare_request request = {
    .method = method, .flags = flags, .deadline_ns = deadline_ns,
    .uri_size = length, .headers_size = headers_size,
  };
  memcpy(bytes, &request, sizeof(request));
  memcpy(bytes + sizeof(request), uri, length);
  if (headers_size) {
    memcpy(bytes + sizeof(request) + length, headers, headers_size);
  }
  struct endpoint_packet packet = {0};
  enum call_status status = endpoint_invoke(provider, PROTOCOL_PROVIDER, PROVIDER_PREPARE,
      bytes, sizeof(request) + length + headers_size, NULL, 0, deadline_ns, &packet);
  volatile uint8_t *erase = bytes;
  for (size_t i = 0; i < sizeof(request) + length + headers_size; ++i) {
    erase[i] = 0;
  }
  prepared->delivery = packet.delivery;
  if (status != CALL_OK) {
    close_grants(&packet);
    return status;
  }
  bool post = method == HTTP_PREPARE_POST;
  struct http_prepare_reply response;
  if (packet.result >= CALL_STATUS_COUNT) {
    status = CALL_BAD_REQUEST;
  } else if (packet.result != CALL_OK) {
    status = packet.grant_count || packet.size ? CALL_BAD_REQUEST : packet.result;
  } else if (packet.size != sizeof(response) || packet.grant_count != (post ? 3u : 2u)) {
    status = CALL_BAD_REQUEST;
  } else {
    memcpy(&response, packet.data, sizeof(response));
    if (response.method != method ||
        !grant_valid(&packet.grants[0], PROTOCOL_HTTP_TRANSACTION, HTTP_TRANSACTION_CONTROL_RIGHTS) ||
        (post && !grant_valid(&packet.grants[1], PROTOCOL_FILE, FILE_RIGHT_WRITE)) ||
        !grant_valid(&packet.grants[post ? 2 : 1], PROTOCOL_HTTP_TRANSACTION,
            HTTP_TRANSACTION_RIGHT_STATUS)) {
      status = CALL_BAD_REQUEST;
    } else {
      prepared->control = packet.grants[0].handle;
      prepared->body = post ? packet.grants[1].handle : HANDLE_INVALID;
      prepared->observation = packet.grants[post ? 2 : 1].handle;
      prepared->method = method;
      return CALL_OK;
    }
  }
  close_grants(&packet);
  return status;
}

enum call_status http_transaction_commit(struct http_prepared *prepared,
    uint64_t deadline_ns, struct http_commit_reply *reply)
{
  if (!prepared || !reply) {
    return CALL_BAD_REQUEST;
  }
  *reply = (struct http_commit_reply){0};
  struct endpoint_packet packet = {0};
  enum call_status status = endpoint_invoke(prepared->control, PROTOCOL_HTTP_TRANSACTION,
      HTTP_TRANSACTION_COMMIT, NULL, 0, NULL, 0, deadline_ns, &packet);
  prepared->delivery = packet.delivery;
  if (status != CALL_OK) {
    close_grants(&packet);
    return status;
  }
  struct http_commit_reply response;
  if (packet.result >= CALL_STATUS_COUNT) {
    status = CALL_OUTCOME_UNKNOWN;
  } else if (packet.result != CALL_OK) {
    status = packet.grant_count || packet.size ? CALL_OUTCOME_UNKNOWN : packet.result;
  } else if (packet.size != sizeof(response)) {
    status = CALL_OUTCOME_UNKNOWN;
  } else {
    memcpy(&response, packet.data, sizeof(response));
    bool reader = response.flags == HTTP_COMMIT_RESPONSE;
    if ((response.flags && !reader) || packet.grant_count != (reader ? 2u : 0u) ||
        (reader && (!grant_valid(&packet.grants[0], PROTOCOL_STREAM, STREAM_RIGHT_READ) ||
        !grant_valid(&packet.grants[1], PROTOCOL_HTTP_TRANSACTION, HTTP_TRANSACTION_RIGHT_STATUS)))) {
      status = CALL_OUTCOME_UNKNOWN;
    } else if (reader && prepared->response != HANDLE_INVALID) {
      status = CALL_OUTCOME_UNKNOWN;
    } else {
      if (reader) {
        prepared->response = packet.grants[0].handle;
        handle_close(packet.grants[1].handle);
      }
      *reply = response;
      return CALL_OK;
    }
  }
  close_grants(&packet);
  return status;
}

enum call_status http_transaction_abort(handle_t control, uint64_t deadline_ns)
{
  struct endpoint_packet packet = {0};
  enum call_status status = endpoint_invoke(control, PROTOCOL_HTTP_TRANSACTION,
      HTTP_TRANSACTION_ABORT, NULL, 0, NULL, 0, deadline_ns, &packet);
  close_grants(&packet);
  return status != CALL_OK ? status : packet.grant_count || packet.size ||
      packet.result >= CALL_STATUS_COUNT ? CALL_BAD_REQUEST : packet.result;
}

enum call_status http_transaction_status(handle_t observation, uint64_t deadline_ns,
    struct http_transaction_status *status)
{
  if (!status) {
    return CALL_BAD_REQUEST;
  }
  struct endpoint_packet packet = {0};
  enum call_status result = endpoint_invoke(observation, PROTOCOL_HTTP_TRANSACTION,
      HTTP_TRANSACTION_STATUS, NULL, 0, NULL, 0, deadline_ns, &packet);
  close_grants(&packet);
  if (result != CALL_OK) {
    return result;
  }
  if (packet.grant_count || packet.result >= CALL_STATUS_COUNT ||
      (packet.result == CALL_OK && packet.size != sizeof(*status))) {
    return CALL_BAD_REQUEST;
  }
  if (packet.result != CALL_OK) {
    return packet.size ? CALL_BAD_REQUEST : packet.result;
  }
  struct http_transaction_status response;
  memcpy(&response, packet.data, sizeof(response));
  if (response.phase > HTTP_TRANSACTION_ABORTED || response.submission > HTTP_SUBMISSION_SENT ||
      response.status >= CALL_STATUS_COUNT || response.source_status >= CALL_STATUS_COUNT ||
      response.cleanup_status >= CALL_STATUS_COUNT) {
    return CALL_BAD_REQUEST;
  }
  *status = response;
  return CALL_OK;
}

enum call_status http_prepared_close(struct http_prepared *prepared)
{
  if (!prepared) {
    return CALL_BAD_REQUEST;
  }
  handle_t handles[] = {prepared->control, prepared->body,
    prepared->observation, prepared->response};
  enum call_status status = CALL_OK;
  for (size_t i = 0; i < sizeof(handles) / sizeof(handles[0]); ++i) {
    if (handles[i] != HANDLE_INVALID && handle_close(handles[i])) {
      status = CALL_BAD_HANDLE;
    }
  }
  *prepared = (struct http_prepared){0};
  return status;
}
