#ifndef USERSPACE_HTTP_TYPES_H
#define USERSPACE_HTTP_TYPES_H

#include <abi/describe.h>

#define PROVIDER_MEDIA_TYPE_MAX_BYTES RESOURCE_MEDIA_TYPE_MAX_BYTES
#define PROVIDER_HTTP_URI_MAX_BYTES 2048
#define PROVIDER_HTTP_HOST_MAX_BYTES 255
#define PROVIDER_HTTP UINT64_C(0)
#define PROVIDER_HTTPS UINT64_C(1)

/* Parsed initial origin, not destination authority. host is lowercase, with
 * no terminal DNS dot; port is the effective numeric port. */
struct provider_http_origin {
  uint64_t scheme;
  uint64_t port;
  char host[PROVIDER_HTTP_HOST_MAX_BYTES + 1];
};

/* Decoded body and framing charges, independent of transport authority. */
struct provider_http_budget {
  uint64_t body_bytes;
  uint64_t header_bytes;
  uint64_t fields;
  uint64_t informational;
};

#endif
