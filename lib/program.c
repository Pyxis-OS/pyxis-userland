#include <abi/file.h>
#include <file.h>
#include <handle.h>
#include <launcher.h>
#include <path.h>
#include <pxe/shebang.h>
#include <stdlib.h>
#include <string.h>

static enum call_status read_prefix(handle_t file, char *bytes, size_t *size)
{
  *size = 0;
  while (*size < SHEBANG_PREFIX_SIZE) {
    size_t count;
    enum call_status status = file_read(file, *size, bytes + *size,
        SHEBANG_PREFIX_SIZE - *size, &count);
    if (status != CALL_OK) {
      return status;
    }
    if (!count) {
      break;
    }
    size_t start = *size;
    *size += count;
    for (size_t i = start; i < *size; ++i) {
      if (bytes[i] == '\n') {
        return CALL_OK;
      }
    }
    if (*size >= 2 && (bytes[0] != '#' || bytes[1] != '!')) {
      break;
    }
  }
  return CALL_OK;
}

static enum call_status open_interpreter(const char *uri, handle_t *image)
{
  size_t capacity = strlen(uri) + 1; /* Bounded by the shebang parser. */
  handle_t *directories = malloc(capacity * sizeof(*directories));
  char *component = malloc(capacity);
  if (!directories || !component) {
    free(component);
    free(directories);
    return CALL_NO_MEMORY;
  }
  struct path_workspace workspace = {directories, capacity, component, capacity};
  enum call_status status = path_resolve(NULL, uri, DIRECTORY_KIND_FILE, FILE_RIGHT_READ,
      &workspace, image);
  free(component);
  free(directories);
  return status;
}

static enum call_status launch_script(handle_t launcher, const struct launch_request *source,
    const char *interpreter, handle_t *child)
{
  if (!source->argc || !source->argv ||
      (source->grant_count && !source->grants) ||
      (source->resource_count && !source->resources)) {
    return CALL_BAD_REQUEST;
  }
  if (source->argc >= LAUNCH_CAPTURE_MAX_SIZE / sizeof(char *) - 1 ||
      source->grant_count >= LAUNCH_CAPTURE_MAX_SIZE / sizeof(struct launch_grant) ||
      source->resource_count >= LAUNCH_CAPTURE_MAX_SIZE / sizeof(struct launch_binding)) {
    return CALL_LIMIT;
  }
  const struct launch_binding *original_resources = (const void *)(uintptr_t)source->resources;
  for (size_t i = 0; i < source->resource_count; ++i) {
    if (!strcmp((const char *)(uintptr_t)original_resources[i].name, "script")) {
      return CALL_BAD_REQUEST;
    }
  }

  struct launch_grant *grants = malloc((source->grant_count + 1) * sizeof(*grants));
  struct launch_binding *resources = malloc((source->resource_count + 1) * sizeof(*resources));
  const char **arguments = malloc((source->argc + 2) * sizeof(*arguments));
  handle_t image = HANDLE_INVALID;
  enum call_status status = CALL_NO_MEMORY;
  if (!grants || !resources || !arguments) {
    goto done;
  }
  status = open_interpreter(interpreter, &image);
  if (status != CALL_OK) {
    goto done;
  }

  if (source->grant_count) {
    memcpy(grants, (const void *)(uintptr_t)source->grants, source->grant_count * sizeof(*grants));
  }
  grants[source->grant_count] = (struct launch_grant){source->image, FILE_RIGHT_READ};
  if (source->resource_count) {
    memcpy(resources, original_resources, source->resource_count * sizeof(*resources));
  }
  resources[source->resource_count] = (struct launch_binding){(uintptr_t)"script", source->grant_count};
  arguments[0] = interpreter;
  memcpy(arguments + 1, (const void *)(uintptr_t)source->argv, source->argc * sizeof(*arguments));
  arguments[source->argc + 1] = NULL;

  struct launch_request request = *source;
  request.image = image;
  request.grants = (uintptr_t)grants;
  request.grant_count++;
  request.resources = (uintptr_t)resources;
  request.resource_count++;
  request.argv = (uintptr_t)arguments;
  request.argc++;
  /* The low-level launcher accepts only PXE, so another script cannot recurse.
   * Appending the new grant leaves every caller-supplied index unchanged. */
  status = launcher_launch(launcher, &request, child);

done:
  if (image != HANDLE_INVALID) {
    handle_close(image);
  }
  free(arguments);
  free(resources);
  free(grants);
  return status;
}

enum call_status program_launch(handle_t launcher, const struct launch_request *request,
                                 handle_t *child)
{
  if (!child) {
    return CALL_BAD_REQUEST;
  }
  *child = HANDLE_INVALID;
  if (!request) {
    return CALL_BAD_REQUEST;
  }
  /* The first user stack is one page: keep the bounded file prefix on the heap. */
  char *prefix = malloc(SHEBANG_PREFIX_SIZE);
  if (!prefix) {
    return CALL_NO_MEMORY;
  }
  size_t size;
  enum call_status status = read_prefix(request->image, prefix, &size);
  if (status == CALL_OK) {
    struct shebang script;
    enum shebang_result format = shebang_parse(prefix, size, &script);
    if (format == SHEBANG_NONE) {
      status = launcher_launch(launcher, request, child);
    } else if (format == SHEBANG_OK) {
      /* The limit leaves one byte beyond the URI for this terminator, even
       * when EOF rather than LF ends a maximum-length shebang. */
      prefix[script.interpreter - prefix + script.length] = '\0';
      status = launch_script(launcher, request, script.interpreter, child);
    } else {
      status = format == SHEBANG_TOO_LONG ? CALL_LIMIT : CALL_BAD_REQUEST;
    }
  }
  free(prefix);
  return status;
}
