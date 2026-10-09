#ifndef USERSPACE_HTTP_URL_H
#define USERSPACE_HTTP_URL_H

#include <abi/provider.h>
#include <abi/syscall.h>
#include <stdbool.h>
#include <stdint.h>

#define HTTP_REDIRECT_MAX 10
#define HTTP_URI_MAX PROVIDER_HTTP_URI_MAX_BYTES
/* Empty request paths acquire one slash in comparison keys. */
#define HTTP_URL_KEY_BYTES (HTTP_URI_MAX + 2)
#define HTTP_HEADERS_MAX 32768
#define HTTP_FIELDS_MAX 256
#define HTTP_INFORMATIONAL_MAX 8
#define HTTP_BODY_MAX (16 * 1024 * 1024)
#define HTTP_FETCH_NS UINT64_C(30000000000)

enum http_scheme { HTTP_SCHEME_HTTP = PROVIDER_HTTP, HTTP_SCHEME_HTTPS = PROVIDER_HTTPS };

struct http_uri {
  enum http_scheme scheme;
  char host[256];
  char authority[262];
  char target[HTTP_URI_MAX + 2];
  uint16_t port;
};

/* No allocation, transport or authority discovery. Output is cleared on error.
 * Inputs and output must be disjoint. Keys normalize for comparison
 * only; outgoing encoded path/query bytes remain unchanged. */
enum call_status http_url_parse(const char *text, struct http_uri *uri);
enum call_status http_url_resolve(const char *base, const char *location,
    char output[HTTP_URI_MAX + 1]);
enum call_status http_url_key(const char *text, char output[HTTP_URL_KEY_BYTES]);

#endif
