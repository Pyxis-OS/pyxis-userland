#include "internal.h"
#include <file.h>
#include <handle.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tcp.h>
#include <wait.h>

static bool fail(struct http_response *response, enum http_error error)
{
  if (response->error == HTTP_OK) {
    response->error = error;
  }
  return false;
}

static bool network(struct http_stream *stream, enum call_status status)
{
  if (status == CALL_OK) {
    return true;
  }
  if (stream->result->error == HTTP_OK) {
    stream->result->network_status = status;
  }
  return fail(stream->result, HTTP_NETWORK_ERROR);
}

static bool source(struct http_response *response, enum call_status status)
{
  if (status == CALL_OK) {
    return true;
  }
  response->source_status = status;
  return fail(response, HTTP_SOURCE_ERROR);
}

static unsigned char lowercase(unsigned char byte)
{
  return byte >= 'A' && byte <= 'Z' ? byte + 'a' - 'A' : byte;
}

static bool same_name(const struct http_request_header *left,
    const struct http_request_header *right)
{
  if (left->name_length != right->name_length) {
    return false;
  }
  for (size_t i = 0; i < left->name_length; ++i) {
    if (lowercase(left->name[i]) != lowercase(right->name[i])) {
      return false;
    }
  }
  return true;
}

static bool owned_header(const struct http_request_header *header)
{
  static const char *const names[] = {
    "host", "content-length", "transfer-encoding", "connection", "accept-encoding",
    "content-encoding", "expect", "trailer", "te", "upgrade", "proxy-connection",
  };
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
    if (http_equal(header->name, header->name_length, names[i])) {
      return true;
    }
  }
  return false;
}

static void erase(void *data, size_t length)
{
  /* Request fields/body chunks can carry secrets; erase owned scratch too. */
  volatile unsigned char *bytes = data;
  while (length) {
    *bytes++ = 0;
    --length;
  }
}

void http_release_headers(struct http_outgoing *outgoing)
{
  if (outgoing->headers) {
    erase(outgoing->headers, outgoing->header_size);
    free(outgoing->headers);
    outgoing->headers = NULL;
  }
}

void http_release_request(struct http_outgoing *outgoing)
{
  http_release_headers(outgoing);
  *outgoing = (struct http_outgoing){0};
}

bool http_prepare_request(const struct http_request *request, const struct http_uri *uri,
    struct http_outgoing *outgoing, struct http_response *response, bool check_source)
{
  *outgoing = (struct http_outgoing){0};
  if (!request || (request->header_count && !request->headers)) {
    return fail(response, HTTP_BAD_ARGUMENT);
  }
  const char *method;
  switch (request->method) {
  case HTTP_METHOD_GET: method = "GET"; break;
  case HTTP_METHOD_POST: method = "POST"; break;
  case HTTP_METHOD_DELETE: method = "DELETE"; break;
  case HTTP_METHOD_PUT: method = "PUT"; break;
  case HTTP_METHOD_PATCH: method = "PATCH"; break;
  default: return fail(response, HTTP_BAD_ARGUMENT);
  }
  bool has_body = http_method_has_body(request->method);
  if (!has_body && (request->body != HANDLE_INVALID || request->body_length)) {
    return fail(response, HTTP_BAD_ARGUMENT);
  }
  size_t generated = has_body ? 4 : 3;
  if (request->header_count > HTTP_REQUEST_FIELDS_MAX - generated) {
    return fail(response, HTTP_LIMIT);
  }
  char initial[HTTP_URI_MAX + 384];
  int written = snprintf(initial, sizeof(initial),
      "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\nAccept-Encoding: identity\r\n",
      method, uri->target, uri->authority);
  if (written < 0 || (size_t)written >= sizeof(initial)) {
    return fail(response, HTTP_LIMIT);
  }
  size_t length = (size_t)written;
  if (has_body) {
    written = snprintf(initial + length, sizeof(initial) - length,
        "Content-Length: %llu\r\n", (unsigned long long)request->body_length);
    if (written < 0 || (size_t)written >= sizeof(initial) - length) {
      return fail(response, HTTP_LIMIT);
    }
    length += written;
  }
  size_t initial_size = length;
  length += 2;
  for (size_t i = 0; i < request->header_count; ++i) {
    const struct http_request_header *header = &request->headers[i];
    if (!header->name || !header->name_length || (header->value_length && !header->value)) {
      return fail(response, HTTP_BAD_ARGUMENT);
    }
    if (header->name_length > HTTP_REQUEST_HEADERS_MAX ||
        header->value_length > HTTP_REQUEST_HEADERS_MAX ||
        header->name_length + header->value_length + 4 > HTTP_REQUEST_HEADERS_MAX - length) {
      return fail(response, HTTP_LIMIT);
    }
    for (size_t j = 0; j < header->name_length; ++j) {
      if (!http_token((unsigned char)header->name[j])) {
        return fail(response, HTTP_BAD_ARGUMENT);
      }
    }
    for (size_t j = 0; j < header->value_length; ++j) {
      unsigned char byte = header->value[j];
      if ((byte < ' ' && byte != '\t') || byte == 127) {
        return fail(response, HTTP_BAD_ARGUMENT);
      }
    }
    if (owned_header(header)) {
      return fail(response, HTTP_UNSUPPORTED);
    }
    for (size_t j = 0; j < i; ++j) {
      if (same_name(header, &request->headers[j])) {
        return fail(response, HTTP_BAD_ARGUMENT);
      }
    }
    length += header->name_length + header->value_length + 4;
  }
  if (has_body && check_source) {
    struct handle_info info;
    if (!source(response, handle_query(request->body, &info))) {
      return false;
    }
    if (info.kind != HANDLE_KIND_NATIVE || info.protocol != PROTOCOL_FILE || info.transport) {
      return source(response, CALL_BAD_OPERATION);
    }
    if (!(info.rights & FILE_RIGHT_READ)) {
      return source(response, CALL_DENIED);
    }
    uint64_t size;
    if (!source(response, file_size(request->body, &size)) ||
        !source(response, size == request->body_length ? CALL_OK : CALL_BAD_REQUEST)) {
      return false;
    }
  }
  char *bytes = malloc(length);
  if (!bytes) {
    return fail(response, HTTP_NO_MEMORY);
  }
  size_t offset = initial_size;
  memcpy(bytes, initial, offset);
  for (size_t i = 0; i < request->header_count; ++i) {
    const struct http_request_header *header = &request->headers[i];
    memcpy(bytes + offset, header->name, header->name_length);
    offset += header->name_length;
    memcpy(bytes + offset, ": ", 2);
    offset += 2;
    if (header->value_length) {
      memcpy(bytes + offset, header->value, header->value_length);
      offset += header->value_length;
    }
    memcpy(bytes + offset, "\r\n", 2);
    offset += 2;
  }
  memcpy(bytes + offset, "\r\n", 2);
  outgoing->headers = bytes;
  outgoing->header_size = length;
  outgoing->body = request->body;
  outgoing->body_length = request->body_length;
  return true;
}

static bool receive_headers_step(struct http_stream *stream, uint64_t *wanted)
{
  if (!http_stream_check_deadline(stream)) {
    return false;
  }
  size_t remaining = stream->limit.header_bytes - stream->header_bytes;
  if (stream->buffered >= remaining) {
    return fail(stream->result, HTTP_LIMIT);
  }
  size_t capacity = remaining - stream->buffered;
  if (capacity > stream->header_receive_max) {
    capacity = stream->header_receive_max;
  }
  size_t count;
  bool eof;
  if (stream->tls) {
    enum tls_step_result step = tls_read_step(stream->tls, stream->bytes + stream->buffered,
        capacity, &count, &stream->result->tls_failure);
    if (step == TLS_STEP_FAILED) {
      return fail(stream->result, HTTP_TLS_ERROR);
    }
    if (step == TLS_STEP_WANT_READ || step == TLS_STEP_WANT_WRITE) {
      *wanted = step == TLS_STEP_WANT_READ ? WAIT_READABLE : WAIT_WRITABLE;
      return true;
    }
    eof = step == TLS_STEP_EOF;
  } else {
    struct tcp_read_reply reply;
    enum call_status status = tcp_try_read(stream->stream,
        stream->bytes + stream->buffered, capacity, &reply);
    if (status == CALL_WOULD_BLOCK) {
      *wanted = WAIT_READABLE;
      return true;
    }
    if (!network(stream, status)) {
      return false;
    }
    count = reply.length;
    eof = count == 0;
  }
  if (eof) {
    return fail(stream->result, HTTP_BAD_RESPONSE);
  }
  stream->buffered += count;
  *wanted = 0;
  return true;
}

static bool send_step(struct http_stream *stream, const char *bytes, size_t size,
    size_t *accepted, uint64_t *wanted)
{
  *accepted = 0;
  if (!http_stream_check_deadline(stream)) {
    return false;
  }
  stream->result->request_started = true;
  if (stream->tls) {
    enum tls_step_result step = tls_write_step(stream->tls, bytes, size,
        accepted, &stream->result->tls_failure);
    if (step == TLS_STEP_FAILED) {
      return fail(stream->result, HTTP_TLS_ERROR);
    }
    if (step == TLS_STEP_WANT_READ || step == TLS_STEP_WANT_WRITE) {
      *wanted = step == TLS_STEP_WANT_READ ? WAIT_READABLE : WAIT_WRITABLE;
      return true;
    }
  } else {
    struct tcp_write_reply reply;
    enum call_status status = tcp_try_write(stream->stream, bytes, size, &reply);
    if (status == CALL_WOULD_BLOCK) {
      *wanted = WAIT_WRITABLE;
      return true;
    }
    if (!network(stream, status)) {
      return false;
    }
    *accepted = reply.length;
  }
  *wanted = 0;
  return *accepted > 0 || network(stream, CALL_IO);
}

static bool park(struct http_stream *stream, uint64_t wanted)
{
  uint64_t deadline;
  if (!http_deadline(stream, WAIT_MAX_WAIT_NS, &deadline)) {
    return false;
  }
  struct wait_interest interest = {.handle = stream->stream, .events = wanted};
  uint64_t events;
  enum call_status status = wait_many(&interest, 1, deadline, &events);
  return status == CALL_TIMED_OUT ? http_stream_check_deadline(stream) : network(stream, status);
}

bool http_exchange_request(struct http_stream *stream, const struct http_outgoing *outgoing,
    struct framing *framing)
{
  char body[HTTP_READ_MAX];
  size_t header_sent = 0, loaded = 0, body_sent = 0;
  uint64_t read_wanted = 0, write_wanted = 0;
  bool success = false;
  for (;;) {
    if (!http_stream_check_deadline(stream)) {
      break;
    }
    int parsed = http_parse_response_headers(stream, framing);
    if (parsed < 0) {
      break;
    }
    if (parsed > 0) {
      success = !stream->tls || stream->result->request_sent ||
          tls_stop_write(stream->tls, &stream->result->tls_failure);
      if (!success) {
        fail(stream->result, HTTP_TLS_ERROR);
      }
      break;
    }
    if (!receive_headers_step(stream, &read_wanted)) {
      break;
    }
    if (!read_wanted) {
      continue;
    }
    /* A read-owned TLS alert must finish before another application write. */
    if (read_wanted != WAIT_WRITABLE && !stream->result->request_sent) {
      const char *bytes;
      size_t size;
      bool headers = header_sent < outgoing->header_size;
      if (headers) {
        bytes = outgoing->headers + header_sent;
        size = outgoing->header_size - header_sent;
      } else {
        if (body_sent == loaded && stream->result->upload_bytes < outgoing->body_length) {
          size = outgoing->body_length - stream->result->upload_bytes;
          if (size > sizeof(body)) {
            size = sizeof(body);
          }
          if (!source(stream->result, file_read(outgoing->body, stream->result->upload_bytes,
              body, size, &loaded)) || !source(stream->result, loaded ? CALL_OK : CALL_IO)) {
            break;
          }
          body_sent = 0;
        }
        bytes = body + body_sent;
        size = loaded - body_sent;
      }
      if (!size) {
        stream->result->request_sent = true;
        continue;
      }
      if (size > TCP_WRITE_MAX_BYTES) {
        size = TCP_WRITE_MAX_BYTES;
      }
      size_t accepted;
      if (!send_step(stream, bytes, size, &accepted, &write_wanted)) {
        break;
      }
      if (accepted) {
        if (headers) {
          header_sent += accepted;
        } else {
          body_sent += accepted;
          stream->result->upload_bytes += accepted;
        }
        stream->result->request_sent = header_sent == outgoing->header_size &&
            stream->result->upload_bytes == outgoing->body_length;
        continue;
      }
    }
    uint64_t wanted = read_wanted;
    if (!stream->result->request_sent && read_wanted != WAIT_WRITABLE) {
      wanted |= write_wanted;
    }
    if (!park(stream, wanted)) {
      break;
    }
  }
  if (!success && stream->tls) {
    /* Pending writes borrow this stack buffer until connection retirement. */
    tls_connection_free(stream->tls);
    stream->tls = NULL;
  }
  erase(body, sizeof(body));
  return success;
}


bool http_request_validate(const struct http_client *client, const char *text,
    const struct http_request *request, struct http_response *response)
{
  *response = (struct http_response){.state = HTTP_RESPONSE_FAILED};
  if (!client || !text) {
    return fail(response, HTTP_BAD_ARGUMENT);
  }
  struct http_uri uri;
  response->error = http_parse_uri(text, &uri);
  if (response->error != HTTP_OK) {
    return false;
  }
  if (uri.scheme != client->scheme) {
    return fail(response, HTTP_UNSUPPORTED);
  }
  struct http_outgoing outgoing;
  if (!http_prepare_request(request, &uri, &outgoing, response, false)) {
    return false;
  }
  http_release_request(&outgoing);
  return true;
}

enum http_step_result http_exchange_step(struct http_stream *stream, struct framing *framing)
{
  uint64_t read_wanted = 0, write_wanted = 0;
  int parsed = http_parse_response_headers(stream, framing);
  if (parsed < 0) {
    return HTTP_STEP_FAILED;
  }
  if (parsed > 0) {
    if (stream->tls && !stream->result->request_sent &&
        !tls_stop_write(stream->tls, &stream->result->tls_failure)) {
      fail(stream->result, HTTP_TLS_ERROR);
      return HTTP_STEP_FAILED;
    }
    erase(stream->body_bytes, sizeof(stream->body_bytes));
    return HTTP_STEP_HEADERS;
  }
  if (!receive_headers_step(stream, &read_wanted)) {
    return HTTP_STEP_FAILED;
  }
  if (!read_wanted) {
    return HTTP_STEP_PROGRESS;
  }
  /* A pending source read does not prevent early response reception. */
  if (read_wanted != WAIT_WRITABLE && !stream->result->request_sent && !stream->pending) {
    bool headers = stream->header_sent < stream->outgoing.header_size;
    const char *bytes;
    size_t size;
    if (headers) {
      bytes = stream->outgoing.headers + stream->header_sent;
      size = stream->outgoing.header_size - stream->header_sent;
    } else {
      if (stream->body_sent == stream->body_loaded &&
          stream->result->upload_bytes < stream->outgoing.body_length) {
        uint64_t remaining = stream->outgoing.body_length - stream->result->upload_bytes;
        struct file_read_request request = {
          .offset = stream->result->upload_bytes,
          .capacity = remaining < sizeof(stream->body_bytes) ? remaining : sizeof(stream->body_bytes),
        };
        if (!http_submit(stream, stream->outgoing.body, PROTOCOL_FILE, FILE_READ,
            TCP_READ_MAX_WAIT_NS, &request, sizeof(request))) {
          return HTTP_STEP_FAILED;
        }
        stream->wanted = read_wanted;
        return HTTP_STEP_WAIT;
      }
      bytes = stream->body_bytes + stream->body_sent;
      size = stream->body_loaded - stream->body_sent;
    }
    if (!size) {
      stream->result->request_sent = true;
      return HTTP_STEP_PROGRESS;
    }
    if (size > TCP_WRITE_MAX_BYTES) {
      size = TCP_WRITE_MAX_BYTES;
    }
    size_t accepted;
    if (!send_step(stream, bytes, size, &accepted, &write_wanted)) {
      return HTTP_STEP_FAILED;
    }
    if (accepted) {
      if (headers) {
        stream->header_sent += accepted;
        if (stream->header_sent == stream->outgoing.header_size) {
          http_release_headers(&stream->outgoing);
        }
      } else {
        stream->body_sent += accepted;
        stream->result->upload_bytes += accepted;
        if (stream->body_sent == stream->body_loaded) {
          erase(stream->body_bytes, sizeof(stream->body_bytes));
        }
      }
      stream->result->request_sent = stream->header_sent == stream->outgoing.header_size &&
          stream->result->upload_bytes == stream->outgoing.body_length;
      return HTTP_STEP_PROGRESS;
    }
  }
  stream->wanted = read_wanted;
  if (!stream->result->request_sent && read_wanted != WAIT_WRITABLE && !stream->pending) {
    stream->wanted |= write_wanted;
  }
  return HTTP_STEP_WAIT;
}
