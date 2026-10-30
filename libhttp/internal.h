#ifndef USERSPACE_HTTP_INTERNAL_H
#define USERSPACE_HTTP_INTERNAL_H

#include "http.h"
#include "../common/dns.h"
#include <stdbool.h>
#include <abi/file.h>
#include <picohttpparser.h>

enum body_stage {
  BODY_FIXED,
  BODY_CLOSE,
  BODY_CHUNK_SIZE,
  BODY_CHUNK_DATA,
  BODY_CHUNK_END,
  BODY_TRAILERS,
  BODY_FRAMED,
};

enum exchange_stage {
  EXCHANGE_SIZE, EXCHANGE_RESOLVE, EXCHANGE_CONNECT, EXCHANGE_TLS,
  EXCHANGE_REQUEST, EXCHANGE_BODY,
};

struct http_outgoing {
  char *headers;
  size_t header_size;
  handle_t body;
  uint64_t body_length;
};

struct http_stream {
  struct http_authority authority;
  struct http_response *result;
  handle_t stream;
  struct tls_connection *tls;
  uint64_t deadline, remaining;
  /* An inactivity window (idle, ceiling): deadline is the earlier of ceiling
   * and the last progress plus idle; progressed notes data received since. */
  uint64_t idle, ceiling;
  bool progressed;
  struct http_response_limits limit;
  size_t header_receive_max, header_previous, buffered, header_bytes, fields;
  enum body_stage stage;
  char bytes[HTTP_HEADERS_MAX];
  struct phr_header headers[HTTP_FIELDS_MAX];
  bool cooperative, yielded, headers_ready, aborting;
  unsigned receive_steps;
  uint64_t wanted, pending, pending_operation;
  handle_t receiver;
  uint64_t *next_id;
  struct http_uri uri;
  struct tls_runtime *runtime;
  enum exchange_stage exchange_stage;
  struct http_outgoing outgoing;
  struct dns_query_state dns;
  uint32_t address;
  size_t header_sent, body_loaded, body_sent;
  char body_bytes[FILE_READ_MAX_BYTES];
};

struct framing {
  bool length_present, chunked;
  uint64_t length;
};

bool http_deadline(struct http_stream *stream, uint64_t maximum, uint64_t *deadline);
int http_parse_response_headers(struct http_stream *stream, struct framing *framing);
bool http_prepare_request(const struct http_request *request, const struct http_uri *uri,
    struct http_outgoing *outgoing, struct http_response *response, bool check_source);
void http_release_headers(struct http_outgoing *outgoing);
void http_release_request(struct http_outgoing *outgoing);
enum http_step_result http_exchange_step(struct http_stream *stream, struct framing *framing);
bool http_submit(struct http_stream *stream, handle_t target, uint64_t protocol,
    uint64_t operation, uint64_t maximum, const void *payload, size_t size);
bool http_exchange_request(struct http_stream *stream, const struct http_outgoing *outgoing,
    struct framing *framing);

enum http_error http_parse_uri(const char *text, struct http_uri *uri);
bool http_equal(const char *value, size_t length, const char *literal);
bool http_token(unsigned char byte);

/* Snapshot compatibility: bound header read-ahead independently of body
 * delivery, and account bytes discarded at a redirect handoff. */
struct http_stream *http_get_open_prefetch(const struct http_client *client,
    const char *uri, uint64_t deadline_ns, const struct http_response_limits *limits,
    size_t header_receive_max, struct http_response *response);
size_t http_stream_read_ahead(const struct http_stream *stream);
bool http_stream_check_deadline(struct http_stream *stream);

#endif
