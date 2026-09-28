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
#define HTTP_INITIAL_CAPACITY 4096

struct fetch {
  const struct http_authority *authority;
  struct http_result *result;
  struct http_body body;
  handle_t stream;
  uint64_t deadline;
  size_t buffered, header_bytes, fields;
  char bytes[HTTP_HEADERS_MAX];
  struct phr_header headers[HTTP_FIELDS_MAX];
};

struct framing {
  bool length_present, chunked;
  size_t length;
};

static bool fail(struct fetch *fetch, enum http_error error)
{
  fetch->result->error = error;
  return false;
}

static bool network(struct fetch *fetch, enum call_status status)
{
  if (status == CALL_OK) {
    return true;
  }
  fetch->result->network_status = status;
  return fail(fetch, HTTP_NETWORK_ERROR);
}

static bool check_deadline(struct fetch *fetch)
{
  uint64_t now;
  if (!network(fetch, clock_now(fetch->authority->clock, &now))) {
    return false;
  }
  return network(fetch, now >= fetch->deadline ? CALL_TIMED_OUT : CALL_OK);
}

void http_body_release(struct http_body *body)
{
  if (body->storage) {
    body->storage->reserved -= body->capacity;
  }
  free(body->data);
  *body = (struct http_body){0};
}

static bool reserve_body(struct fetch *fetch, size_t capacity)
{
  struct http_body *body = &fetch->body;
  if (capacity > HTTP_BODY_MAX) {
    return fail(fetch, HTTP_LIMIT);
  }
  if (capacity <= body->capacity) {
    return true;
  }
  struct http_storage *storage = body->storage;
  if (storage->reserved > HTTP_STORAGE_MAX || capacity > HTTP_STORAGE_MAX - storage->reserved) {
    return fail(fetch, HTTP_QUOTA);
  }
  /* Keep the old reservation until replacement storage actually exists. */
  storage->reserved += capacity;
  unsigned char *data = malloc(capacity);
  if (!data) {
    storage->reserved -= capacity;
    return fail(fetch, HTTP_NO_MEMORY);
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

static bool append_body(struct fetch *fetch, const char *bytes, size_t size)
{
  struct http_body *body = &fetch->body;
  if (size > HTTP_BODY_MAX - body->size) {
    return fail(fetch, HTTP_LIMIT);
  }
  size_t needed = body->size + size;
  if (needed > body->capacity) {
    size_t capacity = body->capacity ? body->capacity : HTTP_INITIAL_CAPACITY;
    while (capacity < needed) {
      capacity *= 2;
    }
    if (!reserve_body(fetch, capacity)) {
      return false;
    }
  }
  if (size) {
    memcpy(body->data + body->size, bytes, size);
  }
  body->size += size;
  return true;
}

static void consume(struct fetch *fetch, size_t count)
{
  fetch->buffered -= count;
  memmove(fetch->bytes, fetch->bytes + count, fetch->buffered);
}

/* EOF is separate from native failure; only close-delimited bodies accept it. */
static bool receive(struct fetch *fetch, bool *eof)
{
  if (!check_deadline(fetch)) {
    return false;
  }
  size_t available = sizeof(fetch->bytes) - fetch->buffered;
  if (!available) {
    return fail(fetch, HTTP_LIMIT);
  }
  struct tcp_read_reply reply;
  if (!network(fetch, tcp_read(fetch->stream, fetch->bytes + fetch->buffered,
      available, fetch->deadline, &reply))) {
    return false;
  }
  fetch->buffered += reply.length;
  *eof = reply.length == 0;
  return true;
}

static bool receive_required(struct fetch *fetch)
{
  bool eof;
  if (!receive(fetch, &eof)) {
    return false;
  }
  return !eof || fail(fetch, HTTP_BAD_RESPONSE);
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

static bool count_headers(struct fetch *fetch, size_t bytes, size_t fields)
{
  if (bytes > HTTP_HEADERS_MAX - fetch->header_bytes ||
      fields > HTTP_FIELDS_MAX - fetch->fields) {
    return fail(fetch, HTTP_LIMIT);
  }
  if (!valid_lines(fetch->bytes, bytes)) {
    return fail(fetch, HTTP_BAD_RESPONSE);
  }
  fetch->header_bytes += bytes;
  fetch->fields += fields;
  return true;
}

static bool field_is(const struct phr_header *header, const char *name)
{
  return http_equal(header->name, header->name_len, name);
}

static bool parse_fields(struct fetch *fetch, size_t count, struct framing *framing,
    bool trailer)
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
      size_t size = 0;
      for (size_t j = 0; j < length; ++j) {
        if (value[j] < '0' || value[j] > '9') {
          return fail(fetch, HTTP_BAD_RESPONSE);
        }
        unsigned digit = value[j] - '0';
        if (size > (SIZE_MAX - digit) / 10) {
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

static bool response_headers(struct fetch *fetch, struct framing *framing)
{
  unsigned informational = 0;
  size_t previous = 0;
  for (;;) {
    if (!check_deadline(fetch)) {
      return false;
    }
    int minor, status;
    const char *message;
    size_t message_size, count = HTTP_FIELDS_MAX;
    int parsed = phr_parse_response(fetch->bytes, fetch->buffered, &minor, &status,
        &message, &message_size, fetch->headers, &count, previous);
    if (parsed == -2) {
      if (fetch->buffered >= HTTP_HEADERS_MAX - fetch->header_bytes) {
        return fail(fetch, HTTP_LIMIT);
      }
      previous = fetch->buffered;
      if (!receive_required(fetch)) {
        return false;
      }
      continue;
    }
    if (parsed < 0) {
      return fail(fetch, count == HTTP_FIELDS_MAX ? HTTP_LIMIT : HTTP_BAD_RESPONSE);
    }
    if (status >= 200) {
      fetch->result->status = status;
    }
    if (!count_headers(fetch, parsed, count)) {
      return false;
    }
    if (minor > 1) {
      return fail(fetch, HTTP_UNSUPPORTED);
    }
    *framing = (struct framing){0};
    if (!parse_fields(fetch, count, framing, false)) {
      return false;
    }
    consume(fetch, parsed);
    if (status >= 100 && status < 200) {
      if (status == 101) {
        return fail(fetch, HTTP_UNSUPPORTED);
      }
      if (framing->chunked || framing->length_present) {
        return fail(fetch, HTTP_BAD_RESPONSE);
      }
      if (++informational > HTTP_INFORMATIONAL_MAX) {
        return fail(fetch, HTTP_LIMIT);
      }
      fetch->result->media_type[0] = 0;
      previous = 0;
      continue;
    }
    if (status < 200 || status > 599) {
      return fail(fetch, HTTP_BAD_RESPONSE);
    }
    if (status != 200 && status != 204) {
      return fail(fetch, HTTP_REJECTED_STATUS);
    }
    if ((status == 204 && (framing->length_present || framing->chunked)) ||
        (minor == 0 && framing->chunked)) {
      return fail(fetch, HTTP_BAD_RESPONSE);
    }
    return true;
  }
}

static bool trailers(struct fetch *fetch)
{
  for (;;) {
    size_t count = HTTP_FIELDS_MAX;
    int parsed = phr_parse_headers(fetch->bytes, fetch->buffered, fetch->headers,
        &count, 0);
    if (parsed == -2) {
      if (fetch->buffered >= HTTP_HEADERS_MAX - fetch->header_bytes) {
        return fail(fetch, HTTP_LIMIT);
      }
      if (!receive_required(fetch)) {
        return false;
      }
      continue;
    }
    if (parsed < 0) {
      return fail(fetch, count == HTTP_FIELDS_MAX ? HTTP_LIMIT : HTTP_BAD_RESPONSE);
    }
    struct framing ignored = {0};
    return count_headers(fetch, parsed, count) && parse_fields(fetch, count, &ignored, true);
  }
}

/* picohttpparser decodes chunk data; validate ignored extension syntax before
 * handing it a complete bounded size line. Do not let its trailer-skipping
 * option bypass field validation and the aggregate header budget. */
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

static bool chunked_body(struct fetch *fetch)
{
  struct phr_chunked_decoder decoder = {0};
  for (;;) {
    if (!check_deadline(fetch)) {
      return false;
    }
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
      if (!receive_required(fetch)) {
        return false;
      }
    }
    if (line_size < 3 || fetch->bytes[line_size - 2] != '\r' ||
        !chunk_extensions(fetch->bytes, line_size - 2)) {
      return fail(fetch, HTTP_BAD_RESPONSE);
    }
    size_t decoded = line_size;
    ssize_t state = phr_decode_chunked(&decoder, fetch->bytes, &decoded);
    if (state == -1 || decoded) {
      return fail(fetch, HTTP_BAD_RESPONSE);
    }
    consume(fetch, line_size);
    if (state == 0) {
      return trailers(fetch);
    }
    if (decoder.bytes_left_in_chunk > HTTP_BODY_MAX - fetch->body.size) {
      return fail(fetch, HTTP_LIMIT);
    }
    while (decoder.bytes_left_in_chunk) {
      if (!fetch->buffered && !receive_required(fetch)) {
        return false;
      }
      size_t count = fetch->buffered;
      if (count > decoder.bytes_left_in_chunk) {
        count = decoder.bytes_left_in_chunk;
      }
      decoded = count;
      if (phr_decode_chunked(&decoder, fetch->bytes, &decoded) != -2 || decoded != count) {
        return fail(fetch, HTTP_BAD_RESPONSE);
      }
      if (!append_body(fetch, fetch->bytes, decoded)) {
        return false;
      }
      consume(fetch, count);
    }
    while (fetch->buffered < 2) {
      if (!receive_required(fetch)) {
        return false;
      }
    }
    decoded = 2;
    if (phr_decode_chunked(&decoder, fetch->bytes, &decoded) != -2 || decoded) {
      return fail(fetch, HTTP_BAD_RESPONSE);
    }
    consume(fetch, 2);
  }
}

static bool response_body(struct fetch *fetch, const struct framing *framing)
{
  if (fetch->result->status == 204) {
    return true;
  }
  if (framing->chunked) {
    return chunked_body(fetch);
  }
  if (framing->length_present && !reserve_body(fetch, framing->length)) {
    return false;
  }
  for (;;) {
    if (!check_deadline(fetch)) {
      return false;
    }
    size_t count = fetch->buffered;
    if (framing->length_present && count > framing->length - fetch->body.size) {
      count = framing->length - fetch->body.size;
    }
    if (!append_body(fetch, fetch->bytes, count)) {
      return false;
    }
    consume(fetch, count);
    if (framing->length_present && fetch->body.size == framing->length) {
      return true;
    }
    bool eof;
    if (!receive(fetch, &eof)) {
      return false;
    }
    if (eof) {
      return !framing->length_present || fail(fetch, HTTP_BAD_RESPONSE);
    }
  }
}

static bool resolve(struct fetch *fetch, const char *host, uint32_t *address)
{
  if (udp_parse_address(host, address)) {
    return true;
  }
  struct dns_name name;
  if (!dns_name_from_text(host, &name)) {
    return fail(fetch, HTTP_INVALID_URI);
  }
  const struct http_authority *authority = fetch->authority;
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

static bool send_request(struct fetch *fetch, const struct http_uri *uri)
{
  char request[HTTP_URI_MAX + 384];
  int length = snprintf(request, sizeof(request),
      "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\nAccept-Encoding: identity\r\n\r\n",
      uri->target, uri->authority);
  if (length < 0 || (size_t)length >= sizeof(request)) {
    return fail(fetch, HTTP_LIMIT);
  }
  size_t sent = 0;
  while (sent < (size_t)length) {
    struct tcp_write_reply reply;
    if (!network(fetch, tcp_write(fetch->stream, request + sent, length - sent,
        fetch->deadline, &reply))) {
      return false;
    }
    sent += reply.length;
  }
  return true;
}

void http_fetch(const struct http_authority *authority, struct http_storage *storage,
    const char *uri_text, uint64_t deadline_ns, struct http_result *result)
{
  *result = (struct http_result){0};
  uint64_t now;
  result->network_status = clock_now(authority->clock, &now);
  if (result->network_status != CALL_OK) {
    result->error = HTTP_NETWORK_ERROR;
    return;
  }
  if (now > UINT64_MAX - HTTP_FETCH_NS) {
    result->error = HTTP_LIMIT;
    return;
  }
  uint64_t deadline = now + HTTP_FETCH_NS;
  if (deadline_ns && deadline_ns < deadline) {
    deadline = deadline_ns;
  }
  struct http_uri uri;
  result->error = http_parse_uri(uri_text, &uri);
  if (result->error != HTTP_OK) {
    return;
  }
  struct fetch *fetch = calloc(1, sizeof(*fetch));
  if (!fetch) {
    result->error = HTTP_NO_MEMORY;
    return;
  }
  fetch->authority = authority;
  fetch->result = result;
  fetch->body.storage = storage;
  fetch->stream = HANDLE_INVALID;
  fetch->deadline = deadline;
  uint32_t address;
  struct tcp_connect_reply connection;
  struct framing framing;
  bool success = false;
  if (!check_deadline(fetch) || !resolve(fetch, uri.host, &address) ||
      !network(fetch, tcp_connect(authority->tcp, address, uri.port, deadline, &connection))) {
    goto done;
  }
  fetch->stream = connection.handle;
  success = send_request(fetch, &uri) && response_headers(fetch, &framing) &&
      response_body(fetch, &framing) && check_deadline(fetch);

done:
  if (fetch->stream != HANDLE_INVALID) {
    if (!success) {
      tcp_abort(fetch->stream);
    } else {
      /* HTTP frames the request without EOF. Half-close only after the complete
       * response; a later transport failure cannot invalidate retained bytes. */
      tcp_shutdown_write(fetch->stream);
    }
    if (handle_close(fetch->stream) != 0 && success) {
      success = network(fetch, CALL_BAD_HANDLE);
    }
  }
  if (success) {
    result->body = fetch->body;
  } else {
    http_body_release(&fetch->body);
    result->media_type[0] = 0;
  }
  free(fetch);
}

const char *http_error_name(enum http_error error)
{
  switch (error) {
  case HTTP_OK: return "ok";
  case HTTP_INVALID_URI: return "invalid URI";
  case HTTP_UNSUPPORTED: return "unsupported HTTP feature";
  case HTTP_BAD_RESPONSE: return "malformed or truncated response";
  case HTTP_REJECTED_STATUS: return "rejected HTTP status";
  case HTTP_LIMIT: return "HTTP size or count limit";
  case HTTP_QUOTA: return "body storage quota";
  case HTTP_NO_MEMORY: return "allocation failed";
  case HTTP_NETWORK_ERROR: return "native network operation failed";
  case HTTP_DNS_ERROR: return "DNS answer unavailable or invalid";
  default: return "invalid result";
  }
}
