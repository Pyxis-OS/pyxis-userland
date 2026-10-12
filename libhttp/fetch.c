#include "internal.h"
#include "../common/dns.h"
#include "../common/udp.h"
#include <clock.h>
#include <handle.h>
#include <picohttpparser.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tcp.h>

#define HTTP_CHUNK_LINE_MAX 8192

static bool fail(struct http_stream *fetch, enum http_error error)
{
  if (fetch->result->error == HTTP_OK) {
    fetch->result->error = error;
  }
  return false;
}

static bool network(struct http_stream *fetch, enum call_status status)
{
  if (status == CALL_OK) {
    return true;
  }
  if (fetch->result->error == HTTP_OK) {
    fetch->result->network_status = status;
  }
  return fail(fetch, HTTP_NETWORK_ERROR);
}

static bool check_deadline(struct http_stream *fetch)
{
  uint64_t now;
  if (!network(fetch, clock_now(fetch->authority.clock, &now))) {
    return false;
  }
  return network(fetch, now >= fetch->deadline ? CALL_TIMED_OUT : CALL_OK);
}

bool http_deadline(struct http_stream *fetch, uint64_t maximum, uint64_t *deadline)
{
  uint64_t now;
  if (!network(fetch, clock_now(fetch->authority.clock, &now)) ||
      !network(fetch, now < fetch->deadline ? CALL_OK : CALL_TIMED_OUT)) {
    return false;
  }
  *deadline = fetch->deadline - now > maximum ? now + maximum : fetch->deadline;
  return true;
}

static void consume(struct http_stream *fetch, size_t count)
{
  fetch->buffered -= count;
  memmove(fetch->bytes, fetch->bytes + count, fetch->buffered);
}

/* Only close-delimited bodies accept EOF. TLS reports it exclusively for an
 * authenticated close_notify; incomplete framed input remains an HTTP error. */
static bool receive(struct http_stream *fetch, size_t capacity, bool *eof)
{
  if (!check_deadline(fetch)) {
    return false;
  }
  size_t available = sizeof(fetch->bytes) - fetch->buffered;
  if (available > capacity) {
    available = capacity;
  }
  if (!available) {
    return fail(fetch, HTTP_LIMIT);
  }
  size_t length;
  if (fetch->tls) {
    if (!tls_read(fetch->tls, fetch->bytes + fetch->buffered, available,
        &length, eof, &fetch->result->tls_failure)) {
      return fail(fetch, HTTP_TLS_ERROR);
    }
  } else {
    if (available > TCP_READ_MAX_BYTES) {
      available = TCP_READ_MAX_BYTES;
    }
    struct tcp_read_reply reply;
    for (;;) {
      uint64_t deadline;
      if (!http_deadline(fetch, TCP_READ_MAX_WAIT_NS, &deadline)) {
        return false;
      }
      enum call_status status = tcp_read(fetch->stream, fetch->bytes + fetch->buffered,
          available, deadline, &reply);
      if (status == CALL_TIMED_OUT) {
        if (!check_deadline(fetch)) {
          return false;
        }
        status = tcp_try_read(fetch->stream, fetch->bytes + fetch->buffered, available, &reply);
        if (status == CALL_WOULD_BLOCK) {
          continue;
        }
      }
      if (!network(fetch, status)) {
        return false;
      }
      break;
    }
    length = reply.length;
    *eof = length == 0;
  }
  fetch->buffered += length;
  return true;
}

static bool receive_required(struct http_stream *fetch, size_t capacity)
{
  bool eof;
  if (!receive(fetch, capacity, &eof)) {
    return false;
  }
  return !eof || fail(fetch, HTTP_BAD_RESPONSE);
}

static bool receive_headers(struct http_stream *fetch)
{
  size_t remaining = fetch->limit.header_bytes - fetch->header_bytes;
  if (fetch->buffered >= remaining) {
    return fail(fetch, HTTP_LIMIT);
  }
  size_t capacity = remaining - fetch->buffered;
  if (capacity > fetch->header_receive_max) {
    capacity = fetch->header_receive_max;
  }
  return receive_required(fetch, capacity);
}

static bool valid_lines(const char *bytes, size_t size)
{
  for (size_t i = 0; i < size; ++i) {
    if (bytes[i] == '\r') {
      if (++i == size || bytes[i] != '\n') {
        return false;
      }
    } else if (bytes[i] == '\n') {
      return false;
    }
  }
  return true;
}

static bool count_headers(struct http_stream *fetch, size_t bytes, size_t fields)
{
  if (bytes > fetch->limit.header_bytes - fetch->header_bytes ||
      fields > fetch->limit.fields - fetch->fields) {
    return fail(fetch, HTTP_LIMIT);
  }
  if (!valid_lines(fetch->bytes, bytes)) {
    return fail(fetch, HTTP_BAD_RESPONSE);
  }
  fetch->header_bytes += bytes;
  fetch->fields += fields;
  fetch->result->consumed.header_bytes = fetch->header_bytes;
  fetch->result->consumed.fields = fetch->fields;
  return true;
}

static bool field_is(const struct phr_header *header, const char *name)
{
  return http_equal(header->name, header->name_len, name);
}

static bool parse_fields(struct http_stream *fetch, size_t count, struct framing *framing,
    bool trailer, bool redirect)
{
  bool encoding = false, media_type = false;
  for (size_t i = 0; i < count; ++i) {
    struct phr_header *header = &fetch->headers[i];
    if (!header->name) {
      return fail(fetch, HTTP_BAD_RESPONSE);
    }
    const char *value = header->value;
    size_t length = header->value_len;
    while (length && (*value == ' ' || *value == '\t')) {
      ++value;
      --length;
    }
    while (length && (value[length - 1] == ' ' || value[length - 1] == '\t')) {
      --length;
    }
    bool content_length = field_is(header, "content-length");
    bool transfer_encoding = field_is(header, "transfer-encoding");
    bool content_encoding = field_is(header, "content-encoding");
    bool content_type = field_is(header, "content-type");
    if (!trailer && redirect && field_is(header, "location")) {
      if (fetch->result->location[0] || !length || memchr(value, 0, length)) {
        fetch->result->invalid_location = true;
      } else {
        if (length > HTTP_URI_MAX) {
          return fail(fetch, HTTP_LIMIT);
        }
        memcpy(fetch->result->location, value, length);
        fetch->result->location[length] = 0;
      }
    }
    if (trailer) {
      /* Trailers cannot change the framing or representation already consumed. */
      if (content_length || transfer_encoding || content_encoding || content_type ||
          field_is(header, "host") || field_is(header, "connection") ||
          field_is(header, "trailer")) {
        return fail(fetch, HTTP_BAD_RESPONSE);
      }
      continue;
    }
    if (content_length) {
      if (framing->length_present || !length) {
        return fail(fetch, HTTP_BAD_RESPONSE);
      }
      uint64_t size = 0;
      for (size_t j = 0; j < length; ++j) {
        if (value[j] < '0' || value[j] > '9') {
          return fail(fetch, HTTP_BAD_RESPONSE);
        }
        unsigned digit = value[j] - '0';
        if (size > (UINT64_MAX - digit) / 10) {
          return fail(fetch, HTTP_LIMIT);
        }
        size = size * 10 + digit;
      }
      framing->length_present = true;
      framing->length = size;
    } else if (transfer_encoding) {
      if (framing->chunked) {
        return fail(fetch, HTTP_BAD_RESPONSE);
      }
      if (!http_equal(value, length, "chunked")) {
        return fail(fetch, HTTP_UNSUPPORTED);
      }
      framing->chunked = true;
    } else if (content_encoding) {
      if (encoding) {
        return fail(fetch, HTTP_BAD_RESPONSE);
      }
      if (!http_equal(value, length, "identity")) {
        return fail(fetch, HTTP_UNSUPPORTED);
      }
      encoding = true;
    } else if (content_type) {
      if (media_type || !length) {
        return fail(fetch, HTTP_BAD_RESPONSE);
      }
      if (length > HTTP_MEDIA_TYPE_MAX) {
        return fail(fetch, HTTP_LIMIT);
      }
      for (size_t j = 0; j < length; ++j) {
        if ((unsigned char)value[j] < ' ' || (unsigned char)value[j] > '~') {
          return fail(fetch, HTTP_BAD_RESPONSE);
        }
      }
      memcpy(fetch->result->media_type, value, length);
      fetch->result->media_type[length] = 0;
      media_type = true;
    }
  }
  return !(framing->chunked && framing->length_present) || fail(fetch, HTTP_BAD_RESPONSE);
}

int http_parse_response_headers(struct http_stream *fetch, struct framing *framing)
{
  for (;;) {
    if (!check_deadline(fetch)) {
      return -1;
    }
    int minor, status;
    const char *message;
    size_t message_size, field_limit = fetch->limit.fields - fetch->fields;
    size_t count = field_limit;
    int parsed = phr_parse_response(fetch->bytes, fetch->buffered, &minor, &status,
        &message, &message_size, fetch->headers, &count, fetch->header_previous);
    if (parsed == -2) {
      fetch->header_previous = fetch->buffered;
      return 0;
    }
    if (parsed < 0) {
      fail(fetch, count == field_limit ? HTTP_LIMIT : HTTP_BAD_RESPONSE);
      return -1;
    }
    if (status >= 200) {
      fetch->result->status = status;
    }
    if (!count_headers(fetch, parsed, count)) {
      return -1;
    }
    if (minor > 1) {
      fail(fetch, HTTP_UNSUPPORTED);
      return -1;
    }
    *framing = (struct framing){0};
    bool redirect = status == 301 || status == 302 || status == 303 ||
        status == 307 || status == 308;
    if (!parse_fields(fetch, count, framing, false, redirect)) {
      return -1;
    }
    consume(fetch, parsed);
    if (status >= 100 && status < 200) {
      if (status == 101) {
        fail(fetch, HTTP_UNSUPPORTED);
        return -1;
      }
      if (framing->chunked || framing->length_present) {
        fail(fetch, HTTP_BAD_RESPONSE);
        return -1;
      }
      if (fetch->result->consumed.informational == fetch->limit.informational) {
        fail(fetch, HTTP_LIMIT);
        return -1;
      }
      ++fetch->result->consumed.informational;
      fetch->result->media_type[0] = 0;
      fetch->header_previous = 0;
      continue;
    }
    if (status < 200 || status > 599) {
      fail(fetch, HTTP_BAD_RESPONSE);
      return -1;
    }
    if (minor == 0 && framing->chunked) {
      fail(fetch, HTTP_BAD_RESPONSE);
      return -1;
    }
    if (status == 204 && (framing->length_present || framing->chunked)) {
      fail(fetch, HTTP_BAD_RESPONSE);
      return -1;
    }
    fetch->result->length_present = framing->length_present;
    fetch->result->length = framing->length;
    return 1;
  }
}

static bool trailers(struct http_stream *fetch)
{
  for (;;) {
    size_t field_limit = fetch->limit.fields - fetch->fields;
    size_t count = field_limit;
    int parsed = phr_parse_headers(fetch->bytes, fetch->buffered, fetch->headers,
        &count, 0);
    if (parsed == -2) {
      if (!receive_headers(fetch)) {
        return false;
      }
      continue;
    }
    if (parsed < 0) {
      return fail(fetch, count == field_limit ? HTTP_LIMIT : HTTP_BAD_RESPONSE);
    }
    struct framing ignored = {0};
    return count_headers(fetch, parsed, count) &&
        parse_fields(fetch, count, &ignored, true, false);
  }
}

/* Validate ignored extensions as well as the checked chunk-size arithmetic.
 * Chunk lines have a per-line bound; no cumulative body/overhead allowance. */
static bool chunk_extensions(const char *line, size_t size)
{
  size_t i = 0;
  while (i < size && ((line[i] >= '0' && line[i] <= '9') ||
      (line[i] >= 'a' && line[i] <= 'f') || (line[i] >= 'A' && line[i] <= 'F'))) {
    ++i;
  }
  if (!i) {
    return false;
  }
  while (i < size) {
    while (i < size && (line[i] == ' ' || line[i] == '\t')) {
      ++i;
    }
    if (i == size || line[i++] != ';') {
      return false;
    }
    while (i < size && (line[i] == ' ' || line[i] == '\t')) {
      ++i;
    }
    size_t start = i;
    while (i < size && http_token(line[i])) {
      ++i;
    }
    if (start == i) {
      return false;
    }
    size_t after_name = i;
    while (i < size && (line[i] == ' ' || line[i] == '\t')) {
      ++i;
    }
    if (i == size || line[i] != '=') {
      i = after_name;
      continue;
    }
    ++i;
    while (i < size && (line[i] == ' ' || line[i] == '\t')) {
      ++i;
    }
    if (i < size && line[i] == '"') {
      ++i;
      bool closed = false;
      while (i < size) {
        unsigned char byte = line[i++];
        if (byte == '"') {
          closed = true;
          break;
        }
        if (byte == '\\') {
          if (i == size) {
            return false;
          }
          byte = line[i++];
        }
        if ((byte < ' ' && byte != '\t') || byte == 127) {
          return false;
        }
      }
      if (!closed) {
        return false;
      }
    } else {
      start = i;
      while (i < size && http_token(line[i])) {
        ++i;
      }
      if (start == i) {
        return false;
      }
    }
  }
  return true;
}

static bool resolve(struct http_stream *fetch, const char *host, uint32_t *address)
{
  if (udp_parse_address(host, address)) {
    return true;
  }
  struct dns_name name;
  if (!dns_name_from_text(host, &name)) {
    return fail(fetch, HTTP_INVALID_URI);
  }
  const struct http_authority *authority = &fetch->authority;
  if (authority->udp == HANDLE_INVALID || authority->random == HANDLE_INVALID) {
    return network(fetch, CALL_DENIED);
  }
  struct dns_exchange exchange;
  dns_query(authority->udp, authority->clock, authority->random, authority->dns_server,
      &name, fetch->deadline, &exchange);
  if (!network(fetch, exchange.status)) {
    return false;
  }
  if (exchange.response != DNS_COMPLETE) {
    return fail(fetch, HTTP_DNS_ERROR);
  }
  fetch->result->dns_rcode = exchange.reply.rcode;
  if (exchange.reply.rcode || dns_select_address(&exchange.reply, &name, address) != DNS_ADDRESS_FOUND) {
    return fail(fetch, HTTP_DNS_ERROR);
  }
  return true;
}

static enum call_status close_transport(struct http_stream *fetch, bool framed)
{
  enum call_status status = CALL_OK;
  if (fetch->tls) {
    if (framed && fetch->result->request_sent) {
      tls_close_notify(fetch->tls, &fetch->result->tls_cleanup);
    }
    tls_connection_free(fetch->tls);
    fetch->tls = NULL;
  }
  if (fetch->stream != HANDLE_INVALID) {
    if (!framed || !fetch->result->request_sent) {
      status = tcp_abort(fetch->stream);
    } else {
      tcp_shutdown_write(fetch->stream);
    }
    if (handle_close(fetch->stream) != 0 && status == CALL_OK) {
      status = CALL_BAD_HANDLE;
    }
    fetch->stream = HANDLE_INVALID;
  }
  fetch->result->close_status = status;
  return status;
}

static enum http_read_result finish(struct http_stream *fetch, bool framed)
{
  bool success = framed && check_deadline(fetch);
  enum call_status closed = close_transport(fetch, success);
  if (success && !network(fetch, closed)) {
    success = false;
  }
  fetch->result->state = success ? HTTP_RESPONSE_COMPLETE : HTTP_RESPONSE_FAILED;
  return success ? HTTP_READ_COMPLETE : HTTP_READ_FAILED;
}

static struct http_stream *request_open(const struct http_client *client,
    const char *uri_text, const struct http_request *request, uint64_t deadline_ns,
    const struct http_response_limits *limits,
    size_t header_receive_max, struct http_response *result)
{
  *result = (struct http_response){.state = HTTP_RESPONSE_FAILED};
  struct http_response_limits limit = {
    .header_bytes = HTTP_HEADERS_MAX, .fields = HTTP_FIELDS_MAX,
    .informational = HTTP_INFORMATIONAL_MAX,
  };
  if (!client || !uri_text || !header_receive_max || header_receive_max > HTTP_HEADERS_MAX) {
    result->error = HTTP_BAD_ARGUMENT;
    return NULL;
  }
  if (limits) {
    limit = *limits;
    if (limit.header_bytes > HTTP_HEADERS_MAX || limit.fields > HTTP_FIELDS_MAX ||
        limit.informational > HTTP_INFORMATIONAL_MAX) {
      result->error = HTTP_BAD_ARGUMENT;
      return NULL;
    }
  }
  const struct http_authority *authority = &client->authority;
  if (authority->tcp == HANDLE_INVALID || authority->clock == HANDLE_INVALID) {
    result->error = HTTP_NETWORK_ERROR;
    result->network_status = CALL_DENIED;
    return NULL;
  }
  uint64_t now;
  result->network_status = clock_now(authority->clock, &now);
  if (result->network_status != CALL_OK) {
    result->error = HTTP_NETWORK_ERROR;
    return NULL;
  }
  uint64_t deadline = deadline_ns;
  if (!deadline) {
    if (now > UINT64_MAX - HTTP_FETCH_NS) {
      result->error = HTTP_LIMIT;
      return NULL;
    }
    deadline = now + HTTP_FETCH_NS;
  }
  if (now >= deadline) {
    result->error = HTTP_NETWORK_ERROR;
    result->network_status = CALL_TIMED_OUT;
    return NULL;
  }
  struct http_uri uri;
  result->error = http_parse_uri(uri_text, &uri);
  if (result->error != HTTP_OK) {
    return NULL;
  }
  if (uri.scheme != client->scheme) {
    result->error = HTTP_UNSUPPORTED;
    return NULL;
  }
  if (uri.scheme == HTTP_SCHEME_HTTPS && !client->tls) {
    result->error = HTTP_TLS_ERROR;
    result->tls_failure.error = TLS_BAD_STATE;
    return NULL;
  }
  struct http_outgoing outgoing;
  if (!http_prepare_request(request, &uri, &outgoing, result)) {
    return NULL;
  }
  struct http_stream *fetch = calloc(1, sizeof(*fetch));
  if (!fetch) {
    result->error = HTTP_NO_MEMORY;
    http_release_request(&outgoing);
    return NULL;
  }
  fetch->authority = *authority;
  fetch->result = result;
  fetch->stream = HANDLE_INVALID;
  fetch->deadline = deadline;
  fetch->limit = limit;
  fetch->header_receive_max = header_receive_max;
  uint32_t address;
  struct tcp_connect_reply connection;
  struct framing framing;
  uint64_t connect_deadline;
  if (!check_deadline(fetch) || !resolve(fetch, uri.host, &address) ||
      !http_deadline(fetch, TCP_CONNECT_MAX_WAIT_NS, &connect_deadline) ||
      !network(fetch, tcp_connect(authority->tcp, address, uri.port, connect_deadline, &connection))) {
    goto failed;
  }
  fetch->stream = connection.handle;
  if (uri.scheme == HTTP_SCHEME_HTTPS) {
    struct tls_authority tls_authority = {.random = authority->random, .clock = authority->clock};
    if (!tls_connection_open(client->tls, &tls_authority, fetch->stream, uri.host,
        deadline, &fetch->tls, &result->tls_failure)) {
      fail(fetch, HTTP_TLS_ERROR);
      goto failed;
    }
  }
  if (!http_exchange_request(fetch, &outgoing, &framing) || !check_deadline(fetch)) {
    goto failed;
  }
  fetch->remaining = framing.length;
  fetch->stage = result->status == 204 || result->status == 304 ? BODY_FRAMED :
      framing.chunked ? BODY_CHUNK_SIZE : framing.length_present ? BODY_FIXED : BODY_CLOSE;
  result->state = HTTP_RESPONSE_PENDING;
  http_release_request(&outgoing);
  return fetch;

failed:
  http_release_request(&outgoing);
  close_transport(fetch, false);
  free(fetch);
  return NULL;
}

struct http_stream *http_request_open(const struct http_client *client, const char *uri,
    const struct http_request *request, uint64_t deadline_ns,
    const struct http_response_limits *limits, struct http_response *response)
{
  return request_open(client, uri, request, deadline_ns, limits, HTTP_HEADERS_MAX, response);
}

struct http_stream *http_get_open_prefetch(const struct http_client *client,
    const char *uri, uint64_t deadline_ns, const struct http_response_limits *limits,
    size_t header_receive_max, struct http_response *response)
{
  const struct http_request request = {.method = HTTP_METHOD_GET};
  return request_open(client, uri, &request, deadline_ns, limits, header_receive_max, response);
}

struct http_stream *http_get_open(const struct http_client *client, const char *uri,
    uint64_t deadline_ns, const struct http_response_limits *limits, struct http_response *response)
{
  return http_get_open_prefetch(client, uri, deadline_ns, limits, HTTP_HEADERS_MAX, response);
}

size_t http_stream_read_ahead(const struct http_stream *stream)
{
  return stream->buffered;
}

bool http_stream_check_deadline(struct http_stream *stream)
{
  return check_deadline(stream);
}

static bool chunk_size(struct http_stream *fetch)
{
  size_t line_size;
  for (;;) {
    char *end = memchr(fetch->bytes, '\n', fetch->buffered);
    line_size = end ? (size_t)(end - fetch->bytes) + 1 : 0;
    if (line_size > HTTP_CHUNK_LINE_MAX || (!end && fetch->buffered >= HTTP_CHUNK_LINE_MAX)) {
      return fail(fetch, HTTP_LIMIT);
    }
    if (end) {
      break;
    }
    if (!receive_required(fetch, HTTP_CHUNK_LINE_MAX - fetch->buffered)) {
      return false;
    }
  }
  if (line_size < 3 || fetch->bytes[line_size - 2] != '\r' ||
      !chunk_extensions(fetch->bytes, line_size - 2)) {
    return fail(fetch, HTTP_BAD_RESPONSE);
  }
  uint64_t value = 0;
  for (size_t i = 0; i < line_size - 2; i++) {
    unsigned char byte = fetch->bytes[i];
    unsigned digit;
    if (byte >= '0' && byte <= '9') {
      digit = byte - '0';
    } else if (byte >= 'a' && byte <= 'f') {
      digit = byte - 'a' + 10;
    } else if (byte >= 'A' && byte <= 'F') {
      digit = byte - 'A' + 10;
    } else {
      break;
    }
    if (value > (UINT64_MAX - digit) / 16) {
      return fail(fetch, HTTP_LIMIT);
    }
    value = value * 16 + digit;
  }
  if (value > UINT64_MAX - fetch->result->consumed.body_bytes) {
    return fail(fetch, HTTP_LIMIT);
  }
  consume(fetch, line_size);
  fetch->remaining = value;
  fetch->stage = value ? BODY_CHUNK_DATA : BODY_TRAILERS;
  return true;
}

enum http_read_result http_stream_read(struct http_stream *fetch, void *bytes,
    size_t capacity, size_t *count)
{
  *count = 0;
  if (fetch->result->state != HTTP_RESPONSE_PENDING) {
    return fetch->result->state == HTTP_RESPONSE_COMPLETE ? HTTP_READ_COMPLETE : HTTP_READ_FAILED;
  }
  if (!bytes || !capacity) {
    fail(fetch, HTTP_BAD_ARGUMENT);
    return finish(fetch, false);
  }
  if (capacity > HTTP_READ_MAX) {
    capacity = HTTP_READ_MAX;
  }
  for (;;) {
    if (!check_deadline(fetch)) {
      return finish(fetch, false);
    }
    if (fetch->stage == BODY_FRAMED || (fetch->stage == BODY_FIXED && !fetch->remaining)) {
      return finish(fetch, true);
    }
    if (fetch->stage == BODY_CHUNK_SIZE) {
      if (!chunk_size(fetch)) {
        return finish(fetch, false);
      }
      continue;
    }
    if (fetch->stage == BODY_CHUNK_END) {
      while (fetch->buffered < 2) {
        if (!receive_required(fetch, 2 - fetch->buffered)) {
          return finish(fetch, false);
        }
      }
      if (fetch->bytes[0] != '\r' || fetch->bytes[1] != '\n') {
        fail(fetch, HTTP_BAD_RESPONSE);
        return finish(fetch, false);
      }
      consume(fetch, 2);
      fetch->stage = BODY_CHUNK_SIZE;
      continue;
    }
    if (fetch->stage == BODY_TRAILERS) {
      if (!trailers(fetch)) {
        return finish(fetch, false);
      }
      return finish(fetch, true);
    }
    bool framed = fetch->stage != BODY_CLOSE;
    if (!fetch->buffered) {
      size_t receive_max = capacity;
      if (framed && fetch->remaining < receive_max) {
        receive_max = (size_t)fetch->remaining;
      }
      bool eof;
      if (!receive(fetch, receive_max, &eof)) {
        return finish(fetch, false);
      }
      if (eof) {
        if (framed) {
          fail(fetch, HTTP_BAD_RESPONSE);
        }
        return finish(fetch, !framed);
      }
      if (!fetch->buffered) {
        network(fetch, CALL_IO);
        return finish(fetch, false);
      }
    }
    size_t take = fetch->buffered < capacity ? fetch->buffered : capacity;
    if (framed && fetch->remaining < take) {
      take = (size_t)fetch->remaining;
    }
    if (take > UINT64_MAX - fetch->result->consumed.body_bytes) {
      fail(fetch, HTTP_LIMIT);
      return finish(fetch, false);
    }
    memcpy(bytes, fetch->bytes, take);
    consume(fetch, take);
    fetch->result->consumed.body_bytes += take;
    if (framed) {
      fetch->remaining -= take;
      if (!fetch->remaining && fetch->stage == BODY_CHUNK_DATA) {
        fetch->stage = BODY_CHUNK_END;
      }
    }
    *count = take;
    return HTTP_READ_DATA;
  }
}

enum call_status http_stream_close(struct http_stream *stream)
{
  enum call_status status = stream->result->close_status;
  if (stream->result->state == HTTP_RESPONSE_PENDING) {
    fail(stream, HTTP_ABORTED);
    status = close_transport(stream, false);
    stream->result->state = HTTP_RESPONSE_FAILED;
  }
  free(stream);
  return status;
}

const char *http_error_name(enum http_error error)
{
  switch (error) {
  case HTTP_OK: return "ok";
  case HTTP_INVALID_URI: return "invalid URI";
  case HTTP_INVALID_LOCATION: return "invalid redirect Location";
  case HTTP_UNSUPPORTED: return "unsupported HTTP feature";
  case HTTP_BAD_RESPONSE: return "malformed or truncated response";
  case HTTP_REJECTED_STATUS: return "rejected HTTP status";
  case HTTP_LIMIT: return "HTTP size or count limit";
  case HTTP_QUOTA: return "body storage quota";
  case HTTP_NO_MEMORY: return "allocation failed";
  case HTTP_NETWORK_ERROR: return "native network operation failed";
  case HTTP_DNS_ERROR: return "DNS answer unavailable or invalid";
  case HTTP_TLS_ERROR: return "TLS operation failed";
  case HTTP_ABORTED: return "response abandoned before completion";
  case HTTP_BAD_ARGUMENT: return "invalid HTTP argument";
  case HTTP_SOURCE_ERROR: return "request source unavailable or changed";
  default: return "invalid result";
  }
}

static enum call_status tls_status(const struct tls_result *result)
{
  switch (result->error) {
  case TLS_OK:
    return CALL_OK;
  case TLS_CERTIFICATE_ERROR:
  case TLS_TRUST_ERROR:
    return CALL_DENIED;
  case TLS_BAD_STATE:
  case TLS_UNSUPPORTED:
    return CALL_BAD_OPERATION;
  case TLS_PROTOCOL_ERROR:
  case TLS_TRUNCATED:
    return CALL_IO;
  case TLS_LIMIT:
    return CALL_FILE_TOO_LARGE;
  case TLS_QUOTA:
    return CALL_QUOTA;
  case TLS_NO_MEMORY:
    return CALL_NO_MEMORY;
  case TLS_CLOCK_ERROR:
  case TLS_ENTROPY_ERROR:
  case TLS_TRANSPORT_ERROR:
  case TLS_DEADLINE:
    return result->native_status == CALL_OK ? CALL_IO : result->native_status;
  }
  return CALL_IO;
}

enum call_status http_result_status(const struct http_result *result)
{
  switch (result->error) {
  case HTTP_OK:
    return CALL_OK;
  case HTTP_INVALID_URI:
  case HTTP_INVALID_LOCATION:
    return CALL_BAD_REQUEST;
  case HTTP_UNSUPPORTED:
    return CALL_BAD_OPERATION;
  case HTTP_BAD_ARGUMENT:
    return CALL_BAD_REQUEST;
  case HTTP_ABORTED:
  case HTTP_SOURCE_ERROR:
  case HTTP_BAD_RESPONSE:
    return CALL_IO;
  case HTTP_REJECTED_STATUS:
    if (result->status == 404 || result->status == 410) {
      return CALL_NOT_FOUND;
    }
    if (result->status == 401 || result->status == 403) {
      return CALL_DENIED;
    }
    if (result->status >= 200 && result->status < 400) {
      return CALL_BAD_OPERATION;
    }
    return CALL_IO;
  case HTTP_LIMIT:
    return CALL_FILE_TOO_LARGE;
  case HTTP_QUOTA:
    return CALL_QUOTA;
  case HTTP_NO_MEMORY:
    return CALL_NO_MEMORY;
  case HTTP_NETWORK_ERROR:
    return result->network_status;
  case HTTP_DNS_ERROR:
    /* Only NXDOMAIN unambiguously identifies an absent name. */
    return result->dns_rcode == 3 ? CALL_NOT_FOUND : CALL_IO;
  case HTTP_TLS_ERROR:
    return tls_status(&result->tls_failure);
  }
  return CALL_IO;
}
