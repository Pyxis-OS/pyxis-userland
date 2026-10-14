#ifndef USERSPACE_PROVIDER_HTTP_PRIVATE_H
#define USERSPACE_PROVIDER_HTTP_PRIVATE_H

#include <abi/provider_http_private.h>
#include <abi/handle.h>
#include <abi/syscall.h>
#include <stddef.h>
#include <http_url.h>
#include <pyxis/response.h>

struct provider_legacy_http_metadata {
  uint64_t protocol;
  uint64_t representation;
  size_t media_type_size;
  char media_type[PROVIDER_MEDIA_TYPE_MAX_BYTES + 1];
};

struct provider_legacy_http_result {
  enum call_status status;
  uint64_t provider_status;
  uint64_t delivery;
  struct provider_legacy_http_metadata metadata;
  uint64_t outcome;
  struct provider_http_budget consumed;
  char location[HTTP_URI_MAX + 1];
};

/* Private HTTP continuation, removed by B6. Context is copied; uri/rights/size
 * replace its fields. NULL supplies the old default context. BYTES owns one
 * FILE; REDIRECT retains Location and charges with no grant. Other outcomes
 * clear file and close attachments. No generic OPEN fallback, retry or allocation.
 * Only CALL_OK makes result status/HTTP detail meaningful; delivery survives
 * transport/local validation failure. The absolute deadline is not restarted. */
enum call_status provider_legacy_http_open(handle_t provider, const char *uri,
    uint64_t rights, uint64_t deadline_ns,
    const struct provider_legacy_http_open_request *context,
    struct provider_legacy_http_result *result, handle_t *file);

bool provider_http_uri(const char *path);

struct path_context;
struct provider_http_workspace {
  char current[HTTP_URI_MAX + 1];
  char next[HTTP_URI_MAX + 1];
  char keys[HTTP_REDIRECT_MAX + 1][HTTP_URI_MAX + 2];
  struct http_uri parsed;
  struct provider_legacy_http_open_request request;
  struct provider_legacy_http_result hop;
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
