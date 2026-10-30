#ifndef USERSPACE_HTTP_H
#define USERSPACE_HTTP_H

#include <abi/handle.h>
#include <abi/endpoint.h>
#include <http_types.h>
#include <abi/syscall.h>
#include <http_url.h>
#include <stddef.h>
#include <stdint.h>
#include "../libtls/tls.h"

#define HTTP_MEDIA_TYPE_MAX 127
#define HTTP_READ_MAX 4096
#define HTTP_REQUEST_HEADERS_MAX 32768
#define HTTP_REQUEST_FIELDS_MAX 256

/* Borrowed explicit authorities and a numeric DNS server, all host-order.
 * UDP/random/server are only needed for names, not numeric IPv4 addresses. */
struct http_authority {
  handle_t tcp, udp, random, clock;
  uint32_t dns_server;
};

/* The expected scheme is explicit; mismatching URIs never reach the network.
 * HTTPS borrows a ready runtime with immutable trust. Keep it and the authority
 * alive until the stream closes. HTTP needs no runtime and does not use one. */
struct http_client {
  struct http_authority authority;
  enum http_scheme scheme;
  struct tls_runtime *tls;
};

enum http_error {
  HTTP_OK,
  HTTP_INVALID_URI,
  HTTP_INVALID_LOCATION,
  HTTP_UNSUPPORTED,
  HTTP_BAD_RESPONSE,
  HTTP_REJECTED_STATUS,
  HTTP_LIMIT,
  HTTP_QUOTA,
  HTTP_NO_MEMORY,
  HTTP_NETWORK_ERROR,
  HTTP_DNS_ERROR,
  HTTP_TLS_ERROR,
  HTTP_ABORTED,
  HTTP_BAD_ARGUMENT,
  HTTP_SOURCE_ERROR,
};

enum http_response_state {
  HTTP_RESPONSE_PENDING,
  HTTP_RESPONSE_COMPLETE,
  HTTP_RESPONSE_FAILED,
};

/* idle_ns, when nonzero, makes the request's deadline an inactivity window:
 * the deadline passed to the request then bounds the whole transfer (the
 * ceiling), and the transfer also fails when no data has arrived for idle_ns.
 * Data that arrives moves the window forward, so a slow transfer that stays
 * alive has no time limit except the ceiling. */
struct http_response_limits {
  uint64_t header_bytes, fields, informational;
  uint64_t idle_ns;
};

/* Counts describe consumed framing and delivered bytes, never an allowance.
 * HTTP status is independent of framing success, including 4xx/5xx. */
struct http_response {
  enum http_response_state state;
  enum http_error error;
  struct provider_http_budget consumed;
  enum call_status network_status, close_status;
  unsigned status, dns_rcode;
  struct tls_result tls_failure, tls_cleanup;
  bool tls_cleanup_attempted;
  bool length_present, invalid_location;
  uint64_t length;
  enum call_status source_status;
  bool request_started, request_sent;
  uint64_t upload_bytes;
  char media_type[HTTP_MEDIA_TYPE_MAX + 1];
  char location[HTTP_URI_MAX + 1];
};

struct http_stream;
/* The current deadline of STREAM at time NOW, after any inactivity window has
 * moved forward for data received since the last call. */
uint64_t http_stream_deadline(struct http_stream *stream, uint64_t now);

enum http_method {
  HTTP_METHOD_GET = 0,
  HTTP_METHOD_POST = 1,
  HTTP_METHOD_DELETE = 2,
  HTTP_METHOD_PUT = 3,
  HTTP_METHOD_PATCH = 4,
};

static inline bool http_method_has_body(enum http_method method)
{
  return method == HTTP_METHOD_POST || method == HTTP_METHOD_PUT || method == HTTP_METHOD_PATCH;
}

struct http_request_header {
  const char *name, *value;
  size_t name_length, value_length;
};

/* Headers are bounded wire fields, not C strings. POST, PUT and PATCH borrow a
 * native FILE with READ and its exact frozen size; read from offset zero without
 * changing any shared position. The caller owns freezing and keeps it
 * valid/unchanged through open. GET and DELETE have no body. No provider
 * authority is acquired here. */
struct http_request {
  enum http_method method;
  const struct http_request_header *headers;
  size_t header_count;
  handle_t body;
  uint64_t body_length;
};

/* One request, no redirect/reconnect/replay. Validate all headers and body
 * authority/size before connecting. Copy headers privately and erase the copy
 * after sending or failure; input fields are borrowed only through open.
 * Zero deadline selects 30 seconds; an explicit absolute deadline may be longer.
 * DNS/connect/upload/response share it; native waits stay bounded separately.
 * An early final response stops upload and retains its status/body. Started
 * means sending was attempted; sent/upload_bytes report known local acceptance,
 * never remote execution or acknowledgment. Failure after start can be unknown
 * even with zero reported bytes. The stream/response ownership is as below. */
struct http_stream *http_request_open(const struct http_client *client, const char *uri,
    const struct http_request *request, uint64_t deadline_ns,
    const struct http_response_limits *limits, struct http_response *response);

/* Local validation only: no native calls, body authority/size check or I/O.
 * POST/PUT/PATCH body fields are ignored here; GET and DELETE must have no body. */
bool http_request_validate(const struct http_client *client, const char *uri,
    const struct http_request *request, struct http_response *response);

enum http_step_result {
  HTTP_STEP_PROGRESS,
  HTTP_STEP_HEADERS,
  HTTP_STEP_DATA,
  HTTP_STEP_COMPLETE,
  HTTP_STEP_FAILED,
  HTTP_STEP_WAIT,
};

struct http_wait {
  handle_t stream;
  uint64_t events, correlation_id, deadline_ns;
};

/* Copies/validates request fields without network or storage I/O. Borrow receiver,
 * increasing next_id counter, runtime, body and response until close. One native
 * completion is admitted at a time; the provider must also serialize its own
 * staging completions. Random acquisition remains synchronous and bounded.
 * HEADERS publishes response metadata once; DATA requires positive capacity.
 * Zero capacity pauses body consumption after headers, retaining backpressure.
 * PROGRESS needs another scheduled step; WAIT exposes native readiness/completion
 * and the original deadline. No step waits for network or storage progress. */
struct http_stream *http_request_start(const struct http_client *client, const char *uri,
    const struct http_request *request, uint64_t deadline_ns,
    const struct http_response_limits *limits, handle_t receiver,
    uint64_t *next_id, struct http_response *response);
enum http_step_result http_stream_step(struct http_stream *stream, void *bytes,
    size_t capacity, size_t *count, struct http_wait *wait);

/* Matching completion transfers receipt/attachments to the engine; it copies or
 * adopts outputs before ACK. Unrelated packets remain owned by the caller.
 * Abort requests cancellation; pending ownership must drain before close.
 * Close returns CALL_BUSY without freeing while pending() is nonzero. */
bool http_stream_completion(struct http_stream *stream,
    const struct endpoint_packet *packet);
uint64_t http_stream_pending(const struct http_stream *stream);
size_t http_stream_read_ahead(const struct http_stream *stream);
enum call_status http_stream_abort(struct http_stream *stream);

enum http_read_result {
  HTTP_READ_DATA,
  HTTP_READ_COMPLETE,
  HTTP_READ_FAILED,
  HTTP_READ_WAIT, /* Cooperative decoder yield, exposed by stream_step as WAIT. */
};

/* One synchronous GET through headers, no redirect/replay. NULL limits select
 * bounded ordinary metadata limits. Bodies have no total-byte allowance.
 * Zero deadline selects 30 seconds; an explicit absolute deadline may be longer.
 * RESPONSE is caller-owned: observe without modifying it while live, and retain
 * it until close; failure initializes it
 * and returns NULL. Borrowed handles/runtime must remain valid until close.
 * A returned stream owns bounded scratch, not response-body storage. */
struct http_stream *http_get_open(const struct http_client *client, const char *uri,
    uint64_t deadline_ns, const struct http_response_limits *limits,
    struct http_response *response);

/* Positive-capacity caller buffer/count are required. DATA returns 1..HTTP_READ_MAX
 * copied bytes and never implies completion. COMPLETE/FAILED return zero bytes
 * with a retained final response; repeated terminal reads do no I/O. Fixed and
 * chunked completion includes trailers/cleanup; close-delimited HTTPS requires
 * authenticated TLS EOF. A prefix remains delivered after later failure. */
enum http_read_result http_stream_read(struct http_stream *stream, void *bytes,
    size_t capacity, size_t *count);

/* Frees the object; an unfinished response becomes failed/aborted, not complete.
 * Returns local native cleanup status, separate from the retained HTTP result.
 * Completed/failed transfers have already closed their transport. */
enum call_status http_stream_close(struct http_stream *stream);

struct http_result {
  enum http_error error;
  struct provider_http_budget consumed;
  enum call_status network_status;
  unsigned status; /* Final HTTP status, or zero if none was received. */
  unsigned dns_rcode;
  struct tls_result tls_failure;
  struct tls_result tls_cleanup; /* Diagnostic only after complete framing. */
  bool tls_cleanup_attempted;
  char media_type[HTTP_MEDIA_TYPE_MAX + 1];
  char location[HTTP_URI_MAX + 1]; /* Validated URI reference, never a body. */
};

const char *http_error_name(enum http_error error);
enum call_status http_result_status(const struct http_result *result);

#endif
