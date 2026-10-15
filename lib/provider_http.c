#include <handle.h>
#include <http_transaction.h>
#include <http_url.h>
#include <environment.h>
#include <path.h>
#include <provider_http_private.h>
#include <startup.h>
#include <string.h>

bool provider_http_uri(const char *path)
{
  if (!path) {
    return false;
  }
  const char *prefix = "http";
  for (size_t i = 0; i < 4; ++i) {
    unsigned char byte = path[i];
    if (byte >= 'A' && byte <= 'Z') {
      byte += 'a' - 'A';
    }
    if (byte != (unsigned char)prefix[i]) {
      return false;
    }
  }
  size_t offset = path[4] == 's' || path[4] == 'S' ? 5 : 4;
  return !strncmp(path + offset, "://", 3);
}

static enum call_status binding(const struct path_context *context,
    enum http_scheme scheme, handle_t *provider)
{
  (void)context;
  const char *name = scheme == HTTP_SCHEME_HTTPS ? "https" : "http";
  return environment_capability(startup_environment_handle(), name, provider);
}

static enum call_status prepare_bound(const struct path_context *context,
    const char *uri, uint64_t method, uint64_t flags, const char *headers,
    size_t headers_size, uint64_t deadline_ns,
    struct http_prepared *prepared)
{
  struct http_uri parsed;
  enum call_status status = http_url_parse(uri, &parsed);
  if (status != CALL_OK) {
    return status;
  }
  handle_t provider = HANDLE_INVALID;
  status = binding(context, parsed.scheme, &provider);
  if (status != CALL_OK) {
    return status;
  }
  status = http_transaction_prepare(provider, uri, method, flags,
      headers, headers_size, deadline_ns, prepared);
  enum call_status closed = handle_close(provider);
  if (status == CALL_OK && closed != CALL_OK) {
    http_prepared_close(prepared);
    status = closed;
  }
  return status;
}

enum call_status http_transaction_prepare_uri(const struct path_context *context,
    const char *uri, uint64_t method, uint64_t flags, const char *headers,
    size_t headers_size, uint64_t deadline_ns, struct http_prepared *prepared)
{
  if (!prepared) {
    return CALL_BAD_REQUEST;
  }
  *prepared = (struct http_prepared){0};
  if (!uri || !deadline_ns || method > HTTP_PREPARE_POST ||
      (flags & ~HTTP_PREPARE_FLAGS) || (headers_size && !headers)) {
    return CALL_BAD_REQUEST;
  }
  if (!strncmp(uri, "json+", 5)) {
    uri += 5;
    flags |= HTTP_PREPARE_JSON;
  }
  return prepare_bound(context, uri, method, flags, headers, headers_size,
      deadline_ns, prepared);
}

enum call_status http_transaction_open_uri(const struct path_context *context,
    const char *uri, uint64_t deadline_ns, struct http_prepared *prepared,
    struct pyxis_response_info *info)
{
  if (info) {
    *info = (struct pyxis_response_info){0};
  }
  if (!prepared) {
    return CALL_BAD_REQUEST;
  }
  *prepared = (struct http_prepared){0};
  enum call_status status = http_transaction_prepare_uri(context, uri,
      HTTP_PREPARE_GET, 0, NULL, 0, deadline_ns, prepared);
  if (status != CALL_OK) {
    return status;
  }
  struct http_commit_reply committed;
  status = http_transaction_commit(prepared, deadline_ns, &committed);
  if (status != CALL_OK) {
    return status;
  }
  struct http_transaction_status response;
  status = http_transaction_status(prepared->observation, deadline_ns, &response);
  if (status != CALL_OK) {
    return status;
  }
  if (response.phase != HTTP_TRANSACTION_COMPLETE ||
      response.framing != HTTP_FRAMING_COMPLETE || response.status != CALL_OK ||
      response.http_status < 200 || response.http_status >= 300) {
    return response.status != CALL_OK ? response.status : CALL_IO;
  }
  if (info) {
    struct http_response_metadata metadata;
    status = http_transaction_metadata(prepared->observation, deadline_ns, &metadata);
    if (status != CALL_OK) {
      return status;
    }
    info->flags = PYXIS_RESPONSE_PROVIDER | PYXIS_RESPONSE_HTTP | PYXIS_RESPONSE_URL;
    info->status = response.http_status;
    memcpy(info->url, metadata.uri, sizeof(info->url));
    if (metadata.media_type[0]) {
      info->flags |= PYXIS_RESPONSE_MEDIA_TYPE;
      memcpy(info->media_type, metadata.media_type, sizeof(info->media_type));
    }
  }
  return CALL_OK;
}
