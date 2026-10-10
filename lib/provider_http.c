#include <clock.h>
#include <handle.h>
#include <namespace.h>
#include <path.h>
#include <provider.h>
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
  const char *name = scheme == HTTP_SCHEME_HTTPS ? "https" : "http";
  handle_t root = HANDLE_INVALID;
  if (context && context->roots) {
    for (size_t i = 0; i < context->root_count; ++i) {
      if (!strcmp(context->roots[i].name, name)) {
        root = context->roots[i].handle;
        break;
      }
    }
  } else {
    root = startup_root(name);
  }
  if (root != HANDLE_INVALID) {
    return CALL_BAD_REQUEST;
  }
  handle_t namespace = context && context->namespace_handle != HANDLE_INVALID ?
      context->namespace_handle : startup_namespace();
  if (namespace == HANDLE_INVALID) {
    return CALL_NOT_FOUND;
  }
  return namespace_lookup(namespace, name, provider);
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
  workspace->hop = (struct provider_result){0};
  enum call_status status = http_url_parse(uri, &workspace->parsed);
  if (status != CALL_OK) {
    return status;
  }
  memcpy(workspace->current, uri, strlen(uri) + 1);
  struct provider_open_request *request = &workspace->request;
  *request = (struct provider_open_request){
    .flags = PROVIDER_OPEN_HTTP,
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
    status = provider_open_context(providers[scheme], workspace->current, rights,
        deadline_ns, request, &workspace->hop, file);
    if (status != CALL_OK) {
      break;
    }
    status = workspace->hop.status;
    if (status != CALL_OK) {
      break;
    }
    consume(&request->remaining, &workspace->hop.consumed);
    if (workspace->hop.outcome == PROVIDER_OUTCOME_BYTES) {
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
      request->flags |= PROVIDER_OPEN_ORIGIN_CROSSED;
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
