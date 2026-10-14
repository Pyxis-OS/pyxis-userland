#include <environment.h>
#include <abi/console.h>
#include <abi/directory.h>
#include <abi/file.h>
#include <abi/memory.h>
#include <abi/pipe.h>
#include <abi/http_transaction.h>
#include <errno.h>
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
  struct job_preparation stages[LAUNCH_BATCH_MAX];
  size_t count;
  handle_t private_root;
  bool attempted;
};

/* Match the kernel's capture order and eight-byte array alignment, so this
 * owned snapshot cannot exceed the existing synchronous capture budget. */
static void *capture_array(struct job_preparation *prepared, const void *source,
    size_t count, size_t item_size)
{
  if (!count || prepared->error != CALL_OK) {
    return NULL;
  }
  size_t offset = (prepared->used + 7) & ~(size_t)7;
  if (offset > LAUNCH_CAPTURE_MAX_SIZE ||
      count > (LAUNCH_CAPTURE_MAX_SIZE - offset) / item_size) {
    prepared->error = CALL_BAD_REQUEST;
    return NULL;
  }
  void *result = prepared->metadata + offset;
  size_t bytes = count * item_size;
  if (source) {
    memcpy(result, source, bytes);
  } else {
    memset(result, 0, bytes);
  }
  prepared->used = offset + bytes;
  return result;
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
  size_t remaining = LAUNCH_CAPTURE_MAX_SIZE - prepared->used;
  size_t length = strnlen(source, remaining);
  if (length == remaining) {
    prepared->error = CALL_BAD_REQUEST;
    return NULL;
  }
  char *result = (char *)prepared->metadata + prepared->used;
  memcpy(result, source, length + 1);
  prepared->used += length + 1;
  return result;
}

static bool stream_name(const char *name)
{
  return !strcmp(name, "stdin") || !strcmp(name, "stdout") ||
      !strcmp(name, "stderr") || !strcmp(name, "stdin.status");
}

static enum call_status validate_startup_size(const struct launch_request *request)
{
  size_t metadata = sizeof(struct startup_info) + request->working_directory_count * sizeof(handle_t);
  if (request->working_path) {
    metadata += strlen((const char *)(uintptr_t)request->working_path) + 1;
  }
  size_t arguments = (request->argc + 1) * sizeof(uint64_t);
  const char *const *argv = (const void *)(uintptr_t)request->argv;
  for (size_t i = 0; i < request->argc; ++i) {
    arguments += strlen(argv[i]) + 1;
  }
  metadata = (metadata + MEMORY_PAGE_SIZE - 1) & ~(size_t)(MEMORY_PAGE_SIZE - 1);
  arguments = (arguments + MEMORY_PAGE_SIZE - 1) & ~(size_t)(MEMORY_PAGE_SIZE - 1);
  return metadata <= STARTUP_MAX_SIZE && arguments <= STARTUP_MAX_SIZE - metadata ? CALL_OK : CALL_BAD_REQUEST;
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
      if (stream->observation_protocol) {
        prepared->entries[request->entry_count++] = (struct launch_environment_entry){
          .name = (uintptr_t)"stdin.status", .kind = ENVIRONMENT_CAP,
          .flags = ENVIRONMENT_EXPORTED, .value = stream->observation_grant,
        };
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
  if (stack && (stack < LAUNCH_INITIAL_STACK_MIN_SIZE ||
      stack > LAUNCH_INITIAL_STACK_MAX_SIZE || stack % MEMORY_PAGE_SIZE)) {
    return CALL_BAD_REQUEST;
  }
  size_t limit = LAUNCH_CAPTURE_MAX_SIZE / sizeof(struct launch_grant);
  size_t grant_count = options->grant_count;
  if (grant_count > limit || depth > limit - grant_count || options->entry_count > limit - 4) {
    return CALL_BAD_REQUEST;
  }
  grant_count += depth;
  for (size_t i = 0; i < ENVIRONMENT_STREAM_COUNT; ++i) {
    if (options->streams[i] < -1) {
      return CALL_BAD_REQUEST;
    }
    if (options->streams[i] >= 0 || (pipe_streams && pipe_streams[i])) {
      if (grant_count == limit) {
        return CALL_BAD_REQUEST;
      }
      ++grant_count;
      if (options->streams[i] >= 0) {
        if (pyxis_descriptor_borrow(options->streams[i], &prepared->streams[i]) < 0) {
          return errno == EBADF ? CALL_BAD_HANDLE : CALL_IO;
        }
        if (prepared->streams[i].observation != HANDLE_INVALID) {
          if (grant_count == limit) {
            return CALL_BAD_REQUEST;
          }
          ++grant_count;
        }
      }
    }
  }
  struct launch_request *request = &prepared->request;
  prepared->grants = capture_array(prepared, NULL, grant_count, sizeof(*prepared->grants));
  prepared->entries = capture_array(prepared, NULL, options->entry_count + 4, sizeof(*prepared->entries));
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
    if (!name || !environment_name(checked, name)) {
      return CALL_BAD_REQUEST;
    }
    if (stream_name(name)) {
      continue;
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
      if (!text || strlen(text) != entry.length) {
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
      if (options->streams[i] >= 0 && prepared->streams[i].observation != HANDLE_INVALID) {
        stream->observation_grant = next++;
      }
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
  return prepared->error == CALL_OK ? validate_startup_size(request) : prepared->error;
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
    if (binding.observation == HANDLE_INVALID || binding.observation == binding.handle ||
        binding.observation_info.protocol != PROTOCOL_HTTP_TRANSACTION ||
        binding.observation_info.kind != HANDLE_KIND_EXPORTED ||
        !(binding.observation_info.rights & HTTP_TRANSACTION_RIGHT_STATUS) ||
        (binding.observation_info.transport & HANDLE_TRANSPORT_CALL) != HANDLE_TRANSPORT_CALL) {
      return CALL_WRONG_TYPE;
    }
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
  if (binding.info.protocol == PROTOCOL_STREAM) {
    stream->observation_protocol = PROTOCOL_HTTP_TRANSACTION;
    prepared->grants[stream->observation_grant] = (struct launch_grant){
      binding.observation, HTTP_TRANSACTION_RIGHT_STATUS, HANDLE_TRANSPORT_CALL,
    };
  } else if (binding.observation != HANDLE_INVALID) {
    return CALL_BAD_REQUEST;
  }
  stream->protocol = binding.info.protocol;
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
    enum call_status status = handle_copy_restricted(grant->source,
        grant->rights, grant->transport, &copy);
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
  enum call_status status = handle_copy_restricted(image, FILE_RIGHT_READ, 0,
      &prepared->image);
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
  status = handle_copy_restricted(launcher, LAUNCHER_RIGHT_LAUNCH, 0, &prepared->launcher);
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
  free(prepared->metadata);
  prepared->metadata = NULL;
  prepared->copied_grants = 0;
  prepared->image = HANDLE_INVALID;
  prepared->launcher = HANDLE_INVALID;
  return error;
}

enum call_status libc_job_prepare(struct job_preparation *prepared, handle_t launcher,
    const struct pyxis_job_options *options, handle_t private_root)
{
  struct pyxis_job_options selected = *options;
  prepared->metadata = malloc(LAUNCH_CAPTURE_MAX_SIZE);
  if (!prepared->metadata) {
    return CALL_NO_MEMORY;
  }
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
  return status;
}

enum call_status libc_job_publish(struct job_preparation *prepared, handle_t launcher,
    handle_t *observer)
{
  *observer = HANDLE_INVALID;
  if (prepared->attempted) {
    return CALL_BUSY;
  }
  prepared->attempted = true;
  return launcher_launch(launcher, &prepared->request, observer);
}

enum call_status libc_job_launch(handle_t launcher,
    const struct pyxis_job_options *options, handle_t private_root, handle_t *observer)
{
  *observer = HANDLE_INVALID;
  struct job_preparation prepared = {0};
  enum call_status status = libc_job_prepare(&prepared, launcher, options, private_root);
  if (status == CALL_OK) {
    status = libc_job_publish(&prepared, prepared.launcher, observer);
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
    free(stage->metadata);
  }
  enum call_status status = release_handle(prepared->private_root);
  if (error == CALL_OK) {
    error = status;
  }
  free(prepared);
  return error;
}

enum call_status libc_pipeline_prepare(const struct pyxis_job_options *stages,
    size_t count, handle_t private_root, bool has_input,
    struct libc_pipeline_launch **result)
{
  if (result) {
    *result = NULL;
  }
  if (!result || !stages || !count || count > LAUNCH_BATCH_MAX ||
      private_root == HANDLE_INVALID) {
    return CALL_BAD_REQUEST;
  }
  struct libc_pipeline_launch *prepared = calloc(1, sizeof(*prepared));
  if (!prepared) {
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
    stage->metadata = malloc(LAUNCH_CAPTURE_MAX_SIZE);
    if (!stage->metadata) {
      status = CALL_NO_MEMORY;
      break;
    }
    const bool pipe_streams[ENVIRONMENT_STREAM_COUNT] = {i != 0 || has_input, true, true};
    status = capture_metadata(stage, &stages[i], pipe_streams);
    if (status == CALL_OK) {
      stage_streams(stage);
    }
    if (status != CALL_OK) {
      break;
    }
  }
  if (status == CALL_OK) {
    /* Holding the identity requires no directory content authority. */
    status = handle_copy_restricted(private_root, 0, 0, &prepared->private_root);
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
    handle_t *observers, uint64_t *failed_index)
{
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
  struct launch_request requests[LAUNCH_BATCH_MAX];
  for (size_t i = 0; i < prepared->count; ++i) {
    struct job_preparation *stage = &prepared->stages[i];
    status = copy_grants(stage, stage->request.grant_count);
    if (status != CALL_OK) {
      *failed_index = i;
      return status;
    }
    requests[i] = stage->request;
  }
  return launcher_launch_batch(launcher, requests, prepared->count, observers, failed_index);
}
