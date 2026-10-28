#include <program_sources.h>
#include <handle.h>
#include <stdlib.h>
#include <string.h>

/* Runtime roles not themselves manifest resource classes. This only names
 * held sources; it grants no rights beyond the queried capability. */
static uint64_t source_protocol(const char *name)
{
  static const struct {
    const char *name;
    uint64_t protocol;
  } aliases[] = {
    {"child_launcher", PROTOCOL_LAUNCHER},
    {"interpreter_root", PROTOCOL_DIRECTORY},
    {"interpreter_image", PROTOCOL_FILE},
    {"power", PROTOCOL_POWER},
    {"host_mount", PROTOCOL_MOUNT},
    {"native_mount", PROTOCOL_MOUNT},
    {"space", PROTOCOL_SPACE},
    {BUNDLE_TCP_LISTEN_SOURCE, PROTOCOL_TCP_SERVICE},
    {"log", PROTOCOL_LOG},
  };
  /* These resource classes are per-command ownership, not launch sources.
   * Controllers and observers have no canonical resource name or alias here. */
  if (!strcmp(name, "publication") || !strcmp(name, "job_work") ||
      !strcmp(name, "job_provisioner")) {
    return 0;
  }
  uint64_t protocol = bundle_resource_protocol(name);
  if (protocol) {
    return protocol;
  }
  for (size_t i = 0; i < sizeof(aliases) / sizeof(*aliases); ++i) {
    if (!strcmp(name, aliases[i].name)) {
      return aliases[i].protocol;
    }
  }
  return 0;
}

enum call_status program_sources_from_capture(const struct environment_capture *capture,
    struct bundle_authority *authority, size_t capacity, size_t *count)
{
  if (!count) {
    return CALL_BAD_REQUEST;
  }
  *count = 0;
  if (!capture || (capacity && !authority)) {
    return CALL_BAD_REQUEST;
  }
  size_t ordinary = 0;
  for (size_t i = 0; i < capture->count; ++i) {
    const struct environment_value *value = &capture->values[i];
    uint64_t protocol = source_protocol(value->name);
    if (!protocol || value->kind != ENVIRONMENT_CAP) {
      continue;
    }
    struct handle_info info;
    enum call_status status = handle_query(value->handle, &info);
    if (status != CALL_OK) {
      return status;
    }
    if (info.protocol != protocol) {
      return CALL_WRONG_TYPE;
    }
    if (ordinary == capacity) {
      return CALL_LIMIT;
    }
    authority[ordinary++] = (struct bundle_authority){value->name, value->handle, protocol};
  }
  size_t configured = 0;
  enum call_status status = bundle_sources_from_capture(capture,
      authority ? authority + ordinary : NULL, capacity - ordinary, &configured);
  if (status == CALL_OK) {
    *count = ordinary + configured;
  }
  return status;
}

void program_sources_close(struct program_sources *sources)
{
  if (!sources) {
    return;
  }
  free(sources->authority);
  environment_capture_close(&sources->capture);
  *sources = (struct program_sources){0};
}

enum call_status program_sources_capture(handle_t environment, struct program_sources *sources)
{
  if (!sources) {
    return CALL_BAD_REQUEST;
  }
  *sources = (struct program_sources){0};
  struct environment_capture original = {0};
  enum call_status status = environment_capture_init(environment,
      ENVIRONMENT_SNAPSHOT_INCLUDE_PRIVATE, &original);
  if (status != CALL_OK) {
    return status;
  }
  if (original.count > SIZE_MAX / sizeof(*sources->authority)) {
    status = CALL_LIMIT;
    goto done;
  }
  sources->authority = original.count ? calloc(original.count, sizeof(*sources->authority)) : NULL;
  if (original.count && !sources->authority) {
    status = CALL_NO_MEMORY;
    goto done;
  }
  status = program_sources_from_capture(&original, sources->authority,
      original.count, &sources->count);
  if (status != CALL_OK) {
    goto done;
  }
  sources->capture.values = sources->count ? calloc(sources->count,
      sizeof(*sources->capture.values)) : NULL;
  if (sources->count && !sources->capture.values) {
    status = CALL_NO_MEMORY;
    goto done;
  }
  sources->capture.generation = original.generation;
  for (size_t i = 0; i < original.count; ++i) {
    struct environment_value *value = &original.values[i];
    bool selected = false;
    for (size_t j = 0; j < sources->count; ++j) {
      const char *name = sources->authority[j].resource;
      if (name == value->name ||
          (!strncmp(value->name, BUNDLE_SOURCE_PREFIX, sizeof(BUNDLE_SOURCE_PREFIX) - 1) &&
           name == value->name + sizeof(BUNDLE_SOURCE_PREFIX) - 1)) {
        selected = true;
        break;
      }
    }
    if (selected) {
      sources->capture.values[sources->capture.count++] = *value;
      *value = (struct environment_value){0};
    }
  }
  /* Release all unrelated CAPs and TEXT before retaining the filtered owner. */
  environment_capture_close(&original);
  status = program_sources_from_capture(&sources->capture, sources->authority,
      sources->capture.count, &sources->count);
  if (status == CALL_OK) {
    for (size_t i = 0; i < sources->capture.count; ++i) {
      const struct environment_value *value = &sources->capture.values[i];
      if (value->kind == ENVIRONMENT_CAP && source_protocol(value->name)) {
        ++sources->ordinary_count;
      }
    }
  }
done:
  environment_capture_close(&original);
  if (status != CALL_OK) {
    program_sources_close(sources);
  }
  return status;
}

enum call_status program_sources_stage(const struct bundle_authority *authority, size_t count,
    struct launch_request *request, struct launch_grant *grants, size_t grant_capacity,
    struct launch_environment_entry *entries, size_t entry_capacity)
{
  if (!request || (count && !authority) || (grant_capacity && !grants) ||
      (entry_capacity && !entries) || request->grant_count > grant_capacity ||
      request->entry_count > entry_capacity || request->grants != (uintptr_t)grants ||
      request->entries != (uintptr_t)entries) {
    return CALL_BAD_REQUEST;
  }
  for (size_t i = 0; i < count; ++i) {
    const struct bundle_authority *source = &authority[i];
    if (source->source == HANDLE_INVALID || !source->resource ||
        !source_protocol(source->resource)) {
      continue;
    }
    struct handle_info info;
    enum call_status status = handle_query(source->source, &info);
    if (status != CALL_OK) {
      return status;
    }
    if (info.protocol != source->protocol || info.protocol != source_protocol(source->resource)) {
      return CALL_WRONG_TYPE;
    }
    size_t entry = 0;
    while (entry < request->entry_count && strcmp(source->resource,
        (const char *)(uintptr_t)entries[entry].name)) {
      ++entry;
    }
    size_t grant;
    if (entry < request->entry_count) {
      if (entries[entry].kind != ENVIRONMENT_CAP) {
        return CALL_ALREADY_EXISTS;
      }
      grant = entries[entry].value;
      if (grant >= request->grant_count) {
        return CALL_BAD_REQUEST;
      }
    } else {
      if (request->entry_count == entry_capacity || request->grant_count == grant_capacity) {
        return CALL_LIMIT;
      }
      grant = request->grant_count++;
      entries[request->entry_count++] = (struct launch_environment_entry){
        .name = (uintptr_t)source->resource, .kind = ENVIRONMENT_CAP, .value = grant,
      };
    }
    grants[grant] = (struct launch_grant){source->source, info.rights, info.transport};
  }
  return CALL_OK;
}
