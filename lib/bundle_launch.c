#include <abi/directory.h>
#include <abi/clock.h>
#include <abi/launcher.h>
#include <abi/pipe.h>
#include <job_work.h>
#include <process.h>
#include <wait.h>
#include <bundle_launch.h>
#include <handle.h>
#include <stdlib.h>
#include <string.h>

struct bundle_job_context {
  struct bundle_job_context *next;
  struct job_work *work;
  handle_t observer;
  bool admitted;
};

struct bundle_launch {
  struct bundle_jobs *jobs;
  struct bundle_job_context *context;
  struct launch_request request;
  struct launch_grant *grants;
  struct launch_binding *resources;
  struct launch_binding *roots;
  uint64_t *directories;
};

struct context_grants {
  const struct launch_grant *source;
  size_t source_count;
  uint64_t *mapped;
  struct bundle_launch *prepared;
};

static enum call_status retain_grant(struct context_grants *context,
    uint64_t source_index, uint64_t *index)
{
  if (source_index >= context->source_count ||
      context->source[source_index].source == HANDLE_INVALID) {
    return CALL_BAD_REQUEST;
  }
  if (context->mapped[source_index] == UINT64_MAX) {
    size_t next = context->prepared->request.grant_count++;
    context->prepared->grants[next] = context->source[source_index];
    context->mapped[source_index] = next;
  }
  *index = context->mapped[source_index];
  return CALL_OK;
}

static enum call_status prepare_context(struct context_grants *context,
    const struct launch_request *source, const struct startup_binding *bundle_roots,
    size_t bundle_root_count)
{
  struct bundle_launch *prepared = context->prepared;
  const struct launch_binding *roots = (const void *)(uintptr_t)source->roots;
  for (size_t i = 0; i < source->root_count; ++i) {
    const char *name = (const void *)(uintptr_t)roots[i].name;
    if (!name || !*name) {
      return CALL_BAD_REQUEST;
    }
    if (!strcmp(name, "app")) {
      continue;
    }
    for (size_t j = 0; j < bundle_root_count; ++j) {
      if (!strcmp(name, (const void *)(uintptr_t)bundle_roots[j].name)) {
        return CALL_BAD_REQUEST;
      }
    }
    if (prepared->request.root_count == STARTUP_ROOT_LIMIT) {
      return CALL_LIMIT;
    }
    struct launch_binding *binding = &prepared->roots[prepared->request.root_count];
    binding->name = roots[i].name;
    enum call_status status = retain_grant(context, roots[i].grant, &binding->grant);
    if (status != CALL_OK) {
      return status;
    }
    ++prepared->request.root_count;
  }

  const uint64_t *directories = (const void *)(uintptr_t)source->working_directories;
  for (size_t i = 0; i < source->working_directory_count; ++i) {
    enum call_status status = retain_grant(context, directories[i], &prepared->directories[i]);
    if (status != CALL_OK) {
      return status;
    }
  }
  for (size_t i = 0; i < STARTUP_STREAM_COUNT; ++i) {
    if (source->streams[i].protocol == STARTUP_STREAM_NONE) {
      if (source->streams[i].grant || source->streams[i].observation_protocol ||
          source->streams[i].observation_grant) {
        return CALL_BAD_REQUEST;
      }
      continue;
    }
    enum call_status status = retain_grant(context, source->streams[i].grant,
        &prepared->request.streams[i].grant);
    if (status != CALL_OK) {
      return status;
    }
    if (source->streams[i].observation_protocol) {
      status = retain_grant(context, source->streams[i].observation_grant,
          &prepared->request.streams[i].observation_grant);
      if (status != CALL_OK) {
        return status;
      }
    } else if (source->streams[i].observation_grant) {
      return CALL_BAD_REQUEST;
    }
  }
  if (source->namespace_grant) {
    uint64_t index;
    enum call_status status = retain_grant(context, source->namespace_grant - 1, &index);
    if (status != CALL_OK) {
      return status;
    }
    prepared->request.namespace_grant = index + 1;
  }
  return CALL_OK;
}

static enum call_status prepare_resources(struct bundle_launch *prepared,
    const struct bundle_grant_request *requests, size_t count,
    const struct bundle_authority *authority, size_t authority_count)
{
  for (size_t i = 0; i < count; ++i) {
    /* Private context authority is created separately, never copied from a
     * caller's client leaf, including the ordinary Lua/bundle path. */
    if (!strcmp(requests[i].resource, "job_work")) {
      continue;
    }
    handle_t source = HANDLE_INVALID;
    for (size_t j = 0; j < authority_count; ++j) {
      if (authority[j].resource && !strcmp(requests[i].resource, authority[j].resource)) {
        source = authority[j].source;
        break;
      }
    }
    if (source == HANDLE_INVALID) {
      if (requests[i].required) {
        return CALL_DENIED;
      }
      continue;
    }
    struct handle_info info;
    enum call_status status = handle_query(source, &info);
    if (status != CALL_OK) {
      if (requests[i].required) {
        return status;
      }
      continue;
    }
    if (info.protocol != requests[i].protocol ||
        (info.kind != HANDLE_KIND_NATIVE && info.kind != HANDLE_KIND_EXPORTED)) {
      if (requests[i].required) {
        return CALL_WRONG_TYPE;
      }
      continue;
    }
    uint64_t transport = info.kind == HANDLE_KIND_EXPORTED ? HANDLE_TRANSPORT_CALL : 0;
    if ((requests[i].rights & info.rights) != requests[i].rights ||
        (transport & info.transport) != transport) {
      if (requests[i].required) {
        return CALL_DENIED;
      }
      continue;
    }
    size_t index = prepared->request.grant_count++;
    prepared->grants[index] = (struct launch_grant){source, requests[i].rights, transport};
    prepared->resources[prepared->request.resource_count++] =
        (struct launch_binding){(uintptr_t)requests[i].name, index};
  }
  return CALL_OK;
}

static bool has_selected(const struct bundle_launch *prepared,
    uint64_t protocol, uint64_t rights, handle_t *source)
{
  for (size_t i = 0; i < prepared->request.resource_count; ++i) {
    const struct launch_grant *grant = &prepared->grants[prepared->resources[i].grant];
    struct handle_info info;
    if (handle_query(grant->source, &info) == CALL_OK &&
        info.kind == HANDLE_KIND_NATIVE && info.protocol == protocol &&
        (grant->rights & rights) == rights && !grant->transport) {
      if (source) {
        *source = grant->source;
      }
      return true;
    }
  }
  return false;
}

static enum call_status prepare_job_work(struct bundle_launch *prepared,
    const struct bundle_grant_request *requests, size_t count,
    handle_t provisioner, struct bundle_jobs *jobs)
{
  bool declared = false, required = false;
  for (size_t i = 0; i < count; ++i) {
    if (!strcmp(requests[i].resource, "job_work")) {
      declared = true;
      required |= requests[i].required;
    }
  }
  if (!declared) {
    return CALL_OK;
  }
  handle_t clock = HANDLE_INVALID;
  bool usable = jobs && provisioner != HANDLE_INVALID &&
      has_selected(prepared, PROTOCOL_CLOCK, CLOCK_RIGHT_READ, &clock) &&
      has_selected(prepared, PROTOCOL_PIPE_SERVICE, PIPE_SERVICE_RIGHT_CREATE, NULL) &&
      has_selected(prepared, PROTOCOL_LAUNCHER, LAUNCHER_RIGHT_LAUNCH, NULL);
  if (!usable) {
    return required ? CALL_DENIED : CALL_OK;
  }
  struct bundle_job_context *context = calloc(1, sizeof(*context));
  if (!context) {
    return CALL_NO_MEMORY;
  }
  enum call_status status = job_work_create(provisioner, clock, &context->work);
  if (status != CALL_OK) {
    free(context);
    return required ? status : CALL_OK;
  }
  prepared->context = context;
  prepared->jobs = jobs;
  for (size_t i = 0; i < count; ++i) {
    if (!strcmp(requests[i].resource, "job_work")) {
      size_t index = prepared->request.grant_count++;
      prepared->grants[index] = (struct launch_grant){
        job_work_directory(context->work), requests[i].rights, 0,
      };
      prepared->resources[prepared->request.resource_count++] =
          (struct launch_binding){(uintptr_t)requests[i].name, index};
    }
  }
  return CALL_OK;
}

static enum call_status prepare_bundle(const struct bundle_program *program,
    const struct launch_request *source, const struct bundle_authority *authority,
    size_t authority_count, handle_t provisioner, struct bundle_jobs *jobs,
    struct bundle_launch **result)
{
  if (!result) {
    return CALL_BAD_REQUEST;
  }
  *result = NULL;
  if (!program || !source || (authority_count && !authority) ||
      (source->grant_count && !source->grants) ||
      (source->root_count && !source->roots) ||
      (source->working_directory_count && !source->working_directories)) {
    return CALL_BAD_REQUEST;
  }
  size_t root_count, resource_count;
  const struct startup_binding *roots = bundle_program_roots(program, &root_count);
  const struct bundle_grant_request *resources = bundle_program_grants(program, &resource_count);
  const size_t grant_limit = LAUNCH_CAPTURE_MAX_SIZE / sizeof(struct launch_grant);
  if (source->grant_count > grant_limit || root_count > grant_limit ||
      resource_count > grant_limit - root_count ||
      source->grant_count > grant_limit - root_count - resource_count ||
      source->root_count > STARTUP_ROOT_LIMIT ||
      source->working_directory_count > LAUNCH_CAPTURE_MAX_SIZE / sizeof(uint64_t)) {
    return CALL_LIMIT;
  }
  size_t grant_capacity = source->grant_count + root_count + resource_count;
  struct bundle_launch *prepared = calloc(1, sizeof(*prepared));
  uint64_t *mapped = source->grant_count ? malloc(source->grant_count * sizeof(*mapped)) : NULL;
  if (!prepared || (source->grant_count && !mapped)) {
    free(prepared);
    free(mapped);
    return CALL_NO_MEMORY;
  }
  prepared->grants = calloc(grant_capacity, sizeof(*prepared->grants));
  prepared->resources = resource_count ? calloc(resource_count, sizeof(*prepared->resources)) : NULL;
  prepared->roots = calloc(STARTUP_ROOT_LIMIT, sizeof(*prepared->roots));
  prepared->directories = source->working_directory_count ?
      malloc(source->working_directory_count * sizeof(*prepared->directories)) : NULL;
  if (!prepared->grants || (resource_count && !prepared->resources) || !prepared->roots ||
      (source->working_directory_count && !prepared->directories)) {
    free(mapped);
    bundle_launch_close(prepared);
    return CALL_NO_MEMORY;
  }
  for (size_t i = 0; i < source->grant_count; ++i) {
    mapped[i] = UINT64_MAX;
  }
  prepared->request = *source;
  prepared->request.image = bundle_program_image(program);
  prepared->request.initial_stack_bytes = bundle_program_stack_bytes(program);
  prepared->request.grants = (uintptr_t)prepared->grants;
  prepared->request.grant_count = 0;
  prepared->request.resources = (uintptr_t)prepared->resources;
  prepared->request.resource_count = 0;
  prepared->request.roots = (uintptr_t)prepared->roots;
  prepared->request.root_count = 0;
  prepared->request.working_directories = (uintptr_t)prepared->directories;
  prepared->request.namespace_grant = 0;
  struct context_grants context = {
    .source = (const void *)(uintptr_t)source->grants,
    .source_count = source->grant_count, .mapped = mapped, .prepared = prepared,
  };
  enum call_status status = prepare_context(&context, source, roots, root_count);
  if (status == CALL_OK) {
    status = prepare_resources(prepared, resources, resource_count, authority, authority_count);
  }
  if (status == CALL_OK) {
    status = prepare_job_work(prepared, resources, resource_count, provisioner, jobs);
  }
  for (size_t i = 0; status == CALL_OK && i < root_count; ++i) {
    if (prepared->request.root_count == STARTUP_ROOT_LIMIT) {
      status = CALL_LIMIT;
      break;
    }
    size_t index = prepared->request.grant_count++;
    prepared->grants[index] = (struct launch_grant){roots[i].handle,
        DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_ENUMERATE | DIRECTORY_RIGHT_READ_FILES, 0};
    prepared->roots[prepared->request.root_count++] =
        (struct launch_binding){roots[i].name, index};
  }
  free(mapped);
  if (status != CALL_OK) {
    bundle_launch_close(prepared);
    return status;
  }
  *result = prepared;
  return CALL_OK;
}

enum call_status bundle_launch_prepare(const struct bundle_program *program,
    const struct launch_request *source, const struct bundle_authority *authority,
    size_t authority_count, struct bundle_launch **result)
{
  return prepare_bundle(program, source, authority, authority_count,
      HANDLE_INVALID, NULL, result);
}

enum call_status bundle_launch_prepare_jobs(const struct bundle_program *program,
    const struct launch_request *source, const struct bundle_authority *authority,
    size_t authority_count, handle_t provisioner, struct bundle_jobs *jobs,
    struct bundle_launch **result)
{
  return prepare_bundle(program, source, authority, authority_count, provisioner, jobs, result);
}

void bundle_launch_admitted(struct bundle_launch *prepared, enum call_status status,
    handle_t process)
{
  if (!prepared || !prepared->context || (status != CALL_OK && status != CALL_OUTCOME_UNKNOWN)) {
    return;
  }
  struct bundle_job_context *context = prepared->context;
  context->admitted = true;
  /* Failure to retain the observer leaves the private link quarantined. */
  if (status == CALL_OK && process != HANDLE_INVALID) {
    handle_copy_restricted(process, PROCESS_RIGHT_WAIT, 0, &context->observer);
  }
}

enum call_status bundle_jobs_reap(struct bundle_jobs *jobs)
{
  if (!jobs) {
    return CALL_BAD_REQUEST;
  }
  enum call_status result = CALL_OK;
  struct bundle_job_context **link = &jobs->contexts;
  while (*link) {
    struct bundle_job_context *context = *link;
    if (context->observer == HANDLE_INVALID) {
      link = &context->next;
      continue;
    }
    struct wait_interest interest = {.handle = context->observer, .events = WAIT_COMPLETE};
    uint64_t events;
    enum call_status status = wait_many(&interest, 1, 0, &events);
    if (status != CALL_OK || !(events & WAIT_COMPLETE)) {
      if (status != CALL_OK && result == CALL_OK) {
        result = status;
      }
      link = &context->next;
      continue;
    }
    struct process_result completion;
    status = process_wait(context->observer, &completion);
    if (status != CALL_OK) {
      if (result == CALL_OK) {
        result = status;
      }
      link = &context->next;
      continue;
    }
    *link = context->next;
    status = job_work_close(context->work, true);
    if (handle_close(context->observer) && status == CALL_OK) {
      status = CALL_BAD_HANDLE;
    }
    if (status != CALL_OK && result == CALL_OK) {
      result = status;
    }
    free(context);
  }
  return result;
}

enum call_status bundle_jobs_close(struct bundle_jobs *jobs)
{
  enum call_status result = bundle_jobs_reap(jobs);
  if (!jobs) {
    return result;
  }
  while (jobs->contexts) {
    struct bundle_job_context *context = jobs->contexts;
    jobs->contexts = context->next;
    enum call_status status = job_work_close(context->work, false);
    if (context->observer != HANDLE_INVALID && handle_close(context->observer) &&
        status == CALL_OK) {
      status = CALL_BAD_HANDLE;
    }
    if (status != CALL_OK && result == CALL_OK) {
      result = status;
    }
    free(context);
  }
  return result;
}

const struct launch_request *bundle_launch_request(const struct bundle_launch *prepared)
{
  return prepared ? &prepared->request : NULL;
}

void bundle_launch_close(struct bundle_launch *prepared)
{
  if (prepared) {
    if (prepared->context) {
      if (prepared->context->admitted) {
        prepared->context->next = prepared->jobs->contexts;
        prepared->jobs->contexts = prepared->context;
      } else {
        job_work_close(prepared->context->work, true);
        free(prepared->context);
      }
    }
    free(prepared->directories);
    free(prepared->roots);
    free(prepared->resources);
    free(prepared->grants);
    free(prepared);
  }
}

