#include "internal.h"
#include <clock.h>
#include <stdlib.h>
#include <string.h>

#define HTTP_INITIAL_CAPACITY 4096

struct snapshot {
  struct http_result *result;
  struct http_body body;
  size_t body_limit;
};

static bool fail(struct snapshot *snapshot, enum http_error error)
{
  snapshot->result->error = error;
  return false;
}

void http_body_release(struct http_body *body)
{
  if (body->storage) {
    body->storage->reserved -= body->capacity;
  }
  free(body->data);
  *body = (struct http_body){0};
}

static bool reserve_body(struct snapshot *snapshot, size_t capacity)
{
  struct http_body *body = &snapshot->body;
  if (capacity > snapshot->body_limit) {
    return fail(snapshot, HTTP_LIMIT);
  }
  if (capacity <= body->capacity) {
    return true;
  }
  struct http_storage *storage = body->storage;
  if (storage->reserved > HTTP_STORAGE_MAX || capacity > HTTP_STORAGE_MAX - storage->reserved) {
    return fail(snapshot, HTTP_QUOTA);
  }
  /* Keep the old reservation until replacement storage actually exists. */
  storage->reserved += capacity;
  unsigned char *data = malloc(capacity);
  if (!data) {
    storage->reserved -= capacity;
    return fail(snapshot, HTTP_NO_MEMORY);
  }
  if (body->size) {
    memcpy(data, body->data, body->size);
  }
  free(body->data);
  storage->reserved -= body->capacity;
  body->data = data;
  body->capacity = capacity;
  return true;
}

static bool append_body(struct snapshot *snapshot, const unsigned char *bytes, size_t size)
{
  struct http_body *body = &snapshot->body;
  if (size > snapshot->body_limit - body->size) {
    return fail(snapshot, HTTP_LIMIT);
  }
  size_t needed = body->size + size;
  if (needed > body->capacity) {
    size_t capacity = body->capacity ? body->capacity : HTTP_INITIAL_CAPACITY;
    if (capacity > snapshot->body_limit) {
      capacity = snapshot->body_limit;
    }
    while (capacity < needed) {
      if (capacity > snapshot->body_limit / 2) {
        capacity = snapshot->body_limit;
        break;
      }
      capacity *= 2;
    }
    if (!reserve_body(snapshot, capacity)) {
      return false;
    }
  }
  if (size) {
    memcpy(body->data + body->size, bytes, size);
  }
  body->size += size;
  snapshot->result->consumed.body_bytes = body->size;
  return true;
}

static void response_metadata(struct http_result *result, const struct http_response *response)
{
  uint64_t body_bytes = result->consumed.body_bytes;
  result->consumed = response->consumed;
  result->consumed.body_bytes = body_bytes;
  result->network_status = response->network_status;
  result->status = response->status;
  result->dns_rcode = response->dns_rcode;
  result->tls_failure = response->tls_failure;
  result->tls_cleanup = response->tls_cleanup;
  result->tls_cleanup_attempted = response->tls_cleanup_attempted;
  memcpy(result->media_type, response->media_type, sizeof(result->media_type));
  memcpy(result->location, response->location, sizeof(result->location));
}

static bool redirect_status(unsigned status)
{
  return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

static bool collect_body(struct snapshot *snapshot, struct http_stream *stream,
    struct http_response *response)
{
  if (response->length_present) {
    if (response->length > snapshot->body_limit) {
      return fail(snapshot, HTTP_LIMIT);
    }
    if (!reserve_body(snapshot, response->length)) {
      return false;
    }
  }
  unsigned char bytes[HTTP_READ_MAX];
  for (;;) {
    size_t remaining = snapshot->body_limit - snapshot->body.size;
    size_t capacity = remaining < sizeof(bytes) ? remaining : sizeof(bytes);
    size_t count;
    enum http_read_result read = http_stream_read(stream, bytes, capacity ? capacity : 1, &count);
    response_metadata(snapshot->result, response);
    if (read == HTTP_READ_FAILED) {
      return fail(snapshot, response->error);
    }
    if (read == HTTP_READ_COMPLETE) {
      if (response->error != HTTP_OK) {
        return fail(snapshot, response->error);
      }
      if (response->state != HTTP_RESPONSE_COMPLETE) {
        return fail(snapshot, HTTP_BAD_RESPONSE);
      }
      return true;
    }
    if (!remaining) {
      /* The probe distinguishes exact-limit EOF from another body byte. */
      return fail(snapshot, HTTP_LIMIT);
    }
    if (!append_body(snapshot, bytes, count)) {
      return false;
    }
  }
}

void http_fetch(const struct http_client *client, struct http_storage *storage,
    const char *uri, uint64_t deadline_ns,
    const struct provider_legacy_http_open_request *request, struct http_result *result)
{
  *result = (struct http_result){0};
  struct provider_http_budget limit = {
    .body_bytes = HTTP_BODY_MAX, .header_bytes = HTTP_HEADERS_MAX,
    .fields = HTTP_FIELDS_MAX, .informational = HTTP_INFORMATIONAL_MAX,
  };
  if (request) {
    if (request->flags != PROVIDER_LEGACY_HTTP_CONTEXT &&
        request->flags != (PROVIDER_LEGACY_HTTP_CONTEXT | PROVIDER_LEGACY_HTTP_ORIGIN_CROSSED)) {
      result->error = HTTP_INVALID_URI;
      return;
    }
    limit = request->remaining;
    if (limit.body_bytes > HTTP_BODY_MAX || limit.header_bytes > HTTP_HEADERS_MAX ||
        limit.fields > HTTP_FIELDS_MAX || limit.informational > HTTP_INFORMATIONAL_MAX) {
      result->error = HTTP_INVALID_URI;
      return;
    }
  }
  struct http_response_limits metadata_limit = {
    .header_bytes = limit.header_bytes, .fields = limit.fields,
    .informational = limit.informational,
  };
  size_t header_receive_max = limit.body_bytes ? limit.body_bytes : 1;
  if (header_receive_max > HTTP_HEADERS_MAX) {
    header_receive_max = HTTP_HEADERS_MAX;
  }
  uint64_t now;
  result->network_status = client ? clock_now(client->authority.clock, &now) : CALL_BAD_REQUEST;
  if (result->network_status != CALL_OK) {
    result->error = HTTP_NETWORK_ERROR;
    goto failed;
  }
  if (now > UINT64_MAX - HTTP_FETCH_NS) {
    result->error = HTTP_LIMIT;
    goto failed;
  }
  uint64_t maximum = now + HTTP_FETCH_NS;
  if (!deadline_ns || deadline_ns > maximum) {
    deadline_ns = maximum;
  }
  struct http_response response;
  struct http_stream *stream = http_get_open_prefetch(client, uri, deadline_ns,
      &metadata_limit, header_receive_max, &response);
  response_metadata(result, &response);
  if (!stream) {
    result->error = response.error;
    goto failed;
  }
  struct snapshot snapshot = {
    .result = result, .body = {.storage = storage}, .body_limit = limit.body_bytes,
  };
  bool success = false;
  bool redirect = redirect_status(result->status);
  if (redirect) {
    char destination[HTTP_URI_MAX + 1];
    enum call_status resolved = CALL_BAD_REQUEST;
    if (!response.invalid_location && response.location[0]) {
      resolved = http_url_resolve(uri, response.location, destination);
    }
    if (resolved != CALL_OK) {
      result->error = resolved == CALL_FILE_TOO_LARGE ? HTTP_LIMIT : HTTP_INVALID_LOCATION;
    } else {
      size_t read_ahead = http_stream_read_ahead(stream);
      if (read_ahead > limit.body_bytes) {
        result->error = HTTP_LIMIT;
      } else {
        result->consumed.body_bytes = read_ahead;
        success = http_stream_check_deadline(stream);
        response_metadata(result, &response);
        if (!success) {
          result->error = response.error;
        }
      }
    }
  } else if (result->status != 200 && result->status != 204) {
    result->error = HTTP_REJECTED_STATUS;
  } else {
    success = collect_body(&snapshot, stream, &response);
  }
  /* Snapshot policy errors and deliberate redirect aborts precede cleanup. */
  enum call_status closed = http_stream_close(stream);
  if (success && closed != CALL_OK) {
    success = false;
    result->error = HTTP_NETWORK_ERROR;
    result->network_status = closed;
  }
  if (success) {
    if (redirect) {
      result->outcome = PROVIDER_LEGACY_HTTP_OUTCOME_REDIRECT;
      result->media_type[0] = 0;
    } else {
      result->outcome = PROVIDER_LEGACY_HTTP_OUTCOME_BYTES;
      result->body = snapshot.body;
    }
    return;
  }
  http_body_release(&snapshot.body);

failed:
  result->media_type[0] = 0;
  result->location[0] = 0;
  result->outcome = PROVIDER_LEGACY_HTTP_OUTCOME_ERROR;
}
