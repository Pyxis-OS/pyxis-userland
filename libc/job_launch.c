#include <environment.h>
#include <abi/console.h>
#include <abi/directory.h>
#include <abi/file.h>
#include <abi/memory.h>
#include <abi/pipe.h>
#include <abi/stream.h>
#include <errno.h>
#include <file.h>
#include <handle.h>
#include <pyxis/descriptor.h>
#include <pyxis/job.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>
#include "descriptor.h"
#include "job_internal.h"
#include "job_pipeline_launch.h"

struct libc_pipeline_launch {
  struct job_preparation *stages;
  struct launch_request *requests;
  size_t count;
  handle_t private_root;
  bool attempted;
};

struct job_metadata {
  struct job_metadata *next;
  unsigned char bytes[];
};

static enum call_status encoding_limit(struct job_preparation *prepared)
{
  prepared->limit = (struct call_limit){.reason = CALL_LIMIT_REQUEST_ENCODING,
    .requested = UINT64_MAX, .limit = SIZE_MAX,
    .flags = CALL_LIMIT_HAS_REQUESTED | CALL_LIMIT_HAS_LIMIT |
        CALL_LIMIT_REQUESTED_SATURATED};
  return prepared->error = CALL_LIMIT;
}

/* Each owned allocation is stable once its address enters the request. */
static void *capture_array(struct job_preparation *prepared, const void *source,
    size_t count, size_t item_size)
{
  if (!count || prepared->error != CALL_OK) {
    return NULL;
  }
  if (count > (SIZE_MAX - sizeof(struct job_metadata)) / item_size ||
      count > (SIZE_MAX - prepared->used) / item_size) {
    encoding_limit(prepared);
    return NULL;
  }
  size_t bytes = count * item_size;
  struct job_metadata *allocation = malloc(sizeof(*allocation) + bytes);
  if (!allocation) {
    prepared->error = CALL_NO_MEMORY;
    return NULL;
  }
  allocation->next = prepared->metadata;
  prepared->metadata = allocation;
  if (source) {
    memcpy(allocation->bytes, source, bytes);
  } else {
    memset(allocation->bytes, 0, bytes);
  }
  prepared->used += bytes;
  return allocation->bytes;
}

static char *capture_string(struct job_preparation *prepared, const char *source)
{
  if (prepared->error != CALL_OK) {
    return NULL;
  }
  if (!source) {
    prepared->error = CALL_BAD_REQUEST;
    return NULL;
  }
  size_t length = strlen(source);
  if (length == SIZE_MAX) {
    encoding_limit(prepared);
    return NULL;
  }
  return capture_array(prepared, source, length + 1, 1);
}

static void free_metadata(struct job_preparation *prepared)
{
  while (prepared->metadata) {
    struct job_metadata *allocation = prepared->metadata;
    prepared->metadata = allocation->next;
    free(allocation);
  }
}

static bool stream_name(const char *name)
{
  return !strcmp(name, "stdin") || !strcmp(name, "stdout") ||
      !strcmp(name, "stderr") ||
      !strcmp(name, "stdin.append") || !strcmp(name, "stdout.append") ||
      !strcmp(name, "stderr.append");
}

static void stage_streams(struct job_preparation *prepared)
{
  static const char *const names[ENVIRONMENT_STREAM_COUNT] = {"stdin", "stdout", "stderr"};
  struct launch_request *request = &prepared->request;
  for (size_t i = 0; i < ENVIRONMENT_STREAM_COUNT; ++i) {
    struct job_stream_selection *stream = &prepared->selected_streams[i];
    if (stream->protocol) {
      prepared->entries[request->entry_count++] = (struct launch_environment_entry){
        .name = (uintptr_t)names[i], .kind = ENVIRONMENT_CAP,
        .flags = ENVIRONMENT_EXPORTED, .value = stream->grant,
      };
      if (stream->append) {
        prepared->entries[request->entry_count++] = environment_stdio_append_entry(i);
      }
    }
  }
}

static enum call_status capture_metadata(struct job_preparation *prepared,
    const struct pyxis_job_options *options, const bool *pipe_streams)
{
  size_t depth = options->working ? options->working->context.count : 0;
  if ((options->grant_count && !options->grants) ||
      (options->entry_count && !options->entries) ||
      (options->argc && !options->argv) ||
      (depth && !options->working->context.directories) ||
      (options->working && options->working->path && !depth)) {
    return CALL_BAD_REQUEST;
  }
  uint64_t stack = options->initial_stack_bytes;
  if (stack && stack % MEMORY_PAGE_SIZE) {
    return CALL_BAD_REQUEST;
  }
  size_t grant_count = options->grant_count;
  if (depth > SIZE_MAX - grant_count || options->entry_count > SIZE_MAX - 6) {
    return encoding_limit(prepared);
  }
  grant_count += depth;
  for (size_t i = 0; i < ENVIRONMENT_STREAM_COUNT; ++i) {
    if (options->streams[i] < -1) {
      return CALL_BAD_REQUEST;
    }
    if (options->streams[i] >= 0 || (pipe_streams && pipe_streams[i])) {
      if (grant_count == SIZE_MAX) {
        return encoding_limit(prepared);
      }
      ++grant_count;
      if (options->streams[i] >= 0) {
        if (pyxis_descriptor_borrow(options->streams[i], &prepared->streams[i]) < 0) {
          return errno == EBADF ? CALL_BAD_HANDLE : CALL_IO;
        }
      }
    }
  }
  struct launch_request *request = &prepared->request;
  prepared->grants = capture_array(prepared, NULL, grant_count, sizeof(*prepared->grants));
  prepared->entries = capture_array(prepared, NULL, options->entry_count + 6, sizeof(*prepared->entries));
  if (prepared->error != CALL_OK) {
    return prepared->error;
  }
  if (options->grant_count) {
    memcpy(prepared->grants, options->grants, options->grant_count * sizeof(*prepared->grants));
  }
  request->grants = (uintptr_t)prepared->grants;
  request->grant_count = grant_count;
  request->entries = (uintptr_t)prepared->entries;
  for (size_t i = 0; i < options->entry_count; ++i) {
    struct launch_environment_entry entry = options->entries[i];
    const char *name = capture_string(prepared, (const char *)(uintptr_t)entry.name);
    char checked[ENVIRONMENT_NAME_MAX + 1];
    if (!name) {
      return prepared->error;
    }
    if (!environment_name(checked, name)) {
      return CALL_BAD_REQUEST;
    }
    if (stream_name(name)) {
      continue;
    }
    if (entry.flags & ~ENVIRONMENT_EXPORTED) {
      return CALL_BAD_REQUEST;
    }
    for (size_t j = 0; j < request->entry_count; ++j) {
      if (!strcmp(name, (const char *)(uintptr_t)prepared->entries[j].name)) {
        return CALL_BAD_REQUEST;
      }
    }
    entry.name = (uintptr_t)name;
    if (entry.kind == ENVIRONMENT_CAP) {
      if (entry.value >= options->grant_count || entry.length) {
        return CALL_BAD_REQUEST;
      }
    } else if (entry.kind == ENVIRONMENT_TEXT) {
      const char *text = capture_string(prepared, (const char *)(uintptr_t)entry.value);
      if (!text) {
        return prepared->error;
      }
      if (strlen(text) != entry.length) {
        return CALL_BAD_REQUEST;
      }
      entry.value = (uintptr_t)text;
    } else {
      return CALL_BAD_REQUEST;
    }
    prepared->entries[request->entry_count++] = entry;
  }
  uint64_t *directories = capture_array(prepared, NULL, depth, sizeof(*directories));
  request->working_directories = (uintptr_t)directories;
  request->working_directory_count = depth;
  if (prepared->error != CALL_OK) {
    return prepared->error;
  }
  size_t next = options->grant_count;
  for (size_t i = 0; i < depth; ++i) {
    directories[i] = next;
    prepared->grants[next++].source = options->working->context.directories[i];
  }
  for (size_t i = 0; i < ENVIRONMENT_STREAM_COUNT; ++i) {
    struct job_stream_selection *stream = &prepared->selected_streams[i];
    if (options->streams[i] >= 0 || (pipe_streams && pipe_streams[i])) {
      stream->grant = next++;
      if (pipe_streams && pipe_streams[i]) {
        stream->protocol = PROTOCOL_PIPE;
      }
    }
  }
  if (options->working && options->working->path) {
    request->working_path = (uintptr_t)capture_string(prepared, options->working->path);
  }
  const char **argv = capture_array(prepared, options->argv, options->argc, sizeof(*argv));
  request->argv = (uintptr_t)argv;
  request->argc = options->argc;
  for (size_t i = 0; i < options->argc && prepared->error == CALL_OK; ++i) {
    argv[i] = capture_string(prepared, argv[i]);
  }
  request->initial_stack_bytes = stack;
  return prepared->error;
}

static enum call_status validate_directory(handle_t handle, struct handle_info *info)
{
  enum call_status status = handle_query(handle, info);
  if (status != CALL_OK) {
    return status;
  }
  return info->protocol == PROTOCOL_DIRECTORY && info->kind == HANDLE_KIND_NATIVE &&
      !info->transport ? CALL_OK : CALL_WRONG_TYPE;
}

static enum call_status capture_stream(struct job_preparation *prepared,
    enum environment_stream_index index)
{
  struct pyxis_descriptor_binding binding = prepared->streams[index];
  bool input = index == ENVIRONMENT_STDIN;
  if (!(binding.access & (input ? PYXIS_DESCRIPTOR_READ : PYXIS_DESCRIPTOR_WRITE))) {
    return CALL_DENIED;
  }
  if (binding.append && (input || binding.info.protocol != PROTOCOL_FILE)) {
    return CALL_BAD_REQUEST;
  }
  uint64_t rights, transport = 0;
  if (binding.info.protocol == PROTOCOL_FILE) {
    rights = input ? FILE_RIGHT_READ : FILE_RIGHT_WRITE;
    if (binding.info.kind == HANDLE_KIND_EXPORTED) {
      transport = HANDLE_TRANSPORT_CALL;
    } else if (binding.info.kind != HANDLE_KIND_NATIVE) {
      return CALL_WRONG_TYPE;
    }
  } else if (binding.info.protocol == PROTOCOL_STREAM && input &&
      binding.info.kind == HANDLE_KIND_EXPORTED) {
    rights = STREAM_RIGHT_READ;
    transport = HANDLE_TRANSPORT_CALL;
  } else if (binding.info.protocol == PROTOCOL_PIPE && binding.info.kind == HANDLE_KIND_NATIVE) {
    rights = input ? PIPE_RIGHT_READ : PIPE_RIGHT_WRITE;
  } else if (binding.info.protocol == PROTOCOL_CONSOLE && binding.info.kind == HANDLE_KIND_NATIVE) {
    rights = input ? CONSOLE_RIGHT_READ : CONSOLE_RIGHT_WRITE;
  } else {
    return CALL_WRONG_TYPE;
  }
  struct job_stream_selection *stream = &prepared->selected_streams[index];
  if ((binding.info.rights & rights) != rights ||
      (binding.info.transport & transport) != transport) {
    return CALL_DENIED;
  }
  if (binding.append) {
    uint64_t offset;
    size_t written;
    enum call_status status = file_append(binding.handle, NULL, 0, &offset, &written);
    if (status != CALL_OK) {
      return status;
    }
  }
  stream->protocol = binding.info.protocol;
  stream->append = binding.append;
  prepared->grants[stream->grant] = (struct launch_grant){binding.handle, rights, transport};
  return CALL_OK;
}

static enum call_status select_handles(struct job_preparation *prepared,
    const struct pyxis_job_options *options)
{
  struct handle_info info;
  enum call_status status = handle_query(options->image, &info);
  if (status != CALL_OK) {
    return status;
  }
  if (info.protocol != PROTOCOL_FILE || info.kind != HANDLE_KIND_NATIVE || info.transport) {
    return CALL_WRONG_TYPE;
  }
  for (size_t i = 0; i < prepared->request.working_directory_count; ++i) {
    struct launch_grant *grant = &prepared->grants[options->grant_count + i];
    status = validate_directory(grant->source, &info);
    if (status != CALL_OK) {
      return status;
    }
    grant->rights = info.rights;
    grant->transport = info.transport;
  }
  return CALL_OK;
}

static enum call_status validate_launcher(handle_t launcher)
{
  struct handle_info info;
  enum call_status status = handle_query(launcher, &info);
  if (status != CALL_OK) {
    return status;
  }
  return info.protocol == PROTOCOL_LAUNCHER && info.kind == HANDLE_KIND_NATIVE &&
      !info.transport ? CALL_OK : CALL_WRONG_TYPE;
}

static enum call_status copy_grants(struct job_preparation *prepared, size_t count)
{
  for (size_t i = prepared->copied_grants; i < count; ++i) {
    struct launch_grant *grant = &prepared->grants[i];
    handle_t copy;
    enum call_status status = handle_copy_restricted_with_limit(grant->source,
        grant->rights, grant->transport, &copy, &prepared->limit);
    if (status != CALL_OK) {
      return status;
    }
    grant->source = copy;
    ++prepared->copied_grants;
  }
  return CALL_OK;
}

static enum call_status copy_image(struct job_preparation *prepared, handle_t image)
{
  enum call_status status = handle_copy_restricted_with_limit(image, FILE_RIGHT_READ, 0,
      &prepared->image, &prepared->limit);
  if (status == CALL_OK) {
    prepared->request.image = prepared->image;
  }
  return status;
}

static enum call_status capture_handles(struct job_preparation *prepared,
    handle_t launcher, const struct pyxis_job_options *options, handle_t private_root)
{
  enum call_status status = validate_launcher(launcher);
  if (status == CALL_OK) {
    status = select_handles(prepared, options);
  }
  if (status != CALL_OK) {
    return status;
  }
  for (size_t i = 0; i < ENVIRONMENT_STREAM_COUNT; ++i) {
    if (options->streams[i] >= 0) {
      status = capture_stream(prepared, i);
      if (status != CALL_OK) {
        return status;
      }
    }
  }
  stage_streams(prepared);
  if (private_root != HANDLE_INVALID) {
    if (prepared->selected_streams[ENVIRONMENT_STDIN].protocol != 0 ||
        prepared->selected_streams[ENVIRONMENT_STDOUT].protocol != PROTOCOL_FILE ||
        prepared->selected_streams[ENVIRONMENT_STDERR].protocol != PROTOCOL_FILE) {
      return CALL_DENIED;
    }
    status = libc_job_validate_capture_grants(prepared->grants,
        prepared->request.grant_count, private_root);
  } else {
    status = libc_job_validate_grants(prepared->grants, prepared->request.grant_count);
  }
  if (status != CALL_OK) {
    return status;
  }
  status = handle_copy_restricted_with_limit(launcher, LAUNCHER_RIGHT_LAUNCH, 0, &prepared->launcher, &prepared->limit);
  if (status != CALL_OK) {
    return status;
  }
  status = copy_image(prepared, options->image);
  if (status != CALL_OK) {
    return status;
  }
  return copy_grants(prepared, prepared->request.grant_count);
}

static enum call_status release_handle(handle_t handle)
{
  if (handle == HANDLE_INVALID) {
    return CALL_OK;
  }
  struct syscall_result result = syscall_close(handle);
  return result.status >= CALL_STATUS_COUNT || result.reply_size ?
      CALL_OUTCOME_UNKNOWN : result.status;
}

enum call_status libc_job_discard_preparation(struct job_preparation *prepared)
{
  enum call_status error = CALL_OK;
  for (size_t i = 0; i < prepared->copied_grants; ++i) {
    enum call_status status = release_handle(prepared->grants[i].source);
    if (error == CALL_OK) {
      error = status;
    }
  }
  enum call_status status = release_handle(prepared->image);
  if (error == CALL_OK) {
    error = status;
  }
  status = release_handle(prepared->launcher);
  if (error == CALL_OK) {
    error = status;
  }
  free_metadata(prepared);
  prepared->metadata = NULL;
  prepared->copied_grants = 0;
  prepared->image = HANDLE_INVALID;
  prepared->launcher = HANDLE_INVALID;
  return error;
}

enum call_status libc_job_validate_metadata(const struct pyxis_job_options *options,
    struct call_limit *limit)
{
  if (limit) {
    *limit = (struct call_limit){0};
  }
  if (!options) {
    return CALL_BAD_REQUEST;
  }
  struct job_preparation prepared = {0};
  enum call_status status = capture_metadata(&prepared, options, NULL);
  if (limit && status == CALL_LIMIT) {
    *limit = prepared.limit;
  }
  free_metadata(&prepared);
  return status;
}

enum call_status libc_job_prepare(struct job_preparation *prepared, handle_t launcher,
    const struct pyxis_job_options *options, handle_t private_root, struct call_limit *limit)
{
  if (limit) {
    *limit = (struct call_limit){0};
  }
  struct pyxis_job_options selected = *options;
  enum call_status status = capture_metadata(prepared, &selected, NULL);
  if (status == CALL_OK) {
    status = capture_handles(prepared, launcher, &selected, private_root);
  }
  if (status == CALL_OK) {
    for (size_t i = ENVIRONMENT_STDOUT; i < ENVIRONMENT_STREAM_COUNT; ++i) {
      if (selected.streams[i] >= 0 && libc_stdio_flush_descriptor(selected.streams[i]) < 0) {
        status = CALL_IO;
        break;
      }
    }
  }
  if (limit) {
    *limit = status == CALL_LIMIT ? prepared->limit : (struct call_limit){0};
  }
  return status;
}

enum call_status libc_job_publish(struct job_preparation *prepared, handle_t launcher,
    handle_t *observer, struct launch_observation *observation, struct call_limit *limit)
{
  if (limit) {
    *limit = (struct call_limit){0};
  }
  if (observation) {
    observation->registration = (struct readiness_set_registration){0};
  }
  *observer = HANDLE_INVALID;
  if (prepared->attempted) {
    return CALL_BUSY;
  }
  prepared->attempted = true;
  return observation ?
      launcher_launch_observed(launcher, &prepared->request, observation, observer, limit) :
      launcher_launch(launcher, &prepared->request, observer, limit);
}

enum call_status libc_job_launch(handle_t launcher,
    const struct pyxis_job_options *options, handle_t private_root, handle_t *observer,
    struct launch_observation *observation, struct call_limit *limit)
{
  if (limit) {
    *limit = (struct call_limit){0};
  }
  if (observation) {
    observation->registration = (struct readiness_set_registration){0};
  }
  *observer = HANDLE_INVALID;
  struct job_preparation prepared = {0};
  enum call_status status = libc_job_prepare(&prepared, launcher, options, private_root, limit);
  if (status == CALL_OK) {
    status = libc_job_publish(&prepared, prepared.launcher, observer, observation, limit);
  }
  int saved_errno = errno;
  libc_job_discard_preparation(&prepared);
  errno = saved_errno;
  return status;
}

enum call_status libc_pipeline_discard(struct libc_pipeline_launch *prepared)
{
  if (!prepared) {
    return CALL_OK;
  }
  enum call_status error = CALL_OK;
  for (size_t i = 0; i < prepared->count; ++i) {
    struct job_preparation *stage = &prepared->stages[i];
    for (size_t j = 0; j < stage->copied_grants; ++j) {
      enum call_status status = release_handle(stage->grants[j].source);
      if (error == CALL_OK) {
        error = status;
      }
    }
    enum call_status status = release_handle(stage->image);
    if (error == CALL_OK) {
      error = status;
    }
    free_metadata(stage);
  }
  enum call_status status = release_handle(prepared->private_root);
  if (error == CALL_OK) {
    error = status;
  }
  free(prepared->requests);
  free(prepared->stages);
  free(prepared);
  return error;
}

enum call_status libc_pipeline_prepare(const struct pyxis_job_options *stages,
    size_t count, handle_t private_root, bool has_input,
    struct libc_pipeline_launch **result, struct call_limit *limit)
{
  if (limit) {
    *limit = (struct call_limit){0};
  }
  if (result) {
    *result = NULL;
  }
  if (!result || !stages || !count ||
      private_root == HANDLE_INVALID) {
    return CALL_BAD_REQUEST;
  }
  struct libc_pipeline_launch *prepared = calloc(1, sizeof(*prepared));
  if (!prepared) {
    return CALL_NO_MEMORY;
  }
  if (count > SIZE_MAX / sizeof(*prepared->stages) ||
      count > SIZE_MAX / sizeof(*prepared->requests)) {
    free(prepared);
    if (limit) {
      *limit = (struct call_limit){.reason = CALL_LIMIT_REQUEST_ENCODING,
        .requested = UINT64_MAX, .limit = SIZE_MAX,
        .flags = CALL_LIMIT_HAS_REQUESTED | CALL_LIMIT_HAS_LIMIT |
            CALL_LIMIT_REQUESTED_SATURATED};
    }
    return CALL_LIMIT;
  }
  prepared->stages = calloc(count, sizeof(*prepared->stages));
  prepared->requests = calloc(count, sizeof(*prepared->requests));
  if (!prepared->stages || !prepared->requests) {
    libc_pipeline_discard(prepared);
    return CALL_NO_MEMORY;
  }
  prepared->count = count;
  enum call_status status = CALL_OK;
  for (size_t i = 0; i < count; ++i) {
    for (size_t j = 0; j < ENVIRONMENT_STREAM_COUNT; ++j) {
      if (stages[i].streams[j] != -1) {
        status = CALL_BAD_REQUEST;
        break;
      }
    }
    if (status != CALL_OK) {
      break;
    }
    struct job_preparation *stage = &prepared->stages[i];
    const bool pipe_streams[ENVIRONMENT_STREAM_COUNT] = {i != 0 || has_input, true, true};
    status = capture_metadata(stage, &stages[i], pipe_streams);
    if (status == CALL_OK) {
      stage_streams(stage);
    }
    if (status != CALL_OK) {
      if (limit && status == CALL_LIMIT) {
        *limit = stage->limit;
      }
      break;
    }
  }
  if (status == CALL_OK) {
    /* Holding the identity requires no directory content authority. */
    status = handle_copy_restricted_with_limit(private_root, 0, 0, &prepared->private_root, limit);
  }
  for (size_t i = 0; i < count && status == CALL_OK; ++i) {
    struct job_preparation *stage = &prepared->stages[i];
    status = select_handles(stage, &stages[i]);
    size_t stream_count = ENVIRONMENT_STREAM_COUNT - (i == 0 && !has_input);
    size_t selected_count = stage->request.grant_count - stream_count;
    if (status == CALL_OK) {
      status = libc_job_validate_capture_grants(stage->grants, selected_count,
          private_root);
    }
    if (status == CALL_OK) {
      status = copy_image(stage, stages[i].image);
    }
    if (status == CALL_OK) {
      status = copy_grants(stage, selected_count);
    }
    if (limit && status == CALL_LIMIT) {
      *limit = stage->limit;
    }
  }
  if (status != CALL_OK) {
    libc_pipeline_discard(prepared);
    return status;
  }
  *result = prepared;
  return CALL_OK;
}

enum call_status libc_pipeline_launch(struct libc_pipeline_launch *prepared,
    handle_t launcher, const handle_t streams[][ENVIRONMENT_STREAM_COUNT],
    handle_t *observers, uint64_t *failed_index, struct call_limit *limit)
{
  if (limit) {
    *limit = (struct call_limit){0};
  }
  if (prepared && prepared->attempted) {
    return CALL_BUSY;
  }
  if (failed_index) {
    *failed_index = LAUNCH_NO_STAGE;
  }
  if (prepared && observers) {
    for (size_t i = 0; i < prepared->count; ++i) {
      observers[i] = HANDLE_INVALID;
    }
  }
  if (!prepared || !streams || !observers || !failed_index) {
    return CALL_BAD_REQUEST;
  }
  prepared->attempted = true;
  enum call_status status = validate_launcher(launcher);
  if (status != CALL_OK) {
    return status;
  }
  for (size_t i = 0; i < prepared->count; ++i) {
    struct job_preparation *stage = &prepared->stages[i];
    for (size_t j = 0; j < ENVIRONMENT_STREAM_COUNT; ++j) {
      struct job_stream_selection *stream = &stage->selected_streams[j];
      if (stream->protocol == 0) {
        if (streams[i][j] != HANDLE_INVALID) {
          status = CALL_BAD_REQUEST;
          break;
        }
        continue;
      }
      struct handle_info info;
      status = handle_query(streams[i][j], &info);
      if (status != CALL_OK) {
        break;
      }
      if (info.protocol != PROTOCOL_PIPE || info.kind != HANDLE_KIND_NATIVE || info.transport) {
        status = CALL_WRONG_TYPE;
        break;
      }
      stage->grants[stream->grant] = (struct launch_grant){
        streams[i][j], j == ENVIRONMENT_STDIN ? PIPE_RIGHT_READ : PIPE_RIGHT_WRITE, 0,
      };
    }
    if (status == CALL_OK) {
      status = libc_job_validate_capture_grants(stage->grants,
          stage->request.grant_count, prepared->private_root);
    }
    if (status != CALL_OK) {
      *failed_index = i;
      return status;
    }
  }
  struct launch_request *requests = prepared->requests;
  for (size_t i = 0; i < prepared->count; ++i) {
    struct job_preparation *stage = &prepared->stages[i];
    status = copy_grants(stage, stage->request.grant_count);
    if (status != CALL_OK) {
      *failed_index = i;
      if (limit && status == CALL_LIMIT) {
        *limit = stage->limit;
      }
      return status;
    }
    requests[i] = stage->request;
  }
  return launcher_launch_batch(launcher, requests, prepared->count, observers, failed_index, limit);
}
