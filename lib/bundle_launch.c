#include <abi/directory.h>
#include <abi/file.h>
#include <abi/clock.h>
#include <abi/launcher.h>
#include <abi/pipe.h>
#include <abi/disk.h>
#include <abi/mount.h>
#include <abi/provider.h>
#include <job_work.h>
#include <process.h>
#include <wait.h>
#include <bundle_launch.h>
#include <command_search.h>
#include <package.h>
#include <package_commands.h>
#include <handle.h>
#include <net_config.h>
#include <tcp.h>
#include <udp.h>
#include <echo.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

struct bundle_job_context {
  struct bundle_job_context *next;
  struct job_work *work;
  handle_t observer;
  bool admitted, delegated;
};

struct bundle_launch {
  struct bundle_jobs *jobs;
  struct bundle_job_context *context;
  struct launch_request request;
  struct launch_grant *grants;
  struct launch_environment_entry *entries;
  uint64_t *directories;
  /* At most one temporary per manifest request: the packages selection or a
   * narrowed network factory. The launch borrows each until preparation closes. */
  handle_t *owned_grants;
  size_t owned_count;
  /* The packages source bound to a package-selected command's capture, and
   * the launch's one capture of the set. */
  handle_t package_root, package_capture;
};

struct context_grants {
  const struct launch_grant *source;
  size_t source_count;
  uint64_t *mapped;
  struct bundle_launch *prepared;
};

static enum call_status configured_source(const struct environment_value *binding,
    const char **name, struct handle_info *info)
{
  const char *binding_name = binding->name;
  size_t prefix_size = sizeof(BUNDLE_SOURCE_PREFIX) - 1;
  if (binding->kind != ENVIRONMENT_CAP ||
      strncmp(binding_name, BUNDLE_SOURCE_PREFIX, prefix_size)) {
    return CALL_NOT_FOUND;
  }
  *name = binding_name + prefix_size;
  if (!**name) {
    return CALL_BAD_REQUEST;
  }
  enum call_status status = handle_query(binding->handle, info);
  if (status != CALL_OK) {
    return status == CALL_BAD_HANDLE ? CALL_NOT_FOUND : status;
  }
  if (info->kind != HANDLE_KIND_NATIVE && info->kind != HANDLE_KIND_EXPORTED) {
    return CALL_WRONG_TYPE;
  }
  if (info->protocol == PROTOCOL_DIRECTORY) {
    return CALL_OK;
  }
  if (info->kind != HANDLE_KIND_NATIVE) {
    return CALL_WRONG_TYPE;
  }
  if (info->protocol == PROTOCOL_MOUNT) {
    return CALL_OK;
  }
  if (info->protocol == PROTOCOL_DISKS) {
    uint64_t allowed = DISKS_RIGHT_ENUMERATE | DISKS_RIGHT_MOUNT_READ_ONLY |
        DISKS_RIGHT_MOUNT_READ_WRITE;
    return (info->rights & ~allowed) ? CALL_DENIED : CALL_OK;
  }
  if (info->protocol == PROTOCOL_FILE) {
    return (info->rights & ~FILE_RIGHT_READ) || info->transport ? CALL_DENIED : CALL_OK;
  }
  return CALL_WRONG_TYPE;
}

enum call_status bundle_sources_from_capture(const struct environment_capture *capture,
    struct bundle_authority *authority, size_t capacity, size_t *count)
{
  if (!capture || !count || (capacity && !authority)) {
    return CALL_BAD_REQUEST;
  }
  *count = 0;
  for (size_t i = 0; i < capture->count; ++i) {
    const char *name;
    struct handle_info info;
    enum call_status status = configured_source(&capture->values[i], &name, &info);
    if (status == CALL_NOT_FOUND) {
      continue;
    }
    if (status != CALL_OK) {
      return status;
    }
    if (*count == capacity || *count == BUNDLE_SOURCE_LIMIT) {
      return CALL_LIMIT;
    }
    authority[(*count)++] = (struct bundle_authority){name, capture->values[i].handle, info.protocol};
  }
  return CALL_OK;
}

enum call_status bundle_sources_forward_capture(const struct environment_capture *capture,
    struct launch_grant *grants, size_t grant_capacity, size_t *grant_count,
    struct launch_environment_entry *entries, size_t entry_capacity, size_t *entry_count)
{
  if (!capture || !grant_count || !entry_count || *grant_count > grant_capacity ||
      *entry_count > entry_capacity || (grant_capacity && !grants) ||
      (entry_capacity && !entries)) {
    return CALL_BAD_REQUEST;
  }
  size_t source_count = 0;
  for (size_t i = 0; i < capture->count; ++i) {
    const struct environment_value *binding = &capture->values[i];
    const char *name;
    struct handle_info info;
    enum call_status status = configured_source(binding, &name, &info);
    if (status == CALL_NOT_FOUND) {
      continue;
    }
    if (status != CALL_OK) {
      return status;
    }
    if (source_count == BUNDLE_SOURCE_LIMIT || *grant_count == grant_capacity ||
        *entry_count == entry_capacity) {
      return CALL_LIMIT;
    }
    for (size_t j = 0; j < *entry_count; ++j) {
      if (!strcmp(binding->name, (const char *)(uintptr_t)entries[j].name)) {
        return CALL_BAD_REQUEST;
      }
    }
    entries[(*entry_count)++] = (struct launch_environment_entry){
      .name = (uintptr_t)binding->name, .kind = ENVIRONMENT_CAP, .value = *grant_count,
    };
    grants[(*grant_count)++] = (struct launch_grant){binding->handle, info.rights, info.transport};
    ++source_count;
  }
  return CALL_OK;
}

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

static bool has_entry(const struct bundle_launch *prepared, const char *name)
{
  for (size_t i = 0; i < prepared->request.entry_count; ++i) {
    if (!strcmp(name, (const char *)(uintptr_t)prepared->entries[i].name)) {
      return true;
    }
  }
  return false;
}

static enum call_status prepare_context(struct context_grants *context,
    const struct launch_request *source, const struct environment_value *bundle_entries,
    size_t bundle_entry_count, bool explicit_providers)
{
  struct bundle_launch *prepared = context->prepared;
  const struct launch_environment_entry *entries = (const void *)(uintptr_t)source->entries;
  for (size_t i = 0; i < source->entry_count; ++i) {
    const struct launch_environment_entry *entry = &entries[i];
    const char *name = (const char *)(uintptr_t)entry->name;
    if (!name || !*name) {
      return CALL_BAD_REQUEST;
    }
    /* A bundle reads packages only through its manifest's "packages" and
     * "sdk", so the space's pkg and sdk roots are withheld like the packages
     * source (decision 38 of docs/wip/sdk-view.md). */
    if (!strcmp(name, "app") || !strcmp(name, "job_work") ||
        !strcmp(name, "job_provisioner") || !strcmp(name, BUNDLE_TCP_LISTEN_SOURCE) ||
        !strcmp(name, BUNDLE_PACKAGES_ROOT) ||
        !strcmp(name, BUNDLE_SDK_ROOT) ||
        !strncmp(name, BUNDLE_SOURCE_PREFIX, sizeof(BUNDLE_SOURCE_PREFIX) - 1)) {
      continue;
    }
    struct launch_environment_entry binding = *entry;
    bool commands = !strcmp(name, "commands");
    if (commands && entry->kind != ENVIRONMENT_CAP) {
      return CALL_WRONG_TYPE;
    }
    if (entry->kind == ENVIRONMENT_CAP) {
      if (entry->value >= context->source_count) {
        return CALL_BAD_REQUEST;
      }
      struct handle_info info;
      enum call_status status = handle_query(context->source[entry->value].source, &info);
      if (status != CALL_OK) {
        return status;
      }
      if (commands) {
        status = command_list_validate(context->source[entry->value].source);
        if (status != CALL_OK) {
          return status;
        }
        if (!(context->source[entry->value].rights & COMMAND_LIST_RIGHT_READ) ||
            context->source[entry->value].transport) {
          return CALL_DENIED;
        }
        if (!(entry->flags & ENVIRONMENT_EXPORTED)) {
          continue;
        }
      }
      /* Filter by actual protocol, so another name for the same provider
       * cannot retain the original, broader grant. */
      if (explicit_providers && info.protocol == PROTOCOL_PROVIDER) {
        continue;
      }
      bool stream = !strcmp(name, "stdin") ||
          !strcmp(name, "stdout") || !strcmp(name, "stderr");
      bool directory = info.protocol == PROTOCOL_DIRECTORY && (entry->flags & ENVIRONMENT_EXPORTED);
      bool provider = info.kind == HANDLE_KIND_EXPORTED && (entry->flags & ENVIRONMENT_EXPORTED);
      if (!stream && !directory && !provider && !commands) {
        continue;
      }
      status = retain_grant(context, entry->value, &binding.value);
      if (status != CALL_OK) {
        return status;
      }
    } else if (entry->kind != ENVIRONMENT_TEXT) {
      return CALL_BAD_REQUEST;
    }
    for (size_t j = 0; j < bundle_entry_count; ++j) {
      if (!strcmp(name, bundle_entries[j].name)) {
        return CALL_BAD_REQUEST;
      }
    }
    if (has_entry(prepared, name)) {
      return CALL_BAD_REQUEST;
    }
    prepared->entries[prepared->request.entry_count++] = binding;
  }
  const uint64_t *directories = (const void *)(uintptr_t)source->working_directories;
  for (size_t i = 0; i < source->working_directory_count; ++i) {
    if (explicit_providers) {
      if (directories[i] >= context->source_count) return CALL_BAD_REQUEST;
      struct handle_info info;
      enum call_status status = handle_query(context->source[directories[i]].source, &info);
      if (status != CALL_OK) return status;
      if (info.protocol == PROTOCOL_PROVIDER) return CALL_WRONG_TYPE;
    }
    enum call_status status = retain_grant(context, directories[i], &prepared->directories[i]);
    if (status != CALL_OK) {
      return status;
    }
  }
  return CALL_OK;
}

static enum call_status provider_source(const struct launch_request *source,
    const struct bundle_grant_request *request, handle_t *handle)
{
  *handle = HANDLE_INVALID;
  const struct launch_environment_entry *entries = (const void *)(uintptr_t)source->entries;
  const struct launch_grant *grants = (const void *)(uintptr_t)source->grants;
  const struct launch_grant *selected = NULL;
  for (size_t i = 0; i < source->entry_count; ++i) {
    const struct launch_environment_entry *entry = &entries[i];
    if (strcmp((const char *)(uintptr_t)entry->name, request->source)) continue;
    if (selected) return CALL_BAD_REQUEST;
    if (entry->kind != ENVIRONMENT_CAP || !(entry->flags & ENVIRONMENT_EXPORTED)) {
      return CALL_NOT_FOUND;
    }
    if (entry->value >= source->grant_count) return CALL_BAD_REQUEST;
    selected = &grants[entry->value];
  }
  if (!selected) return CALL_NOT_FOUND;
  struct handle_info info;
  enum call_status status = handle_query(selected->source, &info);
  if (status != CALL_OK) return status;
  if (info.kind != HANDLE_KIND_EXPORTED || info.protocol != PROTOCOL_PROVIDER) {
    return CALL_WRONG_TYPE;
  }
  if ((request->rights & info.rights) != request->rights ||
      (request->rights & selected->rights) != request->rights ||
      !(info.transport & HANDLE_TRANSPORT_CALL) ||
      !(selected->transport & HANDLE_TRANSPORT_CALL)) {
    return CALL_DENIED;
  }
  *handle = selected->source;
  return CALL_OK;
}

/* The launcher's own sdk root, for a bundle launched by a bundle, which holds
 * no packages source (docs/wip/sdk-view.md, decision 59). It is passed on with
 * at most REQUEST's read-only rights, so the child reads nothing more. */
static enum call_status held_sdk(const struct launch_request *source,
    const struct bundle_grant_request *request, handle_t *handle)
{
  *handle = HANDLE_INVALID;
  const struct launch_environment_entry *entries = (const void *)(uintptr_t)source->entries;
  const struct launch_grant *grants = (const void *)(uintptr_t)source->grants;
  const struct launch_grant *selected = NULL;
  for (size_t i = 0; i < source->entry_count; ++i) {
    const struct launch_environment_entry *entry = &entries[i];
    if (strcmp((const char *)(uintptr_t)entry->name, BUNDLE_SDK_ROOT)) continue;
    if (selected) return CALL_BAD_REQUEST;
    if (entry->kind != ENVIRONMENT_CAP || entry->value >= source->grant_count) {
      return CALL_BAD_REQUEST;
    }
    selected = &grants[entry->value];
  }
  if (!selected) return CALL_NOT_FOUND;
  if ((request->rights & selected->rights) != request->rights) return CALL_DENIED;
  *handle = selected->source;
  return CALL_OK;
}

/* The launch's fixed capture of the packages SOURCE, shared by its pkg:// and
 * sdk:// so one launch never mixes two sets, and the volume root it was taken
 * from. A package-selected command keeps its captured set, whose volume SOURCE
 * must independently hold; the capture alone never supplies authority for
 * another package. Both stay owned by PREPARED. */
static enum call_status launch_capture(const struct bundle_program *program, handle_t source,
    struct bundle_launch *prepared, handle_t *root)
{
  *root = source;
  handle_t pinned = bundle_program_package_view(program);
  enum call_status status = CALL_OK;
  if (pinned != HANDLE_INVALID) {
    if (prepared->package_root == HANDLE_INVALID) {
      status = package_commands_bind(source, pinned, &prepared->package_root);
    }
    *root = prepared->package_root;
  }
  if (status != CALL_OK || prepared->package_capture != HANDLE_INVALID) {
    return status;
  }
  handle_t live = HANDLE_INVALID;
  status = package_commands_open(*root, &live);
  if (status == CALL_OK) {
    status = package_commands_capture(live, pinned, &prepared->package_capture);
    handle_close(live);
  }
  return status;
}

/* The bundle's pkg://: REQUEST's packages, selected from the launch's capture.
 * MISSING names a package that is absent. */
static enum call_status selected_packages(const struct bundle_program *program,
    const struct bundle_grant_request *request, handle_t source,
    struct bundle_launch *prepared, handle_t *view, const char **missing)
{
  *view = HANDLE_INVALID;
  handle_t root;
  enum call_status status = launch_capture(program, source, prepared, &root);
  handle_t selection = HANDLE_INVALID;
  for (size_t i = 0; status == CALL_OK && i < request->package_count; ++i) {
    handle_t next = HANDLE_INVALID;
    status = package_roots_select(prepared->package_capture, request->packages[i], selection,
        &next);
    if (status != CALL_OK) {
      *missing = request->packages[i];
    }
    if (selection != HANDLE_INVALID) {
      handle_close(selection);
    }
    selection = next;
  }
  if (status != CALL_OK) {
    if (selection != HANDLE_INVALID) {
      handle_close(selection);
    }
    return status;
  }
  *view = selection;
  return CALL_OK;
}

/* The bundle's sdk://, of the launch's captured set. */
static enum call_status selected_sdk(const struct bundle_program *program, handle_t source,
    struct bundle_launch *prepared, handle_t *view)
{
  *view = HANDLE_INVALID;
  handle_t root, live = HANDLE_INVALID;
  enum call_status status = launch_capture(program, source, prepared, &root);
  if (status == CALL_OK) {
    status = package_sdk_open(root, &live);
  }
  if (status == CALL_OK) {
    status = package_sdk_capture(live, prepared->package_capture, view);
    handle_close(live);
  }
  return status;
}

static enum call_status scoped_network_grant(const struct bundle_grant_request *request,
    handle_t factory, const struct bundle_authority *authority, size_t authority_count,
    handle_t *derived)
{
  *derived = HANDLE_INVALID;
  handle_t registry = HANDLE_INVALID;
  for (size_t i = 0; i < authority_count; ++i) {
    if (!authority[i].resource || strcmp(authority[i].resource, "net_registry") ||
        authority[i].protocol != PROTOCOL_NET_REGISTRY || authority[i].source == HANDLE_INVALID) {
      continue;
    }
    if (registry != HANDLE_INVALID) {
      return CALL_BAD_REQUEST;
    }
    registry = authority[i].source;
  }
  if (registry == HANDLE_INVALID) {
    return CALL_DENIED;
  }
  struct handle_info info;
  enum call_status status = handle_query(registry, &info);
  if (status != CALL_OK) {
    return status;
  }
  if (info.kind != HANDLE_KIND_NATIVE || info.protocol != PROTOCOL_NET_REGISTRY || info.transport) {
    return CALL_WRONG_TYPE;
  }
  if (!(info.rights & NET_REGISTRY_RIGHT_READ)) {
    return CALL_DENIED;
  }
  uint64_t interface_id = 0, after = 0;
  for (;;) {
    struct net_interface_reply interface;
    status = net_registry_next_interface(registry, after, &interface);
    if (status != CALL_OK) {
      return status;
    }
    if (!interface.interface_id) {
      return CALL_DENIED;
    }
    if (!strcmp(interface.name, request->interface)) {
      interface_id = interface.interface_id;
      break;
    }
    after = interface.interface_id;
  }
  /* The registry supplies identity, never packet authority. Native derivation
   * checks the held scope/rights and preserves the factory's revocation lineage. */
  switch (request->protocol) {
  case PROTOCOL_TCP_SERVICE:
    return tcp_factory_derive(factory, interface_id, request->rights, derived);
  case PROTOCOL_UDP_SERVICE:
    return udp_factory_derive(factory, interface_id, request->rights, derived);
  case PROTOCOL_ECHO:
    return echo_factory_derive(factory, interface_id, request->rights, derived);
  default: return CALL_BAD_REQUEST;
  }
}

static enum call_status prepare_resources(struct bundle_launch *prepared,
    const struct bundle_program *program,
    const struct bundle_grant_request *requests, size_t count,
    const struct bundle_authority *authority, size_t authority_count,
    const struct launch_request *launch_source, const char **missing)
{
  for (size_t i = 0; i < count; ++i) {
    if (has_entry(prepared, requests[i].name) ||
        (requests[i].root && strcmp(requests[i].root, requests[i].name) &&
          has_entry(prepared, requests[i].root))) {
      return CALL_BAD_REQUEST;
    }
    /* Private context authority is created separately, never copied from a
     * caller's client leaf, including the ordinary Lua/bundle path. */
    if (!strcmp(requests[i].resource, "job_work") ||
        !strcmp(requests[i].resource, "job_provisioner")) {
      continue;
    }
    handle_t source = HANDLE_INVALID;
    if (requests[i].protocol == PROTOCOL_PROVIDER) {
      enum call_status status = provider_source(launch_source, &requests[i], &source);
      if (status != CALL_OK) {
        if (status == CALL_BAD_REQUEST) return status;
        if (!requests[i].required) continue;
        *missing = requests[i].name;
        return status == CALL_NOT_FOUND ? CALL_DENIED : status;
      }
    } else {
      const char *selection = requests[i].source ? requests[i].source :
          requests[i].packages || requests[i].sdk ? PACKAGE_SOURCE : requests[i].resource;
      if (requests[i].protocol == PROTOCOL_TCP_SERVICE &&
          (requests[i].rights & TCP_SERVICE_RIGHT_LISTEN)) {
        for (size_t j = 0; j < authority_count; ++j) {
          if (authority[j].resource &&
              !strcmp(authority[j].resource, BUNDLE_TCP_LISTEN_SOURCE) &&
              authority[j].protocol == PROTOCOL_TCP_SERVICE &&
              authority[j].source != HANDLE_INVALID) {
            if (source != HANDLE_INVALID) {
              return CALL_BAD_REQUEST;
            }
            source = authority[j].source;
          }
        }
      }
      /* A caller that already holds a declared TCP LISTEN grant may supply it
       * as ordinary tcp authority for another explicit declaration. */
      bool private_listen = source != HANDLE_INVALID;
      for (size_t j = 0; !private_listen && j < authority_count; ++j) {
        if (authority[j].resource && !strcmp(selection, authority[j].resource) &&
            authority[j].protocol == requests[i].protocol) {
          if (source != HANDLE_INVALID && authority[j].source != HANDLE_INVALID) {
            return CALL_BAD_REQUEST;
          }
          source = authority[j].source;
        }
      }
    }
    if (source != HANDLE_INVALID && requests[i].packages) {
      handle_t view = HANDLE_INVALID;
      /* A damaged set names the first listed package; a missing one names itself. */
      const char *absent = requests[i].packages[0];
      if (selected_packages(program, &requests[i], source, prepared, &view, &absent) != CALL_OK) {
        *missing = absent;
        return CALL_DENIED;
      }
      prepared->owned_grants[prepared->owned_count++] = view;
      source = view;
    } else if (source == HANDLE_INVALID && requests[i].sdk) {
      enum call_status status = held_sdk(launch_source, &requests[i], &source);
      if (status == CALL_BAD_REQUEST) return status;
    } else if (source != HANDLE_INVALID && requests[i].sdk) {
      handle_t view = HANDLE_INVALID;
      if (selected_sdk(program, source, prepared, &view) != CALL_OK) {
        if (!requests[i].required) {
          continue;
        }
        *missing = requests[i].name;
        return CALL_DENIED;
      }
      prepared->owned_grants[prepared->owned_count++] = view;
      source = view;
    }
    if (source == HANDLE_INVALID) {
      if (requests[i].required) {
        *missing = requests[i].name;
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
        (info.kind != HANDLE_KIND_NATIVE && info.kind != HANDLE_KIND_EXPORTED) ||
        (requests[i].interface && (info.kind != HANDLE_KIND_NATIVE || info.transport))) {
      if (requests[i].required) {
        return CALL_WRONG_TYPE;
      }
      continue;
    }
    uint64_t transport = (info.kind == HANDLE_KIND_EXPORTED || info.protocol == PROTOCOL_ENDPOINT) ?
        HANDLE_TRANSPORT_CALL : 0;
    if ((requests[i].rights & info.rights) != requests[i].rights ||
        (transport & info.transport) != transport) {
      if (requests[i].required) {
        *missing = requests[i].name;
        return CALL_DENIED;
      }
      continue;
    }
    if (requests[i].interface) {
      handle_t derived;
      status = scoped_network_grant(&requests[i], source, authority, authority_count, &derived);
      if (status != CALL_OK) {
        if (status == CALL_BAD_REQUEST) {
          return status;
        }
        if (requests[i].required) {
          *missing = requests[i].name;
          return status;
        }
        continue;
      }
      prepared->owned_grants[prepared->owned_count++] = derived;
      source = derived;
      transport = 0;
    }
    size_t index = prepared->request.grant_count++;
    prepared->grants[index] = (struct launch_grant){source, requests[i].rights, transport};
    /* Constructors and listening authority are private runtime roles, selected
     * again explicitly when preparing another declared bundle grant. */
    bool private_role = requests[i].protocol == PROTOCOL_ENDPOINT_SERVICE ||
        (requests[i].protocol == PROTOCOL_TCP_SERVICE &&
         (requests[i].rights & TCP_SERVICE_RIGHT_LISTEN));
    uint64_t flags = private_role ? 0 : ENVIRONMENT_EXPORTED;
    prepared->entries[prepared->request.entry_count++] = (struct launch_environment_entry){
      .name = (uintptr_t)requests[i].name, .kind = ENVIRONMENT_CAP,
      .flags = flags, .value = index,
    };
    if (requests[i].root && strcmp(requests[i].root, requests[i].name)) {
      prepared->entries[prepared->request.entry_count++] = (struct launch_environment_entry){
        .name = (uintptr_t)requests[i].root, .kind = ENVIRONMENT_CAP,
        .flags = flags, .value = index,
      };
    }
  }
  return CALL_OK;
}

static bool has_selected(const struct bundle_launch *prepared,
    uint64_t protocol, uint64_t rights, handle_t *source)
{
  for (size_t i = 0; i < prepared->request.entry_count; ++i) {
    if (prepared->entries[i].kind != ENVIRONMENT_CAP) {
      continue;
    }
    const struct launch_grant *grant = &prepared->grants[prepared->entries[i].value];
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
    handle_t provisioner, struct bundle_jobs *jobs, const char *resource,
    const char **missing)
{
  bool declared = false;
  const char *required = NULL;
  for (size_t i = 0; i < count; ++i) {
    if (!strcmp(requests[i].resource, resource)) {
      declared = true;
      if (requests[i].required && !required) {
        required = requests[i].name;
      }
    }
  }
  if (!declared) {
    return CALL_OK;
  }
  handle_t clock = HANDLE_INVALID;
  bool usable = jobs && provisioner != HANDLE_INVALID &&
      has_selected(prepared, PROTOCOL_CLOCK, CLOCK_RIGHT_READ | CLOCK_RIGHT_PRECISE, &clock) &&
      has_selected(prepared, PROTOCOL_PIPE_SERVICE, PIPE_SERVICE_RIGHT_CREATE, NULL) &&
      has_selected(prepared, PROTOCOL_LAUNCHER, LAUNCHER_RIGHT_LAUNCH, NULL);
  if (!usable) {
    *missing = required;
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
  context->delegated = !strcmp(resource, "job_provisioner");
  context->next = prepared->context;
  prepared->context = context;
  prepared->jobs = jobs;
  for (size_t i = 0; i < count; ++i) {
    if (!strcmp(requests[i].resource, resource)) {
      size_t index = prepared->request.grant_count++;
      prepared->grants[index] = (struct launch_grant){
        job_work_directory(context->work), requests[i].rights, 0,
      };
      prepared->entries[prepared->request.entry_count++] = (struct launch_environment_entry){
        .name = (uintptr_t)requests[i].name, .kind = ENVIRONMENT_CAP, .value = index,
      };
    }
  }
  return CALL_OK;
}

static enum call_status prepare_bundle(const struct bundle_program *program,
    const struct launch_request *source, const struct bundle_authority *authority,
    size_t authority_count, handle_t provisioner, bool allow_provisioner,
    struct bundle_jobs *jobs,
    struct bundle_launch **result, const char **missing)
{
  const char *unused;
  if (!missing) {
    missing = &unused;
  }
  *missing = NULL;
  if (!result) {
    return CALL_BAD_REQUEST;
  }
  *result = NULL;
  if (!program || !source || (authority_count && !authority) ||
      (source->grant_count && !source->grants) ||
      (source->entry_count && !source->entries) ||
      (source->working_directory_count && !source->working_directories)) {
    return CALL_BAD_REQUEST;
  }
  size_t bundle_count, resource_count;
  const struct environment_value *bundle_entries = bundle_program_entries(program, &bundle_count);
  const struct bundle_grant_request *resources = bundle_program_grants(program, &resource_count);
  bool explicit_providers = false;
  for (size_t i = 0; i < resource_count; ++i) {
    explicit_providers |= resources[i].protocol == PROTOCOL_PROVIDER;
  }
  const size_t grant_limit = LAUNCH_CAPTURE_MAX_SIZE / sizeof(struct launch_grant);
  const size_t entry_limit = LAUNCH_CAPTURE_MAX_SIZE / sizeof(struct launch_environment_entry);
  if (source->grant_count > grant_limit || bundle_count > grant_limit ||
      resource_count > grant_limit - bundle_count ||
      source->grant_count > grant_limit - bundle_count - resource_count ||
      bundle_count > entry_limit || resource_count > (entry_limit - bundle_count) / 2 ||
      source->entry_count > entry_limit - bundle_count - 2 * resource_count ||
      source->working_directory_count > LAUNCH_CAPTURE_MAX_SIZE / sizeof(uint64_t)) {
    return CALL_LIMIT;
  }
  size_t grant_capacity = source->grant_count + bundle_count + resource_count;
  size_t entry_capacity = source->entry_count + bundle_count + 2 * resource_count;
  struct bundle_launch *prepared = calloc(1, sizeof(*prepared));
  uint64_t *mapped = source->grant_count ? malloc(source->grant_count * sizeof(*mapped)) : NULL;
  if (!prepared || (source->grant_count && !mapped)) {
    free(prepared);
    free(mapped);
    return CALL_NO_MEMORY;
  }
  prepared->grants = grant_capacity ? calloc(grant_capacity, sizeof(*prepared->grants)) : NULL;
  prepared->entries = entry_capacity ? calloc(entry_capacity, sizeof(*prepared->entries)) : NULL;
  prepared->directories = source->working_directory_count ?
      malloc(source->working_directory_count * sizeof(*prepared->directories)) : NULL;
  prepared->owned_grants = resource_count ? malloc(resource_count * sizeof(*prepared->owned_grants)) : NULL;
  if ((grant_capacity && !prepared->grants) || (entry_capacity && !prepared->entries) ||
      (source->working_directory_count && !prepared->directories) ||
      (resource_count && !prepared->owned_grants)) {
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
  prepared->request.entries = (uintptr_t)prepared->entries;
  prepared->request.entry_count = 0;
  prepared->request.working_directories = (uintptr_t)prepared->directories;
  struct context_grants context = {
    .source = (const void *)(uintptr_t)source->grants,
    .source_count = source->grant_count, .mapped = mapped, .prepared = prepared,
  };
  enum call_status status = prepare_context(&context, source, bundle_entries, bundle_count,
      explicit_providers);
  if (status == CALL_OK) {
    status = prepare_resources(prepared, program, resources, resource_count, authority, authority_count,
        source, missing);
  }
  if (status == CALL_OK) {
    status = prepare_job_work(prepared, resources, resource_count, provisioner, jobs,
        "job_work", missing);
  }
  if (status == CALL_OK) {
    status = prepare_job_work(prepared, resources, resource_count,
        allow_provisioner ? provisioner : HANDLE_INVALID, jobs, "job_provisioner", missing);
  }
  for (size_t i = 0; status == CALL_OK && i < bundle_count; ++i) {
    if (has_entry(prepared, bundle_entries[i].name)) {
      status = CALL_BAD_REQUEST;
      break;
    }
    size_t index = prepared->request.grant_count++;
    prepared->grants[index] = (struct launch_grant){bundle_entries[i].handle,
        DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_ENUMERATE | DIRECTORY_RIGHT_READ_FILES, 0};
    prepared->entries[prepared->request.entry_count++] = (struct launch_environment_entry){
      .name = (uintptr_t)bundle_entries[i].name, .kind = ENVIRONMENT_CAP,
      .flags = ENVIRONMENT_EXPORTED, .value = index,
    };
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
      HANDLE_INVALID, false, NULL, result, NULL);
}

enum call_status bundle_launch_prepare_jobs(const struct bundle_program *program,
    const struct launch_request *source, const struct bundle_authority *authority,
    size_t authority_count, handle_t provisioner, bool allow_provisioner,
    struct bundle_jobs *jobs,
    struct bundle_launch **result, const char **missing)
{
  return prepare_bundle(program, source, authority, authority_count, provisioner,
      allow_provisioner, jobs, result, missing);
}

void bundle_launch_admitted(struct bundle_launch *prepared, enum call_status status,
    handle_t process)
{
  if (!prepared || !prepared->context || (status != CALL_OK && status != CALL_OUTCOME_UNKNOWN)) {
    return;
  }
  for (struct bundle_job_context *context = prepared->context; context; context = context->next) {
    context->admitted = true;
    /* A provisioner can escape through successors/services. PROCESS retirement
     * proves only an ordinary client leaf is no longer used. */
    if (!context->delegated && status == CALL_OK && process != HANDLE_INVALID) {
      handle_copy_restricted(process, PROCESS_RIGHT_WAIT, 0, &context->observer);
    }
  }
}

struct job_work *bundle_launch_take_delegation(struct bundle_launch *prepared)
{
  if (!prepared) {
    return NULL;
  }
  struct bundle_job_context **link = &prepared->context;
  while (*link) {
    struct bundle_job_context *context = *link;
    if (context->admitted && context->delegated) {
      *link = context->next;
      struct job_work *work = context->work;
      free(context);
      return work;
    }
    link = &context->next;
  }
  return NULL;
}

enum call_status bundle_jobs_reap(struct bundle_jobs *jobs, bool report_start_failures)
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
    if (report_start_failures && completion.kind == PROCESS_START_FAILED) {
      fprintf(stderr, "bundle: job client did not start: %s: %s\n",
          process_start_step_name(completion.exit_status),
          process_start_status_name(completion.exit_status));
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
  enum call_status result = bundle_jobs_reap(jobs, true);
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
    while (prepared->context) {
      struct bundle_job_context *context = prepared->context;
      prepared->context = context->next;
      if (context->admitted && !context->delegated) {
        context->next = prepared->jobs->contexts;
        prepared->jobs->contexts = context;
      } else {
        job_work_close(context->work, !context->admitted);
        free(context);
      }
    }
    for (size_t i = 0; i < prepared->owned_count; ++i) {
      handle_close(prepared->owned_grants[i]);
    }
    free(prepared->owned_grants);
    if (prepared->package_root) handle_close(prepared->package_root);
    if (prepared->package_capture) handle_close(prepared->package_capture);
    free(prepared->directories);
    free(prepared->entries);
    free(prepared->grants);
    free(prepared);
  }
}

