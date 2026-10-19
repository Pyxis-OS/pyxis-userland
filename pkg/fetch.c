#include "fetch.h"
#include <clock.h>
#include <directory.h>
#include <file.h>
#include <handle.h>
#include <http_prepare.h>
#include <http_url.h>
#include <literal.h>
#include <resource.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FETCH_DEFAULT_SECONDS UINT64_C(30)
#define FETCH_MAX_SECONDS UINT64_C(86400)
#define FETCH_BYTES_PER_SECOND UINT64_C(262144)
#define NS_PER_SECOND UINT64_C(1000000000)

static void set_detail(char detail[RESOURCE_DETAIL_MAX_BYTES + 1], const char *text)
{
  size_t length = strnlen(text, RESOURCE_DETAIL_MAX_BYTES);
  memcpy(detail, text, length);
  detail[length] = 0;
}

bool pkg_repository_url_valid(const char *base)
{
  if (!base) return false;
  size_t length = strnlen(base, PKG_BASE_URL_MAX + 1);
  if (!length || length > PKG_BASE_URL_MAX || base[length - 1] != '/' ||
      strchr(base, '@') || strchr(base, '?') || strchr(base, '#')) {
    return false;
  }
  struct http_uri uri;
  return http_url_parse(base, &uri) == CALL_OK;
}

static enum call_status config_read(handle_t directory, const char *name,
    char **bytes, size_t *size)
{
  *bytes = NULL;
  *size = 0;
  handle_t file = HANDLE_INVALID;
  enum call_status status = directory_lookup(directory, name, DIRECTORY_KIND_FILE,
      FILE_RIGHT_READ, &file);
  if (status != CALL_OK) {
    return status;
  }
  uint64_t length = 0;
  status = file_size(file, &length);
  if (status == CALL_OK && length > LITERAL_SOURCE_LIMIT) {
    status = CALL_FILE_TOO_LARGE;
  }
  char *buffer = status == CALL_OK ? malloc(length ? (size_t)length : 1) : NULL;
  if (status == CALL_OK && !buffer) {
    status = CALL_NO_MEMORY;
  }
  size_t used = 0;
  while (status == CALL_OK && used < length) {
    size_t count = 0;
    status = file_read(file, used, buffer + used, (size_t)length - used, &count);
    if (status == CALL_OK && !count) {
      status = CALL_IO;
    }
    if (status == CALL_OK) {
      used += count;
    }
  }
  if (status == CALL_OK) {
    char extra;
    size_t count = 0;
    status = file_read(file, length, &extra, 1, &count);
    if (status == CALL_OK && count) {
      status = CALL_IO;
    }
  }
  if (handle_close(file) != 0 && status == CALL_OK) {
    status = CALL_BAD_HANDLE;
  }
  if (status != CALL_OK) {
    free(buffer);
    return status;
  }
  *bytes = buffer;
  *size = used;
  return CALL_OK;
}

static enum call_status repositories_parse(const char *bytes, size_t size,
    struct pkg_repositories *repositories, const char **reason)
{
  *repositories = (struct pkg_repositories){0};
  *reason = NULL;
  struct literal *document = NULL;
  struct literal_diagnostic diagnostic;
  bool parsed = literal_parse(bytes, size, &document, &diagnostic);
  if (!parsed) {
    *reason = "packages.lua is not strict literal data";
    return CALL_BAD_REQUEST;
  }
  const struct literal_value *root = document->root;
  const struct literal_value *list = literal_field(root, "repositories");
  bool valid = root->kind == LITERAL_TABLE && root->as.table.count == 1 && list &&
      list->kind == LITERAL_TABLE && list->as.table.count <= PKG_REPOSITORIES_MAX;
  size_t count = 0;
  for (const struct literal_entry *entry = valid ? list->as.table.entries : NULL;
       valid && entry; entry = entry->next) {
    const struct literal_value *value = entry->value;
    valid = entry->key->kind == LITERAL_INTEGER &&
        entry->key->as.integer == (int64_t)count + 1 && value->kind == LITERAL_STRING &&
        value->as.string.size <= PKG_BASE_URL_MAX &&
        strlen(value->as.string.bytes) == value->as.string.size &&
        pkg_repository_url_valid(value->as.string.bytes);
    for (size_t i = 0; valid && i < count; ++i) {
      valid = strcmp(repositories->urls[i], value->as.string.bytes) != 0;
    }
    if (valid) {
      memcpy(repositories->urls[count++], value->as.string.bytes, value->as.string.size + 1);
    }
  }
  literal_free(document);
  if (!valid) {
    *repositories = (struct pkg_repositories){0};
    *reason = "packages.lua must contain only repositories: at most eight distinct HTTP/HTTPS base URLs ending in / without credentials, query or fragment";
    return CALL_BAD_REQUEST;
  }
  repositories->count = count;
  return CALL_OK;
}

enum call_status pkg_repositories_load(handle_t package_config,
    struct pkg_repositories *repositories, const char **reason)
{
  *repositories = (struct pkg_repositories){0};
  *reason = NULL;
  char *bytes = NULL;
  size_t size = 0;
  enum call_status status = config_read(package_config, "packages.lua", &bytes, &size);
  if (status == CALL_NOT_FOUND) {
    return CALL_OK;
  }
  if (status != CALL_OK) {
    *reason = "cannot read package_config/packages.lua";
    return status;
  }
  status = repositories_parse(bytes, size, repositories, reason);
  free(bytes);
  return status;
}

enum call_status pkg_repositories_stage(handle_t config,
    const struct pkg_repositories *repositories, const char **reason)
{
  *reason = "cannot prepare packages.lua.new; repository configuration was not replaced";
  if (repositories->count > PKG_REPOSITORIES_MAX) {
    return CALL_BAD_REQUEST;
  }
  /* Quote as literal bytes; never interpret repository URLs as Lua. */
  size_t capacity = 64 + repositories->count * (4 * PKG_BASE_URL_MAX + 8);
  char *bytes = malloc(capacity);
  if (!bytes) {
    return CALL_NO_MEMORY;
  }
  size_t used = snprintf(bytes, capacity, "return { repositories = {\n");
  for (size_t i = 0; i < repositories->count; ++i) {
    if (!pkg_repository_url_valid(repositories->urls[i])) {
      free(bytes);
      return CALL_BAD_REQUEST;
    }
    used += snprintf(bytes + used, capacity - used, "  \"");
    for (const unsigned char *byte = (const unsigned char *)repositories->urls[i];
        *byte; ++byte) {
      if (*byte == '"' || *byte == '\\') {
        bytes[used++] = '\\';
        bytes[used++] = *byte;
      } else if (*byte < 32 || *byte >= 127) {
        used += snprintf(bytes + used, capacity - used, "\\%03u", *byte);
      } else {
        bytes[used++] = *byte;
      }
    }
    used += snprintf(bytes + used, capacity - used, "\",\n");
  }
  used += snprintf(bytes + used, capacity - used, "} }\n");
  enum call_status status = directory_remove(config, "packages.lua.new", DIRECTORY_KIND_FILE);
  if (status == CALL_NOT_FOUND) status = CALL_OK;
  handle_t file = HANDLE_INVALID;
  if (status == CALL_OK) {
    status = directory_create(config, "packages.lua.new", DIRECTORY_KIND_FILE,
        FILE_RIGHT_WRITE, &file);
  }
  for (size_t offset = 0; status == CALL_OK && offset < used;) {
    size_t count = 0;
    status = file_write(file, offset, bytes + offset, used - offset, &count);
    if (status == CALL_OK && !count) status = CALL_IO;
    offset += count;
  }
  free(bytes);
  char *stored = NULL;
  size_t size = 0;
  struct pkg_repositories checked;
  if (status == CALL_OK) {
    status = config_read(config, "packages.lua.new", &stored, &size);
  }
  if (status == CALL_OK) {
    status = repositories_parse(stored, size, &checked, reason);
  }
  free(stored);
  if (status == CALL_OK && checked.count != repositories->count) status = CALL_IO;
  for (size_t i = 0; status == CALL_OK && i < checked.count; ++i) {
    if (strcmp(checked.urls[i], repositories->urls[i])) status = CALL_IO;
  }
  if (status == CALL_OK) status = file_sync(file);
  if (file != HANDLE_INVALID) handle_close(file);
  if (status != CALL_OK && !*reason) {
    *reason = "cannot verify or synchronize packages.lua.new; repository configuration was not replaced";
  }
  if (status == CALL_OK) *reason = NULL;
  return status;
}

enum call_status pkg_timeout_parse(const char *text, uint64_t *seconds)
{
  *seconds = 0;
  if (!text || !*text) {
    return CALL_BAD_REQUEST;
  }
  uint64_t value = 0;
  for (const char *byte = text; *byte; ++byte) {
    if (*byte < '0' || *byte > '9' || value > FETCH_MAX_SECONDS / 10) {
      return CALL_BAD_REQUEST;
    }
    value = value * 10 + (unsigned)(*byte - '0');
    if (value > FETCH_MAX_SECONDS) {
      return CALL_BAD_REQUEST;
    }
  }
  if (!value) {
    return CALL_BAD_REQUEST;
  }
  *seconds = value;
  return CALL_OK;
}

static enum call_status fetch_duration(bool index, uint64_t size,
    uint64_t override, uint64_t *duration)
{
  uint64_t seconds = override;
  if (!seconds) {
    uint64_t chunks = index ? 0 : size / FETCH_BYTES_PER_SECOND +
        (size % FETCH_BYTES_PER_SECOND != 0);
    if (chunks > FETCH_MAX_SECONDS - FETCH_DEFAULT_SECONDS) {
      return CALL_LIMIT;
    }
    seconds = FETCH_DEFAULT_SECONDS + chunks;
  }
  if (seconds > FETCH_MAX_SECONDS) {
    return CALL_BAD_REQUEST;
  }
  *duration = seconds * NS_PER_SECOND;
  return CALL_OK;
}

static enum call_status readonly_root(handle_t root, handle_t *captured)
{
  struct handle_info info;
  enum call_status status = handle_query(root, &info);
  if (status != CALL_OK) {
    return status;
  }
  if (info.kind != HANDLE_KIND_EXPORTED || info.protocol != PROTOCOL_PROVIDER) {
    return CALL_WRONG_TYPE;
  }
  if (!(info.rights & PROVIDER_RIGHT_OPEN_READ) ||
      (info.transport & HANDLE_TRANSPORT_CALL) != HANDLE_TRANSPORT_CALL) {
    return CALL_DENIED;
  }
  return handle_copy_restricted(root, PROVIDER_RIGHT_OPEN_READ, HANDLE_TRANSPORT_CALL, captured);
}

static enum call_status result_status(const struct resource_result *result,
    char detail[RESOURCE_DETAIL_MAX_BYTES + 1])
{
  enum call_status status = resource_outcome_status(result->info.description.outcome);
  if (status != CALL_OK && result->info.description.detail_size) {
    set_detail(detail, result->info.description.detail);
  }
  return status;
}

enum call_status pkg_fetch(const struct pkg_fetch_authority *authority,
    const char *base, const char *relative, bool index, uint64_t size,
    uint64_t timeout_seconds, handle_t *file,
    char detail[RESOURCE_DETAIL_MAX_BYTES + 1])
{
  *file = HANDLE_INVALID;
  set_detail(detail, "invalid package fetch request");
  if (!authority || !base || !relative || !pkg_repository_url_valid(base) || !*relative ||
      *relative == '/' || strchr(relative, ':') || strchr(relative, '@') ||
      strchr(relative, '?') || strchr(relative, '#')) {
    return CALL_BAD_REQUEST;
  }
  size_t base_size = strlen(base);
  size_t relative_size = strnlen(relative, HTTP_URI_MAX + 1);
  if (relative_size > HTTP_URI_MAX || base_size > HTTP_URI_MAX - relative_size) {
    set_detail(detail, "repository target exceeds HTTP URL limit");
    return CALL_FILE_TOO_LARGE;
  }
  char url[HTTP_URI_MAX + 1];
  memcpy(url, base, base_size);
  memcpy(url + base_size, relative, relative_size + 1);
  struct http_uri uri;
  enum call_status status = http_url_parse(url, &uri);
  if (status != CALL_OK) {
    return status;
  }
  uint64_t duration = 0;
  status = fetch_duration(index, size, timeout_seconds, &duration);
  if (status != CALL_OK) {
    set_detail(detail, "fetch timeout exceeds the 86400 second bound; specify --timeout 1..86400");
    return status;
  }
  handle_t root = HANDLE_INVALID;
  status = readonly_root(uri.scheme == HTTP_SCHEME_HTTPS ? authority->https : authority->http,
      &root);
  if (status != CALL_OK) {
    set_detail(detail, "missing GET-only HTTP/HTTPS provider authority");
    return status;
  }
  uint64_t now = 0;
  status = clock_now(authority->clock, &now);
  if (status == CALL_OK && (now > UINT64_MAX - duration ||
      now > UINT64_MAX - FETCH_DEFAULT_SECONDS * NS_PER_SECOND)) {
    status = CALL_LIMIT;
  }
  set_detail(detail, "cannot establish fetch deadline");
  struct resource_result configured = {0}, result = {0};
  if (status == CALL_OK) {
    const char *target = strstr(url, "://") + 3;
    set_detail(detail, "HTTP GET endpoint preparation failed");
    status = http_prepare_endpoint(root, HTTP_ENDPOINT_GET, 0, target, strlen(target),
        NULL, 0, duration, now + duration, &configured, NULL);
    if (status == CALL_OK) {
      status = result_status(&configured, detail);
    }
  }
  if (status == CALL_OK && configured.resource == HANDLE_INVALID) {
    status = CALL_BAD_REQUEST;
  }
  if (status == CALL_OK) {
    set_detail(detail, "HTTP GET failed");
    status = resource_open_held(configured.resource, "", PROVIDER_INTENT_READ, 0,
        now + duration, &result);
    if (status == CALL_OK) {
      status = result_status(&result, detail);
    }
  }
  if (status == CALL_OK) {
    uint64_t flags = RESOURCE_COMPLETE | RESOURCE_REREADABLE | RESOURCE_LENGTH_KNOWN;
    if (result.resource == HANDLE_INVALID || result.info.protocol != PROTOCOL_FILE ||
        result.info.rights != FILE_RIGHT_READ ||
        (result.info.description.flags & flags) != flags) {
      set_detail(detail, "HTTP result is not a completed rereadable READ-only FILE");
      status = CALL_BAD_REQUEST;
    }
  }
  if (status == CALL_OK) {
    uint64_t length = 0;
    status = file_size(result.resource, &length);
    if (status != CALL_OK) {
      set_detail(detail, "cannot read HTTP snapshot length");
    } else if (length != result.info.description.length ||
        (index ? length > LITERAL_SOURCE_LIMIT : length != size)) {
      set_detail(detail, index ? "repository index exceeds its limit or has inconsistent length" :
          "archive length differs from its repository index");
      status = CALL_FILE_TOO_LARGE;
    }
  }
  enum call_status cleanup = resource_result_close(&configured);
  if (handle_close(root) != 0 && cleanup == CALL_OK) {
    cleanup = CALL_BAD_HANDLE;
  }
  if (status == CALL_OK && cleanup != CALL_OK) {
    set_detail(detail, "cannot release configured HTTP endpoint");
    status = cleanup;
  }
  if (status == CALL_OK) {
    *file = result.resource;
    result.resource = HANDLE_INVALID;
    detail[0] = 0;
  }
  cleanup = resource_result_close(&result);
  if (status == CALL_OK && cleanup != CALL_OK) {
    handle_close(*file);
    *file = HANDLE_INVALID;
    set_detail(detail, "cannot release HTTP result metadata");
    status = cleanup;
  }
  return status;
}
