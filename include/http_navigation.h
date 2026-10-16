#ifndef USERSPACE_HTTP_NAVIGATION_H
#define USERSPACE_HTTP_NAVIGATION_H

#include <http_types.h>
#include <stddef.h>
#include <stdio.h>

#define HTTP_NAVIGATION_METADATA_MAX 4096u
#define HTTP_NAVIGATION_PREFIX_MAX (4u + HTTP_NAVIGATION_METADATA_MAX)

struct http_navigation {
  char effective_url[PROVIDER_HTTP_URI_MAX_BYTES + 1];
  char media_type[PROVIDER_MEDIA_TYPE_MAX_BYTES + 1];
  unsigned status;
};

#ifdef __cplusplus
extern "C" {
#endif

/* Explicit response content: LE32 JSON byte count, bounded UTF-8 JSON with
 * unique effective_url/media_type/status members, then uninterpreted body.
 * Encode includes the LE32 prefix; read consumes only that prefix and metadata.
 * No allocation, response seek or generic describe metadata is involved. */
enum call_status http_navigation_encode(const struct http_navigation *metadata,
    void *bytes, size_t capacity, size_t *size);
int http_navigation_read(FILE *stream, struct http_navigation *metadata);

/* Capture a named root once. HTTP GET configuration selects navigation content;
 * only a delivered, empty BAD_OPERATION refusal permits ordinary READ through
 * that same held root. All other failures are final. Native/relative files
 * remain ordinary files and return zero metadata. Requires only the caller's
 * declared root and, for a provider, its declared clock; never provisions grants.
 * Successful return owns one FILE positioned at its raw body. Failed resource
 * outcomes stay failures, even if diagnostic bytes exist. */
FILE *http_navigation_fopen(const char *path, struct http_navigation *metadata);

#ifdef __cplusplus
}
#endif

#endif
