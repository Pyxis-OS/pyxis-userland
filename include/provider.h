#ifndef USERSPACE_PROVIDER_H
#define USERSPACE_PROVIDER_H

#include <abi/provider.h>
#include <abi/handle.h>
#include <abi/syscall.h>
#include <stddef.h>
#include <http_url.h>
#include <pyxis/response.h>

struct provider_metadata {
  uint64_t protocol;
  uint64_t representation;
  size_t media_type_size;
  char media_type[PROVIDER_MEDIA_TYPE_MAX_BYTES + 1];
};

struct provider_result {
  enum call_status status;
  uint64_t provider_status;
  uint64_t delivery;
  struct provider_metadata metadata;
  uint64_t outcome;
  struct provider_http_budget consumed;
  char location[HTTP_URI_MAX + 1];
};

/* OPEN sends the complete URI unchanged and requests exact FILE rights.
 * The return value reports transport or local validation failure. Only CALL_OK
 * makes result->status and provider_status meaningful, including provider errors.
 * delivery retains the endpoint's delivery state even when validation fails.
 * BYTES owns one exported byte-file grant with CALL transport; REDIRECT has
 * no grant, and retains its bounded Location and charges. Its
 * metadata copies and NUL-terminates the media type. Other outcomes clear file
 * and metadata and close every returned grant. result and file are required.
 * deadline_ns is absolute monotonic time; zero waits without a deadline.
 * Malformed application replies report BAD_REQUEST. No retry or allocation. */
enum call_status provider_open(handle_t provider, const char *uri, uint64_t rights,
    uint64_t deadline_ns, struct provider_result *result, handle_t *file);

/* Context is copied into the request; uri/rights/size replace those fields.
 * NULL is generic OPEN. HTTP context is continuity/accounting, not authority. */
enum call_status provider_open_context(handle_t provider, const char *uri,
    uint64_t rights, uint64_t deadline_ns, const struct provider_open_request *context,
    struct provider_result *result, handle_t *file);

bool provider_http_uri(const char *path);

struct path_context;
struct provider_http_workspace {
  char current[HTTP_URI_MAX + 1];
  char next[HTTP_URI_MAX + 1];
  char keys[HTTP_REDIRECT_MAX + 1][HTTP_URI_MAX + 2];
  struct http_uri parsed;
  struct provider_open_request request;
  struct provider_result hop;
};

/* Borrows initial_provider; owns other held bindings until return. Caller owns
 * disjoint scratch. deadline_ns must be the already-started absolute deadline.
 * No allocation or provider/transport retry; failure clears file/info. */
enum call_status provider_http_open(const struct path_context *context,
    handle_t initial_provider, const char *uri, uint64_t rights, handle_t clock,
    uint64_t deadline_ns,
    struct provider_http_workspace *workspace, struct pyxis_response_info *info,
    handle_t *file);

#endif
