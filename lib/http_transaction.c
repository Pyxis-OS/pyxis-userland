#include <http_transaction.h>
#include <endpoint.h>
#include <handle.h>
#include <http_url.h>
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

enum call_status http_transaction_prepare_context(handle_t provider, const char *uri,
    uint64_t method, uint64_t flags, const char *headers, size_t headers_size,
    uint64_t deadline_ns, const struct http_prepare_context *context,
    struct http_prepared *prepared)
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
  if (context) {
    request.context = *context;
  } else {
    struct http_uri parsed;
    const char *endpoint = !strncmp(uri, "json+", 5) ? uri + 5 : uri;
    enum call_status status = http_url_parse(endpoint, &parsed);
    if (status != CALL_OK) {
      return status;
    }
    request.context.origin.scheme = parsed.scheme;
    request.context.origin.port = parsed.port;
    memcpy(request.context.origin.host, parsed.host, strlen(parsed.host) + 1);
    request.context.remaining = (struct http_framing_budget){
      HTTP_HEADERS_MAX, HTTP_FIELDS_MAX, HTTP_INFORMATIONAL_MAX};
  }
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
    if (response.method != method || (response.trust_flags & ~HTTP_TRUST_ORIGIN_ONLY) ||
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
      prepared->trust_flags = response.trust_flags;
      return CALL_OK;
    }
  }
  close_grants(&packet);
  return status;
}

enum call_status http_transaction_prepare(handle_t provider, const char *uri,
    uint64_t method, uint64_t flags, const char *headers, size_t headers_size,
    uint64_t deadline_ns, struct http_prepared *prepared)
{
  return http_transaction_prepare_context(provider, uri, method, flags, headers,
      headers_size, deadline_ns, NULL, prepared);
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
        if (prepared->observation == HANDLE_INVALID) {
          prepared->observation = packet.grants[1].handle;
        } else {
          handle_close(packet.grants[1].handle);
        }
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
      response.cleanup_status >= CALL_STATUS_COUNT ||
      response.framing > HTTP_FRAMING_FAILED || response.http_status > 999 ||
      (response.http_status && response.http_status < 100) ||
      response.tls_native_status >= CALL_STATUS_COUNT || response.tls_cleanup_attempted > 1 ||
      (response.response_flags & ~HTTP_STATUS_INVALID_LOCATION) ||
      response.charged.header_bytes > HTTP_HEADERS_MAX ||
      response.charged.fields > HTTP_FIELDS_MAX ||
      response.charged.informational > HTTP_INFORMATIONAL_MAX ||
      !memchr(response.media_type, 0, sizeof(response.media_type)) ||
      !memchr(response.location, 0, sizeof(response.location))) {
    return CALL_BAD_REQUEST;
  }
  *status = response;
  return CALL_OK;
}

enum call_status http_transaction_metadata(handle_t observation, uint64_t deadline_ns,
    struct http_response_metadata *metadata)
{
  if (!metadata) {
    return CALL_BAD_REQUEST;
  }
  *metadata = (struct http_response_metadata){0};
  struct endpoint_packet packet = {0};
  enum call_status status = endpoint_invoke(observation, PROTOCOL_HTTP_TRANSACTION,
      HTTP_TRANSACTION_METADATA, NULL, 0, NULL, 0, deadline_ns, &packet);
  close_grants(&packet);
  if (status != CALL_OK) {
    return status;
  }
  if (packet.grant_count || packet.result >= CALL_STATUS_COUNT ||
      (packet.result == CALL_OK && packet.size != sizeof(*metadata))) {
    return CALL_BAD_REQUEST;
  }
  if (packet.result != CALL_OK) {
    return packet.size ? CALL_BAD_REQUEST : packet.result;
  }
  struct http_response_metadata response;
  memcpy(&response, packet.data, sizeof(response));
  struct http_uri parsed;
  if (!memchr(response.uri, 0, sizeof(response.uri)) ||
      !memchr(response.media_type, 0, sizeof(response.media_type)) ||
      http_url_parse(response.uri, &parsed) != CALL_OK) {
    return CALL_BAD_REQUEST;
  }
  *metadata = response;
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
