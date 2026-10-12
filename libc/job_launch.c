#include <abi/console.h>
#include <abi/directory.h>
#include <abi/file.h>
#include <abi/memory.h>
#include <abi/namespace.h>
#include <abi/pipe.h>
#include <errno.h>
#include <handle.h>
#include <pyxis/descriptor.h>
#include <pyxis/job.h>
#include <stdlib.h>
#include <string.h>
#include "descriptor.h"
#include "job_internal.h"

struct job_preparation {
  struct launch_request request;
  unsigned char *metadata;
  size_t used;
  enum call_status error;
  struct launch_grant *grants;
  size_t copied_grants;
  handle_t image, launcher;
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

static struct launch_binding *capture_bindings(struct job_preparation *prepared,
    const struct launch_binding *source, size_t count, size_t grant_count,
    bool roots)
{
  struct launch_binding *bindings = capture_array(prepared, source, count,
      sizeof(*bindings));
  for (size_t i = 0; i < count && prepared->error == CALL_OK; ++i) {
    if (bindings[i].grant >= grant_count) {
      prepared->error = CALL_BAD_REQUEST;
      break;
    }
    const char *name = capture_string(prepared, (const char *)(uintptr_t)bindings[i].name);
    if (!name) {
      break;
    }
    bindings[i].name = (uintptr_t)name;
    if (!*name || (roots && (strchr(name, ':') || strchr(name, '/')))) {
      prepared->error = CALL_BAD_REQUEST;
      break;
    }
    for (size_t j = 0; j < i; ++j) {
      if (!strcmp(name, (const char *)(uintptr_t)bindings[j].name)) {
        prepared->error = CALL_BAD_REQUEST;
        break;
      }
    }
  }
  return bindings;
}

static enum call_status validate_startup_size(const struct launch_request *request)
{
  size_t metadata = sizeof(struct startup_info) +
      request->resource_count * sizeof(struct startup_binding) +
      request->root_count * sizeof(struct startup_binding) +
      request->working_directory_count * sizeof(handle_t) +
      request->environment_count * sizeof(struct startup_variable);
  const struct launch_binding *resources = (const void *)(uintptr_t)request->resources;
  for (size_t i = 0; i < request->resource_count; ++i) {
    metadata += strlen((const char *)(uintptr_t)resources[i].name) + 1;
  }
  const struct launch_binding *roots = (const void *)(uintptr_t)request->roots;
  for (size_t i = 0; i < request->root_count; ++i) {
    metadata += strlen((const char *)(uintptr_t)roots[i].name) + 1;
  }
  if (request->working_path) {
    metadata += strlen((const char *)(uintptr_t)request->working_path) + 1;
  }
  const struct startup_variable *environment = (const void *)(uintptr_t)request->environment;
  for (size_t i = 0; i < request->environment_count; ++i) {
    metadata += strlen((const char *)(uintptr_t)environment[i].name) + 1;
    metadata += strlen((const char *)(uintptr_t)environment[i].value) + 1;
  }
  size_t arguments = (request->argc + 1) * sizeof(uint64_t);
  const char *const *argv = (const void *)(uintptr_t)request->argv;
  for (size_t i = 0; i < request->argc; ++i) {
    arguments += strlen(argv[i]) + 1;
  }
  /* Child metadata and writable argv occupy separately padded page ranges. */
  metadata = (metadata + MEMORY_PAGE_SIZE - 1) & ~(size_t)(MEMORY_PAGE_SIZE - 1);
  arguments = (arguments + MEMORY_PAGE_SIZE - 1) & ~(size_t)(MEMORY_PAGE_SIZE - 1);
  return metadata <= STARTUP_MAX_SIZE && arguments <= STARTUP_MAX_SIZE - metadata ?
      CALL_OK : CALL_BAD_REQUEST;
}

static enum call_status capture_metadata(struct job_preparation *prepared,
    const struct pyxis_job_options *options)
{
  size_t depth = options->working ? options->working->context.count : 0;
  size_t environment_count = options->environment ? options->environment->count : 0;
  const struct startup_variable *environment = options->environment ?
      options->environment->variables : NULL;
  if ((options->grant_count && !options->grants) ||
      (options->resource_count && !options->resources) ||
      (options->root_count && !options->roots) ||
      (options->argc && !options->argv) ||
      (depth && !options->working->context.directories) ||
      (environment_count && !environment) ||
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
  if (grant_count > limit || depth > limit - grant_count) {
    return CALL_BAD_REQUEST;
  }
  grant_count += depth;
  if (options->namespace_handle != HANDLE_INVALID) {
    if (grant_count == limit) {
      return CALL_BAD_REQUEST;
    }
    ++grant_count;
  }
  for (size_t i = 0; i < STARTUP_STREAM_COUNT; ++i) {
    if (options->streams[i] < -1) {
      return CALL_BAD_REQUEST;
    }
    if (options->streams[i] >= 0) {
      if (grant_count == limit) {
        return CALL_BAD_REQUEST;
      }
      ++grant_count;
    }
  }
  struct launch_request *request = &prepared->request;
  prepared->grants = capture_array(prepared, NULL, grant_count,
      sizeof(*prepared->grants));
  if (prepared->error != CALL_OK) {
    return prepared->error;
  }
  if (options->grant_count) {
    memcpy(prepared->grants, options->grants,
        options->grant_count * sizeof(*prepared->grants));
  }
  request->grants = (uintptr_t)prepared->grants;
  request->grant_count = grant_count;
  request->resource_count = options->resource_count;
  request->resources = (uintptr_t)capture_bindings(prepared, options->resources,
      options->resource_count, options->grant_count, false);
  request->root_count = options->root_count;
  request->roots = (uintptr_t)capture_bindings(prepared, options->roots,
      options->root_count, options->grant_count, true);
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
  if (options->namespace_handle != HANDLE_INVALID) {
    request->namespace_grant = next + 1;
    prepared->grants[next++] = (struct launch_grant){
      options->namespace_handle, NAMESPACE_RIGHT_LOOKUP, 0,
    };
  }
  for (size_t i = 0; i < STARTUP_STREAM_COUNT; ++i) {
    if (options->streams[i] >= 0) {
      request->streams[i].grant = next++;
    }
  }
  if (options->working && options->working->path) {
    request->working_path = (uintptr_t)capture_string(prepared, options->working->path);
  }
  struct startup_variable *variables = capture_array(prepared, environment,
      environment_count, sizeof(*variables));
  request->environment = (uintptr_t)variables;
  request->environment_count = environment_count;
  for (size_t i = 0; i < environment_count && prepared->error == CALL_OK; ++i) {
    const char *name = capture_string(prepared, (const char *)(uintptr_t)variables[i].name);
    variables[i].name = (uintptr_t)name;
    variables[i].value = (uintptr_t)capture_string(prepared,
        (const char *)(uintptr_t)variables[i].value);
    if (prepared->error != CALL_OK) {
      break;
    }
    if (!*name || strchr(name, '=')) {
      prepared->error = CALL_BAD_REQUEST;
      break;
    }
    for (size_t j = 0; j < i; ++j) {
      if (!strcmp(name, (const char *)(uintptr_t)variables[j].name)) {
        prepared->error = CALL_BAD_REQUEST;
        break;
      }
    }
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
    int descriptor, enum startup_stream_index index)
{
  struct pyxis_descriptor_binding binding;
  if (pyxis_descriptor_borrow(descriptor, &binding) < 0) {
    /* The descriptor accessor exposes errno rather than native statuses. */
    return errno == EBADF ? CALL_BAD_HANDLE : CALL_IO;
  }
  bool input = index == STARTUP_STDIN;
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
  } else if (binding.info.protocol == PROTOCOL_PIPE && binding.info.kind == HANDLE_KIND_NATIVE) {
    rights = input ? PIPE_RIGHT_READ : PIPE_RIGHT_WRITE;
  } else if (binding.info.protocol == PROTOCOL_CONSOLE && binding.info.kind == HANDLE_KIND_NATIVE) {
    rights = input ? CONSOLE_RIGHT_READ : CONSOLE_RIGHT_WRITE;
  } else {
    return CALL_WRONG_TYPE;
  }
  struct launch_stream *stream = &prepared->request.streams[index];
  stream->protocol = binding.info.protocol;
  prepared->grants[stream->grant] = (struct launch_grant){binding.handle, rights, transport};
  return CALL_OK;
}

static enum call_status capture_handles(struct job_preparation *prepared,
    handle_t launcher, const struct pyxis_job_options *options)
{
  struct handle_info info;
  enum call_status status = handle_query(launcher, &info);
  if (status != CALL_OK) {
    return status;
  }
  if (info.protocol != PROTOCOL_LAUNCHER || info.kind != HANDLE_KIND_NATIVE || info.transport) {
    return CALL_WRONG_TYPE;
  }
  status = handle_query(options->image, &info);
  if (status != CALL_OK) {
    return status;
  }
  if (info.protocol != PROTOCOL_FILE || info.kind != HANDLE_KIND_NATIVE || info.transport) {
    return CALL_WRONG_TYPE;
  }
  struct launch_binding *roots = (void *)(uintptr_t)prepared->request.roots;
  for (size_t i = 0; i < prepared->request.root_count; ++i) {
    status = validate_directory(prepared->grants[roots[i].grant].source, &info);
    if (status != CALL_OK) {
      return status;
    }
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
  if (prepared->request.namespace_grant) {
    status = handle_query(prepared->grants[prepared->request.namespace_grant - 1].source, &info);
    if (status != CALL_OK) {
      return status;
    }
    if (info.protocol != PROTOCOL_NAMESPACE || info.kind != HANDLE_KIND_NATIVE || info.transport) {
      return CALL_WRONG_TYPE;
    }
  }
  for (size_t i = 0; i < STARTUP_STREAM_COUNT; ++i) {
    if (options->streams[i] >= 0) {
      status = capture_stream(prepared, options->streams[i], i);
      if (status != CALL_OK) {
        return status;
      }
    }
  }
  status = libc_job_validate_grants(prepared->grants, prepared->request.grant_count);
  if (status != CALL_OK) {
    return status;
  }
  status = handle_copy_restricted(launcher, LAUNCHER_RIGHT_LAUNCH, 0, &prepared->launcher);
  if (status != CALL_OK) {
    return status;
  }
  status = handle_copy_restricted(options->image, FILE_RIGHT_READ, 0, &prepared->image);
  if (status != CALL_OK) {
    return status;
  }
  prepared->request.image = prepared->image;
  for (size_t i = 0; i < prepared->request.grant_count; ++i) {
    struct launch_grant *grant = &prepared->grants[i];
    handle_t copy;
    status = handle_copy_restricted(grant->source, grant->rights, grant->transport, &copy);
    if (status != CALL_OK) {
      return status;
    }
    grant->source = copy;
    ++prepared->copied_grants;
  }
  return CALL_OK;
}

static void discard_preparation(struct job_preparation *prepared)
{
  for (size_t i = 0; i < prepared->copied_grants; ++i) {
    handle_close(prepared->grants[i].source);
  }
  if (prepared->image != HANDLE_INVALID) {
    handle_close(prepared->image);
  }
  if (prepared->launcher != HANDLE_INVALID) {
    handle_close(prepared->launcher);
  }
  free(prepared->metadata);
}

enum call_status libc_job_launch(handle_t launcher,
    const struct pyxis_job_options *options, handle_t *observer)
{
  *observer = HANDLE_INVALID;
  struct pyxis_job_options selected = *options;
  struct job_preparation prepared = {0};
  prepared.metadata = malloc(LAUNCH_CAPTURE_MAX_SIZE);
  if (!prepared.metadata) {
    return CALL_NO_MEMORY;
  }
  enum call_status status = capture_metadata(&prepared, &selected);
  if (status == CALL_OK) {
    status = capture_handles(&prepared, launcher, &selected);
  }
  if (status == CALL_OK) {
    for (size_t i = STARTUP_STDOUT; i < STARTUP_STREAM_COUNT; ++i) {
      if (selected.streams[i] >= 0 && libc_stdio_flush_descriptor(selected.streams[i]) < 0) {
        status = CALL_IO;
        break;
      }
    }
  }
  if (status == CALL_OK) {
    status = launcher_launch(prepared.launcher, &prepared.request, observer);
  }
  int saved_errno = errno;
  discard_preparation(&prepared);
  errno = saved_errno;
  return status;
}
