#include <abi/file.h>
#include <file.h>
#include <handle.h>
#include <launcher.h>
#include <path.h>
#include <pxe/shebang.h>
#include <space.h>
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

static enum call_status open_interpreter(const struct path_context *context,
    const char *uri, handle_t *image)
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
  enum call_status status = path_resolve(context, uri, DIRECTORY_KIND_FILE, FILE_RIGHT_READ,
      &workspace, image);
  free(component);
  free(directories);
  return status;
}

struct script_scratch {
  char *prefix;
  struct launch_grant *grants;
  struct launch_binding *resources;
  const char **arguments;
  handle_t image;
};

static void release_script(struct script_scratch *scratch)
{
  if (scratch->image != HANDLE_INVALID) {
    handle_close(scratch->image);
  }
  free(scratch->arguments);
  free(scratch->resources);
  free(scratch->grants);
  free(scratch->prefix);
}

static enum call_status prepare_script(const struct launch_request *source,
    const char *interpreter, const struct path_context *interpreter_context,
    struct launch_request *request, struct script_scratch *scratch)
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

  scratch->grants = malloc((source->grant_count + 1) * sizeof(*scratch->grants));
  scratch->resources = malloc((source->resource_count + 1) * sizeof(*scratch->resources));
  scratch->arguments = malloc((source->argc + 2) * sizeof(*scratch->arguments));
  if (!scratch->grants || !scratch->resources || !scratch->arguments) {
    return CALL_NO_MEMORY;
  }
  enum call_status status = open_interpreter(interpreter_context, interpreter,
      &scratch->image);
  if (status != CALL_OK) {
    return status;
  }

  if (source->grant_count) {
    memcpy(scratch->grants, (const void *)(uintptr_t)source->grants,
        source->grant_count * sizeof(*scratch->grants));
  }
  struct handle_info info;
  enum call_status query = handle_query(source->image, &info);
  if (query != CALL_OK) {
    return query;
  }
  if (info.protocol != PROTOCOL_FILE) {
    return CALL_WRONG_TYPE;
  }
  uint64_t transport = info.kind == HANDLE_KIND_EXPORTED ? HANDLE_TRANSPORT_CALL : 0;
  scratch->grants[source->grant_count] =
      (struct launch_grant){source->image, FILE_RIGHT_READ, transport};
  if (source->resource_count) {
    memcpy(scratch->resources, original_resources,
        source->resource_count * sizeof(*scratch->resources));
  }
  scratch->resources[source->resource_count] =
      (struct launch_binding){(uintptr_t)"script", source->grant_count};
  scratch->arguments[0] = interpreter;
  memcpy(scratch->arguments + 1, (const void *)(uintptr_t)source->argv,
      source->argc * sizeof(*scratch->arguments));
  scratch->arguments[source->argc + 1] = NULL;

  request->image = scratch->image;
  request->grants = (uintptr_t)scratch->grants;
  request->grant_count++;
  request->resources = (uintptr_t)scratch->resources;
  request->resource_count++;
  request->argv = (uintptr_t)scratch->arguments;
  request->argc++;
  /* The low-level launcher accepts only PXE, so another script cannot recurse.
   * Appending the new grant preserves ordinary and standard-stream indices;
   * streams still refer to their sole child grant, without a script-side copy. */
  return CALL_OK;
}

static enum call_status prepare_program(const struct launch_request *source,
    const struct path_context *interpreter_context, struct launch_request *request,
    struct script_scratch *scratch)
{
  *request = *source;
  /* Keep the bounded file prefix out of the caller's stack budget. */
  scratch->prefix = malloc(SHEBANG_PREFIX_SIZE);
  if (!scratch->prefix) {
    return CALL_NO_MEMORY;
  }
  size_t size;
  enum call_status status = read_prefix(source->image, scratch->prefix, &size);
  if (status == CALL_OK) {
    struct shebang script;
    enum shebang_result format = shebang_parse(scratch->prefix, size, &script);
    if (format == SHEBANG_NONE) {
      free(scratch->prefix);
      scratch->prefix = NULL;
    } else if (format == SHEBANG_OK) {
      /* The limit leaves one byte beyond the URI for this terminator, even
       * when EOF rather than LF ends a maximum-length shebang. */
      scratch->prefix[script.interpreter - scratch->prefix + script.length] = '\0';
      status = prepare_script(source, script.interpreter, interpreter_context,
          request, scratch);
    } else {
      status = format == SHEBANG_TOO_LONG ? CALL_LIMIT : CALL_BAD_REQUEST;
    }
  }
  return status;
}

enum call_status program_launch(handle_t launcher, const struct launch_request *request,
    const struct path_context *interpreter_context, handle_t *child)
{
  if (!child) {
    return CALL_BAD_REQUEST;
  }
  *child = HANDLE_INVALID;
  if (!request) {
    return CALL_BAD_REQUEST;
  }

  struct script_scratch scratch = {0};
  struct launch_request prepared;
  enum call_status status = prepare_program(request, interpreter_context,
      &prepared, &scratch);
  if (status == CALL_OK) {
    status = launcher_launch(launcher, &prepared, child);
  }
  release_script(&scratch);
  return status;
}

enum call_status program_create_space(handle_t factory, const struct space_definition *space,
    const uint64_t *cpus, uint64_t cpu_count, const struct launch_request *request,
    const struct path_context *interpreter_context, handle_t *child)
{
  if (!child) {
    return CALL_BAD_REQUEST;
  }
  *child = HANDLE_INVALID;
  if (!request) {
    return CALL_BAD_REQUEST;
  }

  struct script_scratch scratch = {0};
  struct launch_request prepared;
  enum call_status status = prepare_program(request, interpreter_context,
      &prepared, &scratch);
  if (status == CALL_OK) {
    status = space_create_started(factory, space, cpus, cpu_count, &prepared, child);
  }
  release_script(&scratch);
  return status;
}

enum call_status program_launch_batch(handle_t launcher, const struct launch_request *requests,
    size_t count, const struct path_context *interpreter_context,
    handle_t *children, uint64_t *failed_index)
{
  if (failed_index) {
    *failed_index = LAUNCH_NO_STAGE;
  }
  if (children && count > 0 && count <= LAUNCH_BATCH_MAX) {
    for (size_t i = 0; i < count; ++i) {
      children[i] = HANDLE_INVALID;
    }
  }
  if (!requests || !children || !failed_index || !count || count > LAUNCH_BATCH_MAX) {
    return CALL_BAD_REQUEST;
  }

  struct launch_request *prepared = malloc(count * sizeof(*prepared));
  if (!prepared) {
    return CALL_NO_MEMORY;
  }
  struct script_scratch scratch[LAUNCH_BATCH_MAX] = {0};
  enum call_status status = CALL_OK;
  for (size_t i = 0; i < count; ++i) {
    status = prepare_program(&requests[i], interpreter_context,
        &prepared[i], &scratch[i]);
    if (status != CALL_OK) {
      *failed_index = i;
      break;
    }
  }
  if (status == CALL_OK) {
    status = launcher_launch_batch(launcher, prepared, count, children, failed_index);
  }
  for (size_t i = 0; i < count; ++i) {
    release_script(&scratch[i]);
  }
  free(prepared);
  return status;
}
