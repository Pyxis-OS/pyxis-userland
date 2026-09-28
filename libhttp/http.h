#ifndef USERSPACE_HTTP_H
#define USERSPACE_HTTP_H

#include <abi/handle.h>
#include <abi/syscall.h>
#include <stddef.h>
#include <stdint.h>
#include "../libtls/tls.h"

#define HTTP_URI_MAX 2048
#define HTTP_HEADERS_MAX 32768
#define HTTP_FIELDS_MAX 256
#define HTTP_INFORMATIONAL_MAX 8
#define HTTP_BODY_MAX (16 * 1024 * 1024)
#define HTTP_STORAGE_MAX (64 * 1024 * 1024)
#define HTTP_FETCH_NS UINT64_C(30000000000)
#define HTTP_MEDIA_TYPE_MAX 127

/* Borrowed explicit authorities and a numeric DNS server, all host-order.
 * UDP/random/server are only needed for names, not numeric IPv4 addresses. */
struct http_authority {
  handle_t tcp, udp, random, clock;
  uint32_t dns_server;
};

enum http_scheme {
  HTTP_SCHEME_HTTP,
  HTTP_SCHEME_HTTPS,
};

/* The expected scheme is explicit; mismatching URIs never reach the network.
 * HTTPS borrows a ready runtime with immutable trust. Keep it and the authority
 * alive until fetch returns. HTTP needs no runtime and does not use one. */
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
  HTTP_UNSUPPORTED,
  HTTP_BAD_RESPONSE,
  HTTP_REJECTED_STATUS,
  HTTP_LIMIT,
  HTTP_QUOTA,
  HTTP_NO_MEMORY,
  HTTP_NETWORK_ERROR,
  HTTP_DNS_ERROR,
  HTTP_TLS_ERROR,
};

struct http_result {
  enum http_error error;
  enum call_status network_status;
  unsigned status; /* Final HTTP status, or zero if none was received. */
  unsigned dns_rcode;
  struct tls_result tls_failure;
  struct tls_result tls_cleanup; /* Diagnostic only after complete framing. */
  char media_type[HTTP_MEDIA_TYPE_MAX + 1];
  struct http_body body; /* Owned only on success; no partial body on failure. */
};

/* One GET, no replay. deadline_ns is an optional absolute monotonic cap; zero
 * selects the 30-second overall budget. This initializes result; release any
 * previous successful body before reusing it. No printing or startup lookup.
 * Numeric HTTPS hosts are unsupported. Framed responses complete without peer
 * shutdown; close-delimited HTTPS needs authenticated TLS EOF. Cleanup preserves
 * the first fetch failure and status. A native handle-close failure invalidates
 * success; local TLS notification failure is retained as a diagnostic only. */
void http_fetch(const struct http_client *client, struct http_storage *storage,
    const char *uri, uint64_t deadline_ns, struct http_result *result);
void http_body_release(struct http_body *body);
const char *http_error_name(enum http_error error);
enum call_status http_result_status(const struct http_result *result);

#endif
