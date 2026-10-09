#include <abi/directory.h>
#include <bundle_launch.h>
#include <handle.h>
#include <stdlib.h>
#include <string.h>

struct bundle_launch {
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
      if (source->streams[i].grant) {
        return CALL_BAD_REQUEST;
      }
      continue;
    }
    enum call_status status = retain_grant(context, source->streams[i].grant,
        &prepared->request.streams[i].grant);
    if (status != CALL_OK) {
      return status;
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

enum call_status bundle_launch_prepare(const struct bundle_program *program,
    const struct launch_request *source, const struct bundle_authority *authority,
    size_t authority_count, struct bundle_launch **result)
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

const struct launch_request *bundle_launch_request(const struct bundle_launch *prepared)
{
  return prepared ? &prepared->request : NULL;
}

void bundle_launch_close(struct bundle_launch *prepared)
{
  if (prepared) {
    free(prepared->directories);
    free(prepared->roots);
    free(prepared->resources);
    free(prepared->grants);
    free(prepared);
  }
}

