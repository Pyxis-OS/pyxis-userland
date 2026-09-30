#include "trust.h"
#include "../libhttp/http.h"
#include <abi/directory.h>
#include <file.h>
#include <handle.h>
#include <path.h>
#include <startup.h>
#include <string.h>

static enum call_status readonly_directory(handle_t handle)
{
  struct handle_info info;
  enum call_status status = handle_query(handle, &info);
  if (status != CALL_OK) {
    return status;
  }
  if (info.kind != HANDLE_KIND_NATIVE || info.protocol != PROTOCOL_DIRECTORY ||
      info.transport != 0 ||
      (info.rights & ~(DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_ENUMERATE |
          DIRECTORY_RIGHT_READ_FILES | DIRECTORY_RIGHT_FILESYSTEM_INFO))) {
    return CALL_DENIED;
  }
  return CALL_OK;
}

static enum call_status tls_status(const struct tls_result *result)
{
  struct http_result failure = {.error = HTTP_TLS_ERROR, .tls_failure = *result};
  return http_result_status(&failure);
}

enum call_status httpfs_trust_load(struct tls_runtime *runtime, const char *uri,
    struct tls_result *result, enum call_status *cleanup_status)
{
  *result = (struct tls_result){0};
  size_t depth = startup_working_directory_count();
  const handle_t *cwd = startup_working_directories();
  struct path_root roots[STARTUP_ROOT_LIMIT];
  const struct startup_binding *selected_roots = startup_roots();
  size_t root_count = startup_root_count();
  enum call_status status = CALL_OK;
  if (startup_namespace() != HANDLE_INVALID) {
    status = CALL_DENIED;
    goto failed;
  }
  if (root_count > STARTUP_ROOT_LIMIT) {
    status = CALL_LIMIT;
    goto failed;
  }
  for (size_t i = 0; i < root_count; ++i) {
    handle_t handle = selected_roots[i].handle;
    status = readonly_directory(handle);
    if (status != CALL_OK) {
      goto failed;
    }
    roots[i] = (struct path_root){(const char *)selected_roots[i].name, handle};
  }
  for (size_t i = 0; i < depth; ++i) {
    status = readonly_directory(cwd[i]);
    if (status != CALL_OK) {
      goto failed;
    }
  }
  size_t length = strlen(uri);
  if (length == 0 || length > TLS_ALLOCATION_MAX / sizeof(handle_t) - 1 ||
      depth > TLS_ALLOCATION_MAX / sizeof(handle_t) - length - 1) {
    status = CALL_LIMIT;
    goto failed;
  }
  size_t slots = depth + length + 1;
  handle_t *directories = tls_allocate(runtime, slots * sizeof(*directories), result);
  if (!directories) {
    return tls_status(result);
  }
  char *component = tls_allocate(runtime, length + 1, result);
  if (!component) {
    tls_deallocate(directories);
    return tls_status(result);
  }
  struct path_workspace workspace = {directories, slots, component, length + 1};
  struct path_context context = {
    .directories = (handle_t *)cwd, .count = depth,
    .roots = roots, .root_count = root_count,
  };
  handle_t file = HANDLE_INVALID;
  status = path_resolve(&context, uri, DIRECTORY_KIND_FILE, FILE_RIGHT_READ,
      &workspace, &file);
  tls_deallocate(component);
  tls_deallocate(directories);
  if (status != CALL_OK) {
    goto failed;
  }
  struct handle_info info;
  status = handle_query(file, &info);
  if (status == CALL_OK && (info.kind != HANDLE_KIND_NATIVE ||
      info.protocol != PROTOCOL_FILE || info.rights != FILE_RIGHT_READ ||
      info.transport != 0)) {
    status = CALL_DENIED;
  }
  uint64_t size = 0;
  if (status == CALL_OK) {
    status = file_size(file, &size);
  }
  if (status == CALL_OK && (size == 0 || size >= TLS_ALLOCATION_MAX)) {
    status = size == 0 ? CALL_BAD_REQUEST : CALL_FILE_TOO_LARGE;
  }
  unsigned char *pem = NULL;
  if (status == CALL_OK) {
    pem = tls_allocate(runtime, (size_t)size + 1, result);
    if (!pem) {
      status = tls_status(result);
    }
  }
  size_t offset = 0;
  while (status == CALL_OK && offset < size) {
    size_t count;
    status = file_read(file, offset, pem + offset, (size_t)size - offset, &count);
    if (status == CALL_OK && count == 0) {
      status = CALL_IO;
    }
    offset += count;
  }
  if (status == CALL_OK) {
    size_t count;
    status = file_read(file, size, pem + size, 1, &count);
    if (status == CALL_OK && count != 0) {
      status = CALL_IO;
    }
  }
  if (handle_close(file) != 0) {
    *cleanup_status = CALL_BAD_HANDLE;
  }
  if (status == CALL_OK && *cleanup_status != CALL_OK) {
    status = *cleanup_status;
  }
  if (status == CALL_OK) {
    pem[size] = 0;
    if (!tls_trust_import(runtime, pem, (size_t)size + 1, result)) {
      status = tls_status(result);
    }
  }
  tls_deallocate(pem);
  if (status == CALL_OK || result->error != TLS_OK) {
    return status;
  }
failed:
  *result = (struct tls_result){.error = TLS_TRUST_ERROR, .native_status = status};
  return status;
}
