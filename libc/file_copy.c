#include <directory.h>
#include <environment.h>
#include <errno.h>
#include <file.h>
#include <handle.h>
#include <limits.h>
#include <path.h>
#include <pyxis/file.h>
#include <pyxis/working_path.h>
#include <resource.h>
#include <stdlib.h>
#include <string.h>
#include "errors.h"
#include "working_path_internal.h"

#define COPY_IDENTITY_VALID (FILE_INFO_DOMAIN_VALID | FILE_INFO_OBJECT_VALID)

static enum call_status close_handle(handle_t handle, enum call_status status)
{
  if (handle != HANDLE_INVALID && handle_close(handle) < 0 && status == CALL_OK) {
    return CALL_BAD_HANDLE;
  }
  return status;
}

static enum call_status close_working(struct pyxis_working_snapshot *working,
    enum call_status status)
{
  while (working->context.count) {
    status = close_handle(working->context.directories[--working->context.count], status);
  }
  free(working->context.directories);
  free(working->path);
  *working = (struct pyxis_working_snapshot){0};
  return status;
}

/* Retain the original ordered chain. Destination lookup and exclusive creation
 * share one captured parent; an intervening creator is never reopened. */
static enum call_status open_operand(const struct path_context *context, const char *path,
    bool writable, struct resource_result *result, bool *created)
{
  size_t length = strlen(path);
  size_t prefix = libc_path_root_prefix(path);
  size_t depth = prefix ? 1 : context->count;
  if (*path == '/' || length == SIZE_MAX || depth > SIZE_MAX - length - 1 ||
      depth + length + 1 > SIZE_MAX / sizeof(handle_t)) {
    return *path == '/' ? CALL_BAD_REQUEST : CALL_LIMIT;
  }
  handle_t *chain = malloc((depth + length + 1) * sizeof(*chain));
  char *component = malloc(length + 1);
  if (!chain || !component) {
    free(component);
    free(chain);
    return CALL_NO_MEMORY;
  }
  size_t count = 0;
  enum call_status status = CALL_OK;
  uint64_t rights = writable ? FILE_RIGHT_WRITE : FILE_RIGHT_READ;
  const char *tail = path;
  if (prefix) {
    size_t name_size = prefix - 3;
    if (name_size > ENVIRONMENT_NAME_MAX) {
      status = CALL_BAD_REQUEST;
      goto done;
    }
    memcpy(component, path, name_size);
    component[name_size] = 0;
    status = path_root(component, &chain[0]);
    if (status != CALL_OK) {
      goto done;
    }
    count = 1;
    tail += prefix;
    struct handle_info info;
    status = handle_query(chain[0], &info);
    if (status != CALL_OK) {
      goto done;
    }
    if (info.protocol != PROTOCOL_DIRECTORY) {
      if (writable && (info.protocol != PROTOCOL_FILE ||
          info.kind != HANDLE_KIND_NATIVE || info.transport)) {
        status = CALL_BAD_OPERATION;
      } else {
        status = resource_open_held(chain[0], tail,
            writable ? PROVIDER_INTENT_WRITE : PROVIDER_INTENT_READ, 0, 0, result);
      }
      goto done;
    }
    if (info.kind != HANDLE_KIND_NATIVE || info.transport) {
      status = CALL_BAD_OPERATION;
      goto done;
    }
  } else {
    if (!depth) {
      status = CALL_UNAVAILABLE;
      goto done;
    }
    while (count < depth) {
      status = handle_copy(context->directories[count], &chain[count]);
      if (status != CALL_OK) {
        goto done;
      }
      ++count;
    }
  }

  for (;;) {
    while (*tail == '/') {
      ++tail;
    }
    if (!*tail) {
      status = CALL_WRONG_TYPE;
      break;
    }
    const char *start = tail;
    while (*tail && *tail != '/') {
      ++tail;
    }
    size_t size = tail - start;
    bool separator = *tail != 0;
    while (*tail == '/') {
      ++tail;
    }
    if (size == 1 && start[0] == '.') {
      continue;
    }
    if (size == 2 && start[0] == '.' && start[1] == '.') {
      if (count == 1) {
        status = CALL_DENIED;
        break;
      }
      status = close_handle(chain[--count], status);
      if (status != CALL_OK) {
        break;
      }
      continue;
    }
    memcpy(component, start, size);
    component[size] = 0;
    struct handle_info parent;
    status = handle_query(chain[count - 1], &parent);
    if (status != CALL_OK) {
      break;
    }
    if (parent.protocol != PROTOCOL_DIRECTORY || parent.kind != HANDLE_KIND_NATIVE ||
        parent.transport) {
      status = CALL_BAD_OPERATION;
      break;
    }
    if (*tail) {
      status = directory_lookup(chain[count - 1], component, DIRECTORY_KIND_DIRECTORY,
          parent.rights & DIRECTORY_RIGHTS, &chain[count]);
      if (status != CALL_OK) {
        break;
      }
      ++count;
      continue;
    }
    /* A trailing slash requires a directory and cannot create a FILE. */
    if (separator) {
      status = CALL_WRONG_TYPE;
      break;
    }
    handle_t file = HANDLE_INVALID;
    status = directory_lookup(chain[count - 1], component, DIRECTORY_KIND_FILE,
        rights, &file);
    if (writable && status == CALL_NOT_FOUND) {
      status = directory_create(chain[count - 1], component, DIRECTORY_KIND_FILE,
          rights, &file);
      *created = status == CALL_OK;
    }
    if (status == CALL_OK) {
      status = resource_result_adopt(file, result);
      if (status != CALL_OK) {
        status = close_handle(file, status);
      }
    }
    break;
  }
done:
  while (count) {
    status = close_handle(chain[--count], status);
  }
  free(component);
  free(chain);
  return status;
}

static enum call_status source_info(const struct resource_result *source,
    struct file_info_reply *info)
{
  enum call_status status = resource_outcome_status(source->info.description.outcome);
  if (status == CALL_OK && source->info.protocol != PROTOCOL_FILE) {
    status = CALL_BAD_OPERATION;
  }
  if (status == CALL_OK) {
    status = file_info(source->resource, info);
  }
  if (status == CALL_OK && !(info->valid & FILE_INFO_SIZE_VALID)) {
    status = file_size(source->resource, &info->size);
    if (status == CALL_OK) {
      info->valid |= FILE_INFO_SIZE_VALID;
    }
  }
  return status == CALL_OK && info->size > LONG_MAX ? CALL_LIMIT : status;
}

static enum call_status prove_distinct(const struct file_info_reply *source,
    const struct file_info_reply *destination)
{
  if ((source->valid & FILE_INFO_DOMAIN_VALID) &&
      (destination->valid & FILE_INFO_DOMAIN_VALID) && source->domain != destination->domain) {
    return CALL_OK;
  }
  if ((source->valid & COPY_IDENTITY_VALID) != COPY_IDENTITY_VALID ||
      (destination->valid & COPY_IDENTITY_VALID) != COPY_IDENTITY_VALID) {
    return CALL_BAD_OPERATION;
  }
  return source->object == destination->object ? CALL_BAD_REQUEST : CALL_OK;
}

int pyxis_file_copy(const char *source, const char *destination, uint64_t *copied)
{
  if (!source || !*source || !destination || !*destination || !copied) {
    errno = EINVAL;
    return -1;
  }
  int saved_errno = errno;
  struct pyxis_working_snapshot working = {0};
  enum call_status status = libc_path_root_prefix(source) && libc_path_root_prefix(destination) ?
      CALL_OK : pyxis_working_snapshot_init(&working, NULL);
  struct resource_result from = {0}, to = {0};
  bool created = false;
  bool unexpected_eof = false;
  bool same_file = false;
  struct file_info_reply info = {0};
  if (status == CALL_OK) {
    status = open_operand(&working.context, source, false, &from, &created);
  }
  if (status == CALL_OK) {
    status = source_info(&from, &info);
  }
  if (status == CALL_OK) {
    status = open_operand(&working.context, destination, true, &to, &created);
  }
  if (status == CALL_OK) {
    status = resource_outcome_status(to.info.description.outcome);
  }
  if (status == CALL_OK && (to.info.protocol != PROTOCOL_FILE ||
      to.info.kind != HANDLE_KIND_NATIVE || to.info.transport)) {
    status = CALL_BAD_OPERATION;
  }
  if (status == CALL_OK && !created) {
    struct file_info_reply destination_info;
    status = file_info(to.resource, &destination_info);
    if (status == CALL_OK) {
      status = prove_distinct(&info, &destination_info);
      same_file = status == CALL_BAD_REQUEST;
    }
  }
  /* Finish releasing path authority before the first existing-object resize. */
  status = close_working(&working, status);
  if (status == CALL_OK && !created) {
    status = file_resize(to.resource, 0);
  }
  unsigned char buffer[FILE_READ_MAX_BYTES];
  uint64_t offset = 0;
  while (status == CALL_OK && offset < info.size) {
    uint64_t remaining = info.size - offset;
    size_t capacity = remaining < sizeof(buffer) ? (size_t)remaining : sizeof(buffer);
    size_t count;
    status = file_read(from.resource, offset, buffer, capacity, &count);
    if (status != CALL_OK) {
      break;
    }
    if (!count) {
      unexpected_eof = true;
      status = CALL_IO;
      break;
    }
    size_t written = 0;
    while (status == CALL_OK && written < count) {
      size_t progress;
      status = file_write(to.resource, offset + written, buffer + written,
          count - written, &progress);
      if (status == CALL_OK) {
        written += progress;
      }
    }
    offset += written;
  }
  enum call_status closed = resource_result_close(&to);
  if (status == CALL_OK) {
    status = closed;
  }
  closed = resource_result_close(&from);
  if (status == CALL_OK) {
    status = closed;
  }
  if (status != CALL_OK) {
    errno = same_file ? EINVAL : libc_path_errno(status, source, destination);
    return unexpected_eof ? PYXIS_COPY_UNEXPECTED_EOF : -1;
  }
  *copied = info.size;
  errno = saved_errno;
  return 0;
}
