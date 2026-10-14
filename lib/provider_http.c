#include <clock.h>
#include <handle.h>
#include <http_transaction.h>
#include <stdlib.h>
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

static bool same_origin(const struct provider_http_origin *origin,
    const struct http_uri *uri)
{
  return origin->scheme == (uint64_t)uri->scheme && origin->port == uri->port &&
      !strcmp(origin->host, uri->host);
}

static enum call_status binding(const struct path_context *context,
    enum http_scheme scheme, handle_t *provider)
{
  (void)context;
  const char *name = scheme == HTTP_SCHEME_HTTPS ? "https" : "http";
  return environment_capability(startup_environment_handle(), name, provider);
}

static void consume(struct provider_http_budget *remaining,
    const struct provider_http_budget *used)
{
  remaining->body_bytes -= used->body_bytes;
  remaining->header_bytes -= used->header_bytes;
  remaining->fields -= used->fields;
  remaining->informational -= used->informational;
}

enum call_status provider_http_open(const struct path_context *context,
    handle_t initial_provider, const char *uri, uint64_t rights, handle_t clock,
    uint64_t deadline_ns,
    struct provider_http_workspace *workspace, struct pyxis_response_info *info,
    handle_t *file)
{
  if (!file) {
    if (info) {
      *info = (struct pyxis_response_info){0};
    }
    return CALL_BAD_REQUEST;
  }
  *file = HANDLE_INVALID;
  if (info) {
    *info = (struct pyxis_response_info){0};
  }
  if (!workspace || !deadline_ns || rights != FILE_RIGHT_READ) {
    return rights != FILE_RIGHT_READ ? CALL_DENIED : CALL_LIMIT;
  }
  workspace->hop = (struct provider_legacy_http_result){0};
  enum call_status status = http_url_parse(uri, &workspace->parsed);
  if (status != CALL_OK) {
    return status;
  }
  memcpy(workspace->current, uri, strlen(uri) + 1);
  struct provider_legacy_http_open_request *request = &workspace->request;
  *request = (struct provider_legacy_http_open_request){
    .flags = PROVIDER_LEGACY_HTTP_CONTEXT,
    .origin = {.scheme = workspace->parsed.scheme, .port = workspace->parsed.port},
    .remaining = {HTTP_BODY_MAX, HTTP_HEADERS_MAX, HTTP_FIELDS_MAX, HTTP_INFORMATIONAL_MAX},
  };
  memcpy(request->origin.host, workspace->parsed.host, strlen(workspace->parsed.host) + 1);
  enum http_scheme initial_scheme = workspace->parsed.scheme;
  handle_t providers[2] = {HANDLE_INVALID, HANDLE_INVALID};
  providers[initial_scheme] = initial_provider;
  size_t redirects = 0;
  for (;;) {
    status = http_url_key(workspace->current, workspace->keys[redirects]);
    if (status != CALL_OK) {
      break;
    }
    for (size_t i = 0; i < redirects; ++i) {
      if (!strcmp(workspace->keys[i], workspace->keys[redirects])) {
        status = CALL_LIMIT;
        goto done;
      }
    }
    enum http_scheme scheme = workspace->parsed.scheme;
    if (providers[scheme] == HANDLE_INVALID) {
      status = binding(context, scheme, &providers[scheme]);
      if (status != CALL_OK) {
        break;
      }
    }
    status = provider_legacy_http_open(providers[scheme], workspace->current, rights,
        deadline_ns, request, &workspace->hop, file);
    if (status != CALL_OK) {
      break;
    }
    status = workspace->hop.status;
    if (status != CALL_OK) {
      break;
    }
    consume(&request->remaining, &workspace->hop.consumed);
    if (workspace->hop.outcome == PROVIDER_LEGACY_HTTP_OUTCOME_BYTES) {
      if (workspace->hop.provider_status != 200 && workspace->hop.provider_status != 204) {
        status = CALL_BAD_REQUEST;
        break;
      }
      if (info) {
        info->flags = PYXIS_RESPONSE_PROVIDER | PYXIS_RESPONSE_HTTP | PYXIS_RESPONSE_URL;
        info->status = workspace->hop.provider_status;
        info->redirect_count = redirects;
        memcpy(info->url, workspace->current, strlen(workspace->current) + 1);
        if (workspace->hop.metadata.media_type_size) {
          info->flags |= PYXIS_RESPONSE_MEDIA_TYPE;
          memcpy(info->media_type, workspace->hop.metadata.media_type,
              workspace->hop.metadata.media_type_size + 1);
        }
      }
      break;
    }
    if (redirects == HTTP_REDIRECT_MAX) {
      status = CALL_LIMIT;
      break;
    }
    status = http_url_resolve(workspace->current, workspace->hop.location, workspace->next);
    if (status != CALL_OK) {
      status = CALL_BAD_REQUEST;
      break;
    }
    enum http_scheme previous = scheme;
    status = http_url_parse(workspace->next, &workspace->parsed);
    if (status != CALL_OK) {
      break;
    }
    if (previous == HTTP_SCHEME_HTTPS && workspace->parsed.scheme == HTTP_SCHEME_HTTP) {
      status = CALL_DENIED;
      break;
    }
    if (!same_origin(&request->origin, &workspace->parsed)) {
      request->flags |= PROVIDER_LEGACY_HTTP_ORIGIN_CROSSED;
    }
    memcpy(workspace->current, workspace->next, strlen(workspace->next) + 1);
    ++redirects;
  }
done:
  for (size_t i = 0; i < 2; ++i) {
    if (i != (size_t)initial_scheme && providers[i] != HANDLE_INVALID) {
      enum call_status closed = handle_close(providers[i]);
      if (status == CALL_OK) {
        status = closed;
      }
    }
  }
  if (status == CALL_OK) {
    uint64_t now;
    status = clock_now(clock, &now);
    if (status == CALL_OK && now >= deadline_ns) {
      status = CALL_TIMED_OUT;
    }
  }
  if (status != CALL_OK && *file != HANDLE_INVALID) {
    handle_close(*file);
    *file = HANDLE_INVALID;
  }
  if (status != CALL_OK && info) {
    *info = (struct pyxis_response_info){0};
  }
  return status;
}

static enum call_status prepare_bound(const struct path_context *context,
    const char *uri, uint64_t method, uint64_t flags, const char *headers,
    size_t headers_size, uint64_t deadline_ns,
    const struct http_prepare_context *continuity, struct http_prepared *prepared)
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
  status = http_transaction_prepare_context(provider, uri, method, flags,
      headers, headers_size, deadline_ns, continuity, prepared);
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
      deadline_ns, NULL, prepared);
}

struct http_stream_workspace {
  char current[HTTP_URI_MAX + 1], next[HTTP_URI_MAX + 1];
  char keys[HTTP_REDIRECT_MAX + 1][HTTP_URL_KEY_BYTES];
  struct http_uri parsed;
  struct http_prepare_context context;
  struct http_transaction_status status;
  handle_t providers[2];
};

static bool redirect(unsigned status)
{
  return status == 301 || status == 302 || status == 303 ||
      status == 307 || status == 308;
}

static enum call_status ordinary_status(unsigned status)
{
  if (status == 200 || status == 204) {
    return CALL_OK;
  }
  if (status == 404 || status == 410) {
    return CALL_NOT_FOUND;
  }
  if (status == 401 || status == 403) {
    return CALL_DENIED;
  }
  return status >= 200 && status < 400 ? CALL_BAD_OPERATION : CALL_IO;
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
  if (!uri || !deadline_ns) {
    return CALL_BAD_REQUEST;
  }
  uint64_t flags = 0;
  if (!strncmp(uri, "json+", 5)) {
    uri += 5;
    flags = HTTP_PREPARE_JSON;
  }
  struct http_stream_workspace *workspace = calloc(1, sizeof(*workspace));
  if (!workspace) {
    return CALL_NO_MEMORY;
  }
  enum call_status status = http_url_parse(uri, &workspace->parsed);
  if (status != CALL_OK) {
    free(workspace);
    return status;
  }
  memcpy(workspace->current, uri, strlen(uri) + 1);
  workspace->context.origin.scheme = workspace->parsed.scheme;
  workspace->context.origin.port = workspace->parsed.port;
  memcpy(workspace->context.origin.host, workspace->parsed.host,
      strlen(workspace->parsed.host) + 1);
  workspace->context.remaining = (struct http_framing_budget){
    HTTP_HEADERS_MAX, HTTP_FIELDS_MAX, HTTP_INFORMATIONAL_MAX};
  size_t redirects = 0;
  for (;;) {
    status = http_url_key(workspace->current, workspace->keys[redirects]);
    if (status != CALL_OK) {
      break;
    }
    for (size_t i = 0; i < redirects; ++i) {
      if (!strcmp(workspace->keys[i], workspace->keys[redirects])) {
        status = CALL_LIMIT;
        goto done;
      }
    }
    size_t index = workspace->parsed.scheme == HTTP_SCHEME_HTTPS ? 1 : 0;
    if (workspace->providers[index] == HANDLE_INVALID) {
      status = binding(context, workspace->parsed.scheme, &workspace->providers[index]);
      if (status != CALL_OK) {
        break;
      }
    }
    status = http_transaction_prepare_context(workspace->providers[index],
        workspace->current, HTTP_PREPARE_GET, flags, NULL, 0, deadline_ns,
        &workspace->context, prepared);
    if (status != CALL_OK) {
      break;
    }
    struct http_commit_reply committed;
    status = http_transaction_commit(prepared, deadline_ns, &committed);
    if (status != CALL_OK) {
      break;
    }
    if (committed.flags != HTTP_COMMIT_RESPONSE || !prepared->response) {
      status = CALL_BAD_REQUEST;
      break;
    }
    status = http_transaction_status(prepared->observation, deadline_ns, &workspace->status);
    if (status != CALL_OK) {
      break;
    }
    struct http_transaction_status *response = &workspace->status;
    if (response->http_status != committed.http_status ||
        response->charged.header_bytes > workspace->context.remaining.header_bytes ||
        response->charged.fields > workspace->context.remaining.fields ||
        response->charged.informational > workspace->context.remaining.informational) {
      status = CALL_BAD_REQUEST;
      break;
    }
    if (!redirect(response->http_status)) {
      status = ordinary_status(response->http_status);
      if (status == CALL_OK && info) {
        info->flags = PYXIS_RESPONSE_PROVIDER | PYXIS_RESPONSE_HTTP | PYXIS_RESPONSE_URL;
        info->status = response->http_status;
        info->redirect_count = redirects;
        memcpy(info->url, workspace->current, strlen(workspace->current) + 1);
        if (response->media_type[0]) {
          info->flags |= PYXIS_RESPONSE_MEDIA_TYPE;
          memcpy(info->media_type, response->media_type, sizeof(info->media_type));
        }
      }
      break;
    }
    if (redirects == HTTP_REDIRECT_MAX) {
      status = CALL_LIMIT;
      break;
    }
    if (response->response_flags & HTTP_STATUS_INVALID_LOCATION) {
      status = CALL_BAD_REQUEST;
      break;
    }
    status = http_url_resolve(workspace->current, response->location, workspace->next);
    if (status != CALL_OK) {
      status = status == CALL_FILE_TOO_LARGE ? status : CALL_BAD_REQUEST;
      break;
    }
    enum http_scheme previous = workspace->parsed.scheme;
    status = http_url_parse(workspace->next, &workspace->parsed);
    if (status != CALL_OK) {
      break;
    }
    if (previous == HTTP_SCHEME_HTTPS && workspace->parsed.scheme == HTTP_SCHEME_HTTP) {
      status = CALL_DENIED;
      break;
    }
    if (!same_origin(&workspace->context.origin, &workspace->parsed)) {
      workspace->context.flags |= HTTP_PREPARE_ORIGIN_CROSSED;
    }
    if ((prepared->trust_flags & HTTP_TRUST_ORIGIN_ONLY) &&
        (workspace->context.flags & HTTP_PREPARE_ORIGIN_CROSSED)) {
      status = CALL_DENIED;
      break;
    }
    /* Retire this response without draining an unbounded redirect body. The
     * provider acknowledges first ABORT after its pending I/O cleanup. */
    status = http_transaction_abort(prepared->control, deadline_ns);
    if (status == CALL_OK) {
      status = http_transaction_status(prepared->observation, deadline_ns, response);
    }
    if (status == CALL_OK) {
      if (response->charged.header_bytes > workspace->context.remaining.header_bytes ||
          response->charged.fields > workspace->context.remaining.fields ||
          response->charged.informational > workspace->context.remaining.informational) {
        status = CALL_BAD_REQUEST;
      } else {
        workspace->context.remaining.header_bytes -= response->charged.header_bytes;
        workspace->context.remaining.fields -= response->charged.fields;
        workspace->context.remaining.informational -= response->charged.informational;
      }
    }
    enum call_status closed = http_prepared_close(prepared);
    if (status == CALL_OK) {
      status = closed;
    }
    if (status != CALL_OK) {
      break;
    }
    memcpy(workspace->current, workspace->next, strlen(workspace->next) + 1);
    ++redirects;
  }
done:
  for (size_t i = 0; i < 2; ++i) {
    if (workspace->providers[i] != HANDLE_INVALID) {
      enum call_status closed = handle_close(workspace->providers[i]);
      if (status == CALL_OK) {
        status = closed;
      }
    }
  }
  if (status != CALL_OK) {
    http_prepared_close(prepared);
    if (info) {
      *info = (struct pyxis_response_info){0};
    }
  }
  free(workspace);
  return status;
}
