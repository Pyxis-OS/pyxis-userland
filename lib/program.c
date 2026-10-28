#include <abi/file.h>
#include <file.h>
#include <handle.h>
#include <launcher.h>
#include <path.h>
#include <pxe/shebang.h>
#include <space.h>
#include <stdlib.h>
#include <string.h>

struct program_image {
  handle_t image;
  handle_t selected_interpreter;
  char *prefix;
  const char *interpreter;
};

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

void program_image_close(struct program_image *prepared)
{
  if (prepared) {
    if (prepared->image != HANDLE_INVALID) {
      handle_close(prepared->image);
    }
    if (prepared->selected_interpreter != HANDLE_INVALID) {
      handle_close(prepared->selected_interpreter);
    }
    free(prepared->prefix);
    free(prepared);
  }
}

enum call_status program_image_prepare(handle_t image, struct program_image **result)
{
  if (!result) {
    return CALL_BAD_REQUEST;
  }
  *result = NULL;
  struct handle_info info;
  enum call_status status = handle_query(image, &info);
  if (status != CALL_OK) {
    return status;
  }
  if (info.protocol != PROTOCOL_FILE ||
      (info.kind != HANDLE_KIND_NATIVE && info.kind != HANDLE_KIND_EXPORTED)) {
    return CALL_WRONG_TYPE;
  }
  struct program_image *prepared = calloc(1, sizeof(*prepared));
  if (!prepared) {
    return CALL_NO_MEMORY;
  }
  uint64_t transport = info.kind == HANDLE_KIND_EXPORTED ? HANDLE_TRANSPORT_CALL : 0;
  status = handle_copy_restricted(image, FILE_RIGHT_READ, transport, &prepared->image);
  if (status == CALL_OK) {
    prepared->prefix = malloc(SHEBANG_PREFIX_SIZE);
    if (!prepared->prefix) {
      status = CALL_NO_MEMORY;
    }
  }
  size_t size;
  if (status == CALL_OK) {
    status = read_prefix(prepared->image, prepared->prefix, &size);
  }
  if (status == CALL_OK) {
    struct shebang script;
    enum shebang_result format = shebang_parse(prepared->prefix, size, &script);
    if (format == SHEBANG_NONE) {
      free(prepared->prefix);
      prepared->prefix = NULL;
    } else if (format == SHEBANG_OK) {
      /* The parser leaves one byte beyond even a maximum-length URI. */
      prepared->prefix[script.interpreter - prepared->prefix + script.length] = '\0';
      prepared->interpreter = script.interpreter;
    } else {
      status = format == SHEBANG_TOO_LONG ? CALL_LIMIT : CALL_BAD_REQUEST;
    }
  }
  if (status != CALL_OK) {
    program_image_close(prepared);
    return status;
  }
  *result = prepared;
  return CALL_OK;
}

const char *program_image_interpreter(const struct program_image *prepared)
{
  return prepared ? prepared->interpreter : NULL;
}

enum call_status program_image_select_interpreter(struct program_image *prepared, handle_t image)
{
  if (!prepared || !prepared->interpreter) {
    return CALL_BAD_REQUEST;
  }
  struct handle_info info;
  enum call_status status = handle_query(image, &info);
  if (status != CALL_OK) {
    return status;
  }
  if (info.kind != HANDLE_KIND_NATIVE || info.protocol != PROTOCOL_FILE) {
    return CALL_WRONG_TYPE;
  }
  handle_t selected = HANDLE_INVALID;
  status = handle_copy_restricted(image, FILE_RIGHT_READ, 0, &selected);
  if (status != CALL_OK) {
    return status;
  }
  if (prepared->selected_interpreter != HANDLE_INVALID) {
    handle_close(prepared->selected_interpreter);
  }
  prepared->selected_interpreter = selected;
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
  struct path_workspace workspace = {
    .directories = directories, .directory_capacity = capacity,
    .component = component, .component_capacity = capacity,
  };
  enum call_status status = path_resolve(context, uri, DIRECTORY_KIND_FILE, FILE_RIGHT_READ,
      &workspace, image);
  free(component);
  free(directories);
  return status;
}

struct script_scratch {
  struct program_image *input;
  struct launch_grant *grants;
  struct launch_environment_entry *entries;
  const char **arguments;
  handle_t image;
};

static void release_script(struct script_scratch *scratch)
{
  if (scratch->image != HANDLE_INVALID) {
    handle_close(scratch->image);
  }
  free(scratch->arguments);
  free(scratch->entries);
  free(scratch->grants);
  program_image_close(scratch->input);
}

static enum call_status prepare_script(const struct launch_request *source,
    const struct program_image *input, const struct path_context *interpreter_context,
    struct launch_request *request, struct script_scratch *scratch)
{
  if (!source->argc || !source->argv ||
      (source->grant_count && !source->grants) ||
      (source->entry_count && !source->entries)) {
    return CALL_BAD_REQUEST;
  }
  if (source->argc >= LAUNCH_CAPTURE_MAX_SIZE / sizeof(char *) - 1 ||
      source->grant_count >= LAUNCH_CAPTURE_MAX_SIZE / sizeof(struct launch_grant) ||
      source->entry_count >= LAUNCH_CAPTURE_MAX_SIZE / sizeof(struct launch_environment_entry)) {
    return CALL_LIMIT;
  }
  const struct launch_environment_entry *original_entries = (const void *)(uintptr_t)source->entries;
  for (size_t i = 0; i < source->entry_count; ++i) {
    if (!strcmp((const char *)(uintptr_t)original_entries[i].name, "script")) {
      return CALL_BAD_REQUEST;
    }
  }

  if (source->grant_count >= LAUNCH_CAPTURE_MAX_SIZE / sizeof(struct launch_grant) - 1 ||
      source->entry_count >= LAUNCH_CAPTURE_MAX_SIZE / sizeof(struct launch_environment_entry) - 1) {
    return CALL_LIMIT;
  }

  scratch->grants = malloc((source->grant_count + 1) * sizeof(*scratch->grants));
  scratch->entries = malloc((source->entry_count + 1) * sizeof(*scratch->entries));
  scratch->arguments = malloc((source->argc + 2) * sizeof(*scratch->arguments));
  if (!scratch->grants || !scratch->entries || !scratch->arguments) {
    return CALL_NO_MEMORY;
  }
  enum call_status status;
  if (input->selected_interpreter != HANDLE_INVALID) {
    /* The preparation pins selected code; failure never resolves mutable roots. */
    status = handle_copy_restricted(input->selected_interpreter, FILE_RIGHT_READ, 0,
        &scratch->image);
  } else {
    status = open_interpreter(interpreter_context, input->interpreter, &scratch->image);
  }
  if (status != CALL_OK) {
    return status;
  }

  struct handle_info info;
  enum call_status query = handle_query(source->image, &info);
  if (query != CALL_OK) {
    return query;
  }
  if (info.protocol != PROTOCOL_FILE) {
    return CALL_WRONG_TYPE;
  }
  if (source->grant_count) {
    memcpy(scratch->grants, (const void *)(uintptr_t)source->grants,
        source->grant_count * sizeof(*scratch->grants));
  }
  uint64_t transport = info.kind == HANDLE_KIND_EXPORTED ? HANDLE_TRANSPORT_CALL : 0;
  scratch->grants[source->grant_count] =
      (struct launch_grant){source->image, FILE_RIGHT_READ, transport};
  if (source->entry_count) {
    memcpy(scratch->entries, original_entries,
        source->entry_count * sizeof(*scratch->entries));
  }
  scratch->entries[source->entry_count] =
      (struct launch_environment_entry){
      .name = (uintptr_t)"script", .kind = ENVIRONMENT_CAP,
      .flags = ENVIRONMENT_EXPORTED, .value = source->grant_count};
  scratch->arguments[0] = input->interpreter;
  memcpy(scratch->arguments + 1, (const void *)(uintptr_t)source->argv,
      source->argc * sizeof(*scratch->arguments));
  scratch->arguments[source->argc + 1] = NULL;

  request->image = scratch->image;
  request->grants = (uintptr_t)scratch->grants;
  request->grant_count++;
  request->entries = (uintptr_t)scratch->entries;
  request->entry_count++;
  request->argv = (uintptr_t)scratch->arguments;
  request->argc++;
  /* The low-level launcher accepts only PXE, so another script cannot recurse.
   * Appending the new grant preserves ordinary and standard-stream indices;
   * streams still refer to their sole child grant, without a script-side copy. */
  return CALL_OK;
}

static enum call_status prepare_program(const struct launch_request *source,
    const struct program_image *input, const struct path_context *interpreter_context,
    struct launch_request *request, struct script_scratch *scratch)
{
  if (!input) {
    enum call_status status = program_image_prepare(source->image, &scratch->input);
    if (status != CALL_OK) {
      return status;
    }
    input = scratch->input;
  }
  struct launch_request selected = *source;
  selected.image = input->image;
  *request = selected;
  return input->interpreter ? prepare_script(&selected, input, interpreter_context,
      request, scratch) : CALL_OK;
}

enum call_status program_launch(handle_t launcher, const struct launch_request *request,
    const struct path_context *interpreter_context, handle_t *child)
{
  return program_launch_prepared(launcher, request, NULL, interpreter_context, child);
}

enum call_status program_launch_prepared(handle_t launcher, const struct launch_request *request,
    const struct program_image *input, const struct path_context *interpreter_context,
    handle_t *child)
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
  enum call_status status = prepare_program(request, input, interpreter_context,
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
  enum call_status status = prepare_program(request, NULL, interpreter_context,
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
  return program_launch_batch_prepared(launcher, requests, count, NULL, interpreter_context,
      children, failed_index);
}

enum call_status program_launch_batch_prepared(handle_t launcher,
    const struct launch_request *requests, size_t count,
    const struct program_image *const *inputs, const struct path_context *interpreter_context,
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
    status = prepare_program(&requests[i], inputs ? inputs[i] : NULL, interpreter_context,
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
