#include <abi/file.h>
#include <handle.h>
#include <clock.h>
#include <namespace.h>
#include <path.h>
#include <provider.h>
#include <startup.h>
#include <string.h>

static void close_chain(handle_t *directories, size_t count)
{
  while (count) {
    --count;
    handle_close(directories[count]);
    directories[count] = HANDLE_INVALID;
  }
}

enum call_status path_context_init(struct path_context *context,
    handle_t *storage, size_t capacity, const handle_t *directories, size_t count)
{
  if (!context) {
    return CALL_BAD_REQUEST;
  }
  *context = (struct path_context){0};
  if ((capacity && !storage) || (count && !directories)) {
    return CALL_BAD_REQUEST;
  }
  if (count > capacity) {
    return CALL_LIMIT;
  }
  for (size_t i = 0; i < count; ++i) {
    enum call_status status = handle_copy(directories[i], &storage[i]);
    if (status != CALL_OK) {
      close_chain(storage, i);
      return status;
    }
  }
  *context = (struct path_context){
    .directories = storage, .count = count, .capacity = capacity,
  };
  return CALL_OK;
}

void path_context_close(struct path_context *context)
{
  if (context) {
    close_chain(context->directories, context->count);
    *context = (struct path_context){0};
  }
}

static enum call_status copy_component(struct path_workspace *workspace,
                                        const char *start, size_t length)
{
  if (length >= workspace->component_capacity) {
    return CALL_LIMIT;
  }
  for (size_t i = 0; i < length; ++i) {
    workspace->component[i] = start[i];
  }
  workspace->component[length] = 0;
  return CALL_OK;
}

/* Only a leading scheme:// selects a root; colons elsewhere are ordinary name
 * bytes. Startup scheme names exclude ':' and '/', so the prefix is unambiguous. */
static enum call_status starting_chain(const struct path_context *context,
    const char **path, struct path_workspace *workspace, size_t *count,
    handle_t *provider, bool http)
{
  const char *start = *path;
  const char *end = start;
  while (*end && *end != '/' && *end != ':') {
    ++end;
  }
  handle_t root;
  const handle_t *source;
  size_t source_count;
  if (*end == ':' && end[1] == '/' && end[2] == '/') {
    if (end == start) {
      return CALL_BAD_REQUEST;
    }
    enum call_status status = copy_component(workspace, start, end - start);
    if (status != CALL_OK) {
      return status;
    }
    if (http) {
      const char *name = end - start == 5 ? "https" : "http";
      if (strlen(name) >= workspace->component_capacity) {
        return CALL_LIMIT;
      }
      memcpy(workspace->component, name, strlen(name) + 1);
    }
    root = HANDLE_INVALID;
    if (context && context->roots) {
      for (size_t i = 0; i < context->root_count; ++i) {
        if (!strcmp(context->roots[i].name, workspace->component)) {
          root = context->roots[i].handle;
          break;
        }
      }
    } else {
      root = startup_root(workspace->component);
    }
    if (http && root != HANDLE_INVALID) {
      return CALL_BAD_REQUEST;
    }
    handle_t namespace_handle = context && context->namespace_handle != HANDLE_INVALID ?
        context->namespace_handle : startup_namespace();
    if (namespace_handle != HANDLE_INVALID) {
      handle_t binding = HANDLE_INVALID;
      enum call_status lookup = namespace_lookup(namespace_handle,
          workspace->component, &binding);
      if (lookup == CALL_OK || lookup == CALL_ENDPOINT_CLOSED) {
        if (root != HANDLE_INVALID) {
          if (binding != HANDLE_INVALID) {
            handle_close(binding);
          }
          return CALL_BAD_REQUEST;
        }
        *provider = binding;
        return lookup;
      }
      if (lookup != CALL_NOT_FOUND && lookup != CALL_BAD_REQUEST) {
        return lookup;
      }
    }
    if (root == HANDLE_INVALID) {
      return CALL_NOT_FOUND;
    }
    source = &root;
    source_count = 1;
    *path = end + 3;
  } else {
    if (!context || !context->count) {
      return CALL_UNAVAILABLE;
    }
    source = context->directories;
    source_count = context->count;
  }
  if (source_count > workspace->directory_capacity) {
    return CALL_LIMIT;
  }
  while (*count < source_count) {
    handle_t *destination = &workspace->directories[*count];
    enum call_status status = handle_copy(source[*count], destination);
    if (status != CALL_OK) {
      return status;
    }
    ++*count;
  }
  return CALL_OK;
}

enum walk_target {
  WALK_FILE,
  WALK_DIRECTORY,
  WALK_PARENT,
};

static enum call_status walk(const struct path_context *context, const char *path,
    enum walk_target target, uint64_t rights, uint64_t directory_rights,
    bool preserve_directory_rights, struct path_workspace *workspace, size_t *count,
    handle_t *file, bool *provider_route, bool native_only)
{
  if (!path || !path[0] || path[0] == '/' || !workspace ||
      (workspace->directory_capacity && !workspace->directories) ||
      (workspace->component_capacity && !workspace->component)) {
    return CALL_BAD_REQUEST;
  }
  const char *uri = path;
  bool http = target == WALK_FILE && !native_only &&
      provider_http_uri(uri);
  uint64_t deadline = 0;
  enum call_status clock_status = CALL_OK;
  if (http) {
    uint64_t now;
    handle_t clock = workspace->clock ? workspace->clock : startup_resource("clock");
    clock_status = clock_now(clock, &now);
    if (clock_status == CALL_OK) {
      if (now > UINT64_MAX - HTTP_FETCH_NS) {
        clock_status = CALL_LIMIT;
      } else {
        deadline = now + HTTP_FETCH_NS;
        if (workspace->deadline_ns && workspace->deadline_ns < deadline) {
          deadline = workspace->deadline_ns;
        }
      }
    }
  }
  if (clock_status != CALL_OK) {
    return clock_status;
  }
  handle_t provider = HANDLE_INVALID;
  enum call_status status = starting_chain(context, &path, workspace, count, &provider, http);
  if (status != CALL_OK) {
    return status;
  }
  if (provider != HANDLE_INVALID) {
    if (provider_route) {
      *provider_route = true;
    }
    status = native_only ? CALL_BAD_OPERATION : CALL_UNAVAILABLE;
    if (!native_only && target == WALK_FILE) {
      if (http) {
        status = clock_status == CALL_OK ? provider_http_open(context, provider,
            uri, rights, workspace->clock ? workspace->clock : startup_resource("clock"),
            deadline, workspace->http, workspace->response, file) : clock_status;
      } else {
        struct provider_result result;
        status = provider_open(provider, uri, rights, 0, &result, file);
        if (status == CALL_OK) {
          status = result.status;
          if (status == CALL_OK && result.outcome != PROVIDER_OUTCOME_BYTES) {
            status = CALL_BAD_OPERATION;
          }
        }
        if (status == CALL_OK && workspace->response) {
          struct pyxis_response_info *info = workspace->response;
          info->flags = PYXIS_RESPONSE_PROVIDER;
          info->status = result.provider_status;
          if (result.metadata.media_type_size) {
            info->flags |= PYXIS_RESPONSE_MEDIA_TYPE;
            memcpy(info->media_type, result.metadata.media_type,
                result.metadata.media_type_size + 1);
          }
        }
      }
    }
    enum call_status closed = handle_close(provider);
    if (status == CALL_OK) {
      status = closed;
    }
    if (status != CALL_OK && file && *file != HANDLE_INVALID) {
      handle_close(*file);
      *file = HANDLE_INVALID;
    }
    if (status != CALL_OK && workspace->response) {
      *workspace->response = (struct pyxis_response_info){0};
    }
    return status;
  }

  for (;;) {
    while (*path == '/') {
      ++path;
    }
    if (!*path) {
      if (target == WALK_PARENT) {
        return CALL_BAD_REQUEST;
      }
      return target == WALK_DIRECTORY ? CALL_OK : CALL_WRONG_TYPE;
    }
    const char *start = path;
    while (*path && *path != '/') {
      ++path;
    }
    size_t length = path - start;
    bool trailing_separator = *path == '/';
    while (*path == '/') {
      ++path;
    }
    if (length == 1 && start[0] == '.') {
      if (target == WALK_PARENT && !*path) {
        return CALL_BAD_REQUEST;
      }
      continue;
    }
    if (length == 2 && start[0] == '.' && start[1] == '.') {
      if (target == WALK_PARENT && !*path) {
        return CALL_BAD_REQUEST;
      }
      if (*count == 1) {
        return CALL_DENIED;
      }
      --*count;
      close_chain(&workspace->directories[*count], 1);
      continue;
    }

    status = copy_component(workspace, start, length);
    if (status != CALL_OK) {
      return status;
    }
    handle_t parent = workspace->directories[*count - 1];
    if (!*path && target == WALK_PARENT) {
      return CALL_OK;
    }
    if (!*path && !trailing_separator && target == WALK_FILE) {
      return directory_lookup(parent, workspace->component, DIRECTORY_KIND_FILE, rights, file);
    }
    if (*count == workspace->directory_capacity) {
      return CALL_LIMIT;
    }
    if (preserve_directory_rights) {
      status = handle_rights(parent, &directory_rights, NULL);
      if (status != CALL_OK) {
        return status;
      }
    }
    status = directory_lookup(parent, workspace->component, DIRECTORY_KIND_DIRECTORY,
        directory_rights, &workspace->directories[*count]);
    if (status != CALL_OK) {
      return status;
    }
    ++*count;
  }
}

static enum call_status resolve(const struct path_context *context, const char *path,
    uint64_t kind, uint64_t rights, struct path_workspace *workspace, handle_t *handle,
    bool *provider_route, bool native_only)
{
  if (!handle) {
    return CALL_BAD_REQUEST;
  }
  *handle = HANDLE_INVALID;
  if (workspace && workspace->response) {
    *workspace->response = (struct pyxis_response_info){0};
  }
  uint64_t directory_rights = DIRECTORY_RIGHT_LOOKUP;
  if (kind == DIRECTORY_KIND_FILE) {
    if (rights & ~FILE_RIGHTS) {
      return CALL_BAD_REQUEST;
    }
    if (rights & FILE_RIGHT_READ) {
      directory_rights |= DIRECTORY_RIGHT_READ_FILES;
    }
    if (rights & FILE_RIGHT_WRITE) {
      directory_rights |= DIRECTORY_RIGHT_WRITE_FILES;
    }
  } else if (kind == DIRECTORY_KIND_DIRECTORY) {
    if (rights & ~DIRECTORY_RIGHTS) {
      return CALL_BAD_REQUEST;
    }
    directory_rights |= rights;
  } else {
    return CALL_BAD_REQUEST;
  }

  size_t count = 0;
  enum walk_target target = kind == DIRECTORY_KIND_FILE ? WALK_FILE : WALK_DIRECTORY;
  enum call_status status = walk(context, path, target, rights, directory_rights,
      false, workspace, &count, handle, provider_route, native_only);
  if (status == CALL_OK && kind == DIRECTORY_KIND_DIRECTORY) {
    status = handle_copy_restricted(workspace->directories[count - 1], rights, 0, handle);
  }
  if (count) {
    close_chain(workspace->directories, count);
  }
  return status;
}

enum call_status path_resolve(const struct path_context *context, const char *path,
    uint64_t kind, uint64_t rights, struct path_workspace *workspace, handle_t *handle)
{
  return resolve(context, path, kind, rights, workspace, handle, NULL, false);
}

enum call_status path_resolve_native(const struct path_context *context, const char *path,
    uint64_t kind, uint64_t rights, struct path_workspace *workspace, handle_t *handle)
{
  return resolve(context, path, kind, rights, workspace, handle, NULL, true);
}

enum call_status path_open_file(const struct path_context *context, const char *path,
    uint64_t rights, bool create, struct path_workspace *workspace, handle_t *handle)
{
  bool provider_route = false;
  enum call_status status = resolve(context, path, DIRECTORY_KIND_FILE, rights,
      workspace, handle, &provider_route, false);
  if (status != CALL_NOT_FOUND || !create || provider_route) {
    return status;
  }
  size_t length = strlen(path);
  if (path[length - 1] == '/') {
    return status;
  }

  uint64_t directory_rights = DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_CREATE;
  if (rights & FILE_RIGHT_READ) {
    directory_rights |= DIRECTORY_RIGHT_READ_FILES;
  }
  if (rights & FILE_RIGHT_WRITE) {
    directory_rights |= DIRECTORY_RIGHT_WRITE_FILES;
  }
  size_t count = 0;
  handle_t unused;
  status = walk(context, path, WALK_PARENT, 0, directory_rights, false,
      workspace, &count, &unused, NULL, false);
  if (status == CALL_OK) {
    handle_t parent = workspace->directories[count - 1];
    status = directory_create(parent, workspace->component, DIRECTORY_KIND_FILE,
        rights, handle);
    if (status == CALL_ALREADY_EXISTS) {
      /* One concurrent creator may win; do not replay a mutation. */
      status = directory_lookup(parent, workspace->component, DIRECTORY_KIND_FILE,
          rights, handle);
    }
  }
  if (count) {
    close_chain(workspace->directories, count);
  }
  return status;
}

enum call_status path_create_file(const struct path_context *context, const char *path,
    uint64_t rights, struct path_workspace *workspace, handle_t *handle)
{
  if (!handle) {
    return CALL_BAD_REQUEST;
  }
  *handle = HANDLE_INVALID;
  if (!path || !*path || (rights & ~FILE_RIGHTS)) {
    return CALL_BAD_REQUEST;
  }
  if (path[strlen(path) - 1] == '/') {
    return CALL_WRONG_TYPE;
  }
  uint64_t directory_rights = DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_CREATE;
  if (rights & FILE_RIGHT_READ) {
    directory_rights |= DIRECTORY_RIGHT_READ_FILES;
  }
  if (rights & FILE_RIGHT_WRITE) {
    directory_rights |= DIRECTORY_RIGHT_WRITE_FILES;
  }
  size_t count = 0;
  handle_t unused;
  enum call_status status = walk(context, path, WALK_PARENT, 0, directory_rights,
      false, workspace, &count, &unused, NULL, false);
  if (status == CALL_OK) {
    status = directory_create(workspace->directories[count - 1], workspace->component,
        DIRECTORY_KIND_FILE, rights, handle);
  }
  if (count) {
    close_chain(workspace->directories, count);
  }
  return status;
}

enum call_status path_remove(const struct path_context *context, const char *path,
    uint64_t kind, struct path_workspace *workspace)
{
  if (!path || !*path || (kind != DIRECTORY_KIND_ANY && kind != DIRECTORY_KIND_FILE &&
      kind != DIRECTORY_KIND_DIRECTORY)) {
    return CALL_BAD_REQUEST;
  }
  const char *end = path;
  while (*end) {
    ++end;
  }
  bool trailing_separator = end[-1] == '/';
  size_t count = 0;
  handle_t unused;
  enum call_status status = walk(context, path, WALK_PARENT, 0,
      DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_REMOVE, false, workspace, &count, &unused, NULL, false);
  if (status == CALL_OK) {
    if (trailing_separator && kind == DIRECTORY_KIND_FILE) {
      status = CALL_WRONG_TYPE;
    } else {
      status = directory_remove(workspace->directories[count - 1], workspace->component,
          trailing_separator ? DIRECTORY_KIND_DIRECTORY : kind);
    }
  }
  if (count) {
    close_chain(workspace->directories, count);
  }
  return status;
}

enum call_status path_create_directory(const struct path_context *context, const char *path,
    struct path_workspace *workspace)
{
  if (!path || !*path) {
    return CALL_BAD_REQUEST;
  }
  size_t count = 0;
  handle_t unused;
  enum call_status status = walk(context, path, WALK_PARENT, 0,
      DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_CREATE, false, workspace, &count, &unused, NULL, false);
  if (status == CALL_BAD_REQUEST) {
    /* A root or a final . or .. has no parent entry to create, but names an
     * existing directory when it resolves; malformed paths still fail. The
     * partial chain is closed first because resolve reuses the workspace. */
    if (count) {
      close_chain(workspace->directories, count);
      count = 0;
    }
    handle_t existing;
    if (resolve(context, path, DIRECTORY_KIND_DIRECTORY, DIRECTORY_RIGHT_LOOKUP,
        workspace, &existing, NULL, false) == CALL_OK) {
      handle_close(existing);
      status = CALL_ALREADY_EXISTS;
    }
  } else if (status == CALL_OK) {
    handle_t directory;
    status = directory_create(workspace->directories[count - 1], workspace->component,
        DIRECTORY_KIND_DIRECTORY, DIRECTORY_RIGHT_LOOKUP, &directory);
    if (status == CALL_OK) {
      handle_close(directory);
    }
  }
  if (count) {
    close_chain(workspace->directories, count);
  }
  return status;
}

/* Leave the leaf name in component and transfer the last chain handle out.
 * Ancestors are closed; no directory entry or child handle is looked up. */
static enum call_status rename_parent(const struct path_context *context, const char *path,
    uint64_t rights, struct path_workspace *workspace, handle_t *parent)
{
  *parent = HANDLE_INVALID;
  size_t count = 0;
  handle_t unused;
  enum call_status status = walk(context, path, WALK_PARENT, 0,
      DIRECTORY_RIGHT_LOOKUP | rights, false, workspace, &count, &unused, NULL, false);
  if (status == CALL_OK) {
    const char *end = path;
    while (*end) {
      ++end;
    }
    if (end[-1] == '/') {
      status = CALL_WRONG_TYPE;
    } else {
      *parent = workspace->directories[--count];
      workspace->directories[count] = HANDLE_INVALID;
    }
  }
  if (count) {
    close_chain(workspace->directories, count);
  }
  return status;
}

enum call_status path_rename(const struct path_context *context, const char *source,
    const char *destination, uint64_t policy, struct path_workspace *source_workspace,
    struct path_workspace *destination_workspace)
{
  if (policy != DIRECTORY_RENAME_NO_REPLACE && policy != DIRECTORY_RENAME_REPLACE) {
    return CALL_BAD_REQUEST;
  }
  handle_t source_parent, destination_parent;
  enum call_status status = rename_parent(context, source, DIRECTORY_RIGHT_REMOVE,
      source_workspace, &source_parent);
  if (status != CALL_OK) {
    return status;
  }
  uint64_t rights = DIRECTORY_RIGHT_CREATE;
  if (policy == DIRECTORY_RENAME_REPLACE) {
    rights |= DIRECTORY_RIGHT_REMOVE;
  }
  status = rename_parent(context, destination, rights, destination_workspace, &destination_parent);
  if (status == CALL_DENIED && policy == DIRECTORY_RENAME_REPLACE) {
    /* CREATE alone suffices for an absent destination. Do not probe its name
     * in userspace: the kernel checks replacement authority at publication. */
    status = rename_parent(context, destination, DIRECTORY_RIGHT_CREATE,
        destination_workspace, &destination_parent);
  }
  if (status == CALL_OK) {
    status = directory_rename(source_parent, source_workspace->component,
        destination_parent, destination_workspace->component, policy);
    handle_close(destination_parent);
  }
  handle_close(source_parent);
  return status;
}

enum call_status path_change(struct path_context *context, const char *path,
                              struct path_workspace *workspace)
{
  if (!context || (context->capacity && !context->directories)) {
    return CALL_BAD_REQUEST;
  }
  size_t count = 0;
  handle_t unused;
  enum call_status status = walk(context, path, WALK_DIRECTORY,
      0, 0, true, workspace, &count, &unused, NULL, false);
  if (status == CALL_OK) {
    uint64_t rights;
    status = handle_rights(workspace->directories[count - 1], &rights, NULL);
    if (status == CALL_OK && !(rights & DIRECTORY_RIGHT_LOOKUP)) {
      status = CALL_DENIED;
    }
  }
  if (status == CALL_OK && count > context->capacity) {
    status = CALL_LIMIT;
  }
  if (status != CALL_OK) {
    if (count) {
      close_chain(workspace->directories, count);
    }
    return status;
  }

  close_chain(context->directories, context->count);
  for (size_t i = 0; i < count; ++i) {
    context->directories[i] = workspace->directories[i];
    workspace->directories[i] = HANDLE_INVALID;
  }
  context->count = count;
  return CALL_OK;
}
