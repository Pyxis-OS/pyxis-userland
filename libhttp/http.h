#ifndef USERSPACE_HTTP_H
#define USERSPACE_HTTP_H

#include <abi/handle.h>
#include <abi/provider.h>
#include <abi/syscall.h>
#include <http_url.h>
#include <stddef.h>
#include <stdint.h>
#include "../libtls/tls.h"

#define HTTP_STORAGE_MAX (64 * 1024 * 1024)
#define HTTP_MEDIA_TYPE_MAX 127
#define HTTP_READ_MAX 4096

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

/* Zero initialize. Single-task accounting; keep alive until all bodies release.
 * Counts allocated body capacity, including both allocations during growth.
 * Fixed parser/request scratch is bounded separately, outside this budget. */
struct http_storage {
  size_t reserved;
};

struct http_body {
  unsigned char *data;
  size_t size, capacity;
  struct http_storage *storage;
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
};

enum http_response_state {
  HTTP_RESPONSE_PENDING,
  HTTP_RESPONSE_COMPLETE,
  HTTP_RESPONSE_FAILED,
};

struct http_response_limits {
  uint64_t header_bytes, fields, informational;
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
  bool length_present, invalid_location;
  uint64_t length;
  char media_type[HTTP_MEDIA_TYPE_MAX + 1];
  char location[HTTP_URI_MAX + 1];
};

struct http_stream;

enum http_read_result {
  HTTP_READ_DATA,
  HTTP_READ_COMPLETE,
  HTTP_READ_FAILED,
};

/* One synchronous GET through headers, no redirect/replay. NULL limits select
 * bounded ordinary metadata limits. Bodies have no total-byte allowance.
 * One overall 30-second/earlier-caller deadline remains until task 2.
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
  uint64_t outcome; /* PROVIDER_OUTCOME_BYTES or REDIRECT only on success. */
  struct provider_http_budget consumed;
  enum call_status network_status;
  unsigned status; /* Final HTTP status, or zero if none was received. */
  unsigned dns_rcode;
  struct tls_result tls_failure;
  struct tls_result tls_cleanup; /* Diagnostic only after complete framing. */
  char media_type[HTTP_MEDIA_TYPE_MAX + 1];
  char location[HTTP_URI_MAX + 1]; /* Validated URI reference, never a body. */
  struct http_body body; /* Owned only on success; no partial body on failure. */
};

/* One GET, no replay. HTTP context borrows the remaining chain budgets; NULL
 * selects the ordinary limits. Redirects stop after validated headers, retain
 * no body, and charge discarded body read-ahead before checked stream closure.
 * deadline_ns is an optional absolute monotonic cap; zero
 * selects the 30-second overall budget. This initializes result; release any
 * previous successful body before reusing it. No printing or startup lookup.
 * Numeric HTTPS hosts are unsupported. Framed responses complete without peer
 * shutdown; close-delimited HTTPS needs authenticated TLS EOF. Cleanup preserves
 * the first fetch failure and status. A native handle-close failure invalidates
 * success; local TLS notification failure is retained as a diagnostic only. */
void http_fetch(const struct http_client *client, struct http_storage *storage,
    const char *uri, uint64_t deadline_ns,
    const struct provider_open_request *request, struct http_result *result);
void http_body_release(struct http_body *body);
const char *http_error_name(enum http_error error);
enum call_status http_result_status(const struct http_result *result);

#endif
