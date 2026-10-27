#include <abi/file.h>
#include <handle.h>
#include <clock.h>
#include <directory.h>
#include <environment.h>
#include <package.h>
#include <path.h>
#include <resource.h>
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

enum call_status path_root(const char *name, handle_t *root)
{
  return environment_lookup_file(startup_environment_handle(), name, root);
}

/* Lookup captures one owned value; neither context nor scratch retains live
 * table pointers. Relative paths instead copy the independently held cwd chain. */
static enum call_status starting_chain(const struct path_context *context,
    const char **path, struct path_workspace *workspace, size_t *count,
    handle_t *provider)
{
  const char *start = *path;
  const char *end = start;
  while (*end && *end != '/' && *end != ':') {
    ++end;
  }
  if (*end == ':' && end[1] == '/' && end[2] == '/') {
    if (end == start) {
      return CALL_BAD_REQUEST;
    }
    enum call_status status = copy_component(workspace, start, end - start);
    if (status != CALL_OK) {
      return status;
    }
    handle_t root = HANDLE_INVALID;
    status = path_root(workspace->component, &root);
    if (status != CALL_OK) {
      return status;
    }
    struct handle_info info;
    status = handle_query(root, &info);
    if (status != CALL_OK) {
      handle_close(root);
      return status;
    }
    *path = end + 3;
    if (info.protocol != PROTOCOL_DIRECTORY) {
      *provider = root;
      return CALL_OK;
    }
    if (!workspace->directory_capacity) {
      handle_close(root);
      return CALL_LIMIT;
    }
    workspace->directories[(*count)++] = root;
    *path = end + 3;
    return CALL_OK;
  }
  if (!context || !context->count) {
    return CALL_UNAVAILABLE;
  }
  if (context->count > workspace->directory_capacity) {
    return CALL_LIMIT;
  }
  while (*count < context->count) {
    enum call_status status = handle_copy(context->directories[*count],
        &workspace->directories[*count]);
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
  WALK_REMOVE,
  WALK_CREATE_DIRECTORY,
  WALK_CREATE_DIRECTORIES,
};

static enum call_status walk(const struct path_context *context, const char *path,
    enum walk_target target, uint64_t rights, uint64_t directory_rights,
    bool preserve_directory_rights, struct path_workspace *workspace, size_t *count,
    handle_t *file, bool *provider_route, bool native_only, uintmax_t *created)
{
  if (!path || !path[0] || path[0] == '/' || !workspace ||
      (workspace->directory_capacity && !workspace->directories) ||
      (workspace->component_capacity && !workspace->component)) {
    return CALL_BAD_REQUEST;
  }
  handle_t provider = HANDLE_INVALID;
  enum call_status status = starting_chain(context, &path, workspace, count, &provider);
  if (status != CALL_OK) {
    return status;
  }
  if (provider != HANDLE_INVALID) {
    if (provider_route) {
      *provider_route = true;
    }
    struct handle_info binding;
    status = handle_query(provider, &binding);
    if (status == CALL_OK && native_only &&
        (binding.kind != HANDLE_KIND_NATIVE || binding.transport ||
        binding.protocol == PROTOCOL_PROVIDER)) {
      status = CALL_BAD_OPERATION;
    }
    if (status == CALL_OK && target == WALK_REMOVE) {
      if (binding.protocol != PROTOCOL_PROVIDER) {
        status = CALL_BAD_OPERATION;
      } else {
        struct resource_result result;
        status = provider_remove_typed(provider, path, workspace->deadline_ns, &result);
        if (status == CALL_OK) {
          status = resource_outcome_status(result.info.description.outcome);
        }
        resource_result_close(&result);
      }
      enum call_status closed = handle_close(provider);
      return status == CALL_OK ? closed : status;
    }
    if (status == CALL_OK && target != WALK_FILE) {
      status = CALL_BAD_OPERATION;
    }
    if (status == CALL_OK) {
      struct resource_result result;
      uint64_t intent = ((rights & FILE_RIGHT_READ) ? PROVIDER_INTENT_READ : 0) |
          ((rights & FILE_RIGHT_WRITE) ? PROVIDER_INTENT_WRITE : 0);
      status = resource_open_held(provider, path, intent, 0, workspace->deadline_ns, &result);
      if (status == CALL_OK) {
        status = resource_outcome_status(result.info.description.outcome);
        if (status == CALL_OK && result.info.protocol != PROTOCOL_FILE) {
          status = CALL_BAD_OPERATION;
        }
        if (status == CALL_OK) {
          *file = result.resource;
          result.resource = HANDLE_INVALID;
        }
        resource_result_close(&result);
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
    return status;
  }

  for (;;) {
    handle_t current = workspace->directories[*count - 1];
    if (native_only) {
      struct handle_info info;
      status = handle_query(current, &info);
      if (status != CALL_OK) {
        return status;
      }
      if (info.protocol != PROTOCOL_DIRECTORY || info.kind != HANDLE_KIND_NATIVE ||
          info.transport) {
        return CALL_BAD_OPERATION;
      }
    }
    while (*path == '/') {
      ++path;
    }
    if (!*path) {
      if (target == WALK_PARENT || target == WALK_REMOVE) {
        return CALL_BAD_REQUEST;
      }
      if (target == WALK_CREATE_DIRECTORY || target == WALK_CREATE_DIRECTORIES) {
        struct file_info_reply info;
        status = directory_info(current, &info);
        return status == CALL_OK && target == WALK_CREATE_DIRECTORY ?
            CALL_ALREADY_EXISTS : status;
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
      if ((target == WALK_PARENT || target == WALK_REMOVE) && !*path) {
        return CALL_BAD_REQUEST;
      }
      continue;
    }
    if (length == 2 && start[0] == '.' && start[1] == '.') {
      if ((target == WALK_PARENT || target == WALK_REMOVE) && !*path) {
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
    if (!*path && target == WALK_CREATE_DIRECTORY) {
      handle_t created;
      status = directory_create(parent, workspace->component, DIRECTORY_KIND_DIRECTORY,
          0, &created);
      if (status == CALL_OK && handle_close(created) != 0) {
        status = CALL_IO;
      }
      return status;
    }
    if (!*path && (target == WALK_PARENT || target == WALK_REMOVE)) {
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
    if (status == CALL_NOT_FOUND && target == WALK_CREATE_DIRECTORIES) {
      status = directory_create(parent, workspace->component, DIRECTORY_KIND_DIRECTORY,
          directory_rights, &workspace->directories[*count]);
      if (status == CALL_OK && created) {
        ++*created;
      }
      if (status == CALL_ALREADY_EXISTS) {
        /* A competing creator is checked once; uncertain mutations are final. */
        status = directory_lookup(parent, workspace->component, DIRECTORY_KIND_DIRECTORY,
            directory_rights, &workspace->directories[*count]);
      }
    }
    if (status != CALL_OK) {
      return status;
    }
    ++*count;
  }
}

enum call_status path_open_directory_native(const struct path_context *context, const char *path,
    uint64_t required_rights, struct path_workspace *workspace, handle_t *handle)
{
  if (!handle || (required_rights & ~DIRECTORY_RIGHTS)) {
    return CALL_BAD_REQUEST;
  }
  *handle = HANDLE_INVALID;
  size_t count = 0;
  handle_t unused;
  enum call_status status = walk(context, path, WALK_DIRECTORY, 0, 0, true,
      workspace, &count, &unused, NULL, true, NULL);
  if (status == CALL_OK) {
    uint64_t rights;
    status = handle_rights(workspace->directories[count - 1], &rights, NULL);
    if (status == CALL_OK && (rights & required_rights) != required_rights) {
      status = CALL_DENIED;
    }
    if (status == CALL_OK) {
      *handle = workspace->directories[--count];
      workspace->directories[count] = HANDLE_INVALID;
    }
  }
  if (count) {
    close_chain(workspace->directories, count);
  }
  return status;
}

enum call_status path_create_directory_native(const struct path_context *context, const char *path,
    bool recursive, struct path_workspace *workspace)
{
  return path_create_directory_native_count(context, path, recursive, workspace, NULL);
}

enum call_status path_create_directory_native_count(const struct path_context *context, const char *path,
    bool recursive, struct path_workspace *workspace, uintmax_t *created)
{
  size_t count = 0;
  handle_t unused;
  uintmax_t created_count = 0;
  enum call_status status = walk(context, path,
      recursive ? WALK_CREATE_DIRECTORIES : WALK_CREATE_DIRECTORY,
      0, 0, true, workspace, &count, &unused, NULL, true, &created_count);
  if (count) {
    close_chain(workspace->directories, count);
  }
  if (status == CALL_OK && created) {
    *created = recursive ? created_count : 1;
  }
  return status;
}

static enum call_status resolve(const struct path_context *context, const char *path,
    uint64_t kind, uint64_t rights, struct path_workspace *workspace, handle_t *handle,
    bool *provider_route, bool native_only)
{
  if (!handle) {
    return CALL_BAD_REQUEST;
  }
  *handle = HANDLE_INVALID;
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
      false, workspace, &count, handle, provider_route, native_only, NULL);
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
      workspace, &count, &unused, NULL, false, NULL);
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
      false, workspace, &count, &unused, NULL, false, NULL);
  if (status == CALL_OK) {
    status = directory_create(workspace->directories[count - 1], workspace->component,
        DIRECTORY_KIND_FILE, rights, handle);
  }
  if (count) {
    close_chain(workspace->directories, count);
  }
  return status;
}

static enum call_status remove_path(const struct path_context *context, const char *path,
    uint64_t kind, struct path_workspace *workspace, bool native_only)
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
  bool provider_route = false;
  enum walk_target target = kind == DIRECTORY_KIND_DIRECTORY ? WALK_PARENT : WALK_REMOVE;
  enum call_status status = walk(context, path, target, 0,
      DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_REMOVE, native_only, workspace, &count,
      &unused, &provider_route, native_only, NULL);
  if (status == CALL_OK && !provider_route) {
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

enum call_status path_remove(const struct path_context *context, const char *path,
    uint64_t kind, struct path_workspace *workspace)
{
  return remove_path(context, path, kind, workspace, false);
}

enum call_status path_remove_native(const struct path_context *context, const char *path,
    uint64_t kind, struct path_workspace *workspace)
{
  return remove_path(context, path, kind, workspace, true);
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
      DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_CREATE, false, workspace, &count, &unused, NULL, false, NULL);
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
static enum call_status mutation_parent(const struct path_context *context, const char *path,
    uint64_t rights, struct path_workspace *workspace, handle_t *parent, bool native_only)
{
  *parent = HANDLE_INVALID;
  size_t count = 0;
  handle_t unused;
  enum call_status status = walk(context, path, WALK_PARENT, 0,
      DIRECTORY_RIGHT_LOOKUP | rights, native_only, workspace, &count, &unused, NULL, native_only, NULL);
  if (status == CALL_OK) {
    *parent = workspace->directories[--count];
    workspace->directories[count] = HANDLE_INVALID;
  }
  if (count) {
    close_chain(workspace->directories, count);
  }
  return status;
}

static enum call_status rename_path(const struct path_context *context, const char *source,
    const char *destination, uint64_t policy, struct path_workspace *source_workspace,
    struct path_workspace *destination_workspace, bool native_only)
{
  if (policy != DIRECTORY_RENAME_NO_REPLACE && policy != DIRECTORY_RENAME_REPLACE) {
    return CALL_BAD_REQUEST;
  }
  handle_t source_parent, destination_parent;
  enum call_status status = mutation_parent(context, source, DIRECTORY_RIGHT_REMOVE,
      source_workspace, &source_parent, native_only);
  if (status != CALL_OK) {
    return status;
  }
  uint64_t rights = DIRECTORY_RIGHT_CREATE;
  if (policy == DIRECTORY_RENAME_REPLACE) {
    rights |= DIRECTORY_RIGHT_REMOVE;
  }
  status = mutation_parent(context, destination, rights, destination_workspace,
      &destination_parent, native_only);
  if (!native_only && status == CALL_DENIED && policy == DIRECTORY_RENAME_REPLACE) {
    /* CREATE alone suffices for an absent destination. Do not probe its name
     * in userspace: the kernel checks replacement authority at publication. */
    status = mutation_parent(context, destination, DIRECTORY_RIGHT_CREATE,
        destination_workspace, &destination_parent, false);
  }
  if (status == CALL_OK) {
    uint64_t kind = source[strlen(source) - 1] == '/' ||
        destination[strlen(destination) - 1] == '/' ?
        DIRECTORY_KIND_DIRECTORY : DIRECTORY_KIND_ANY;
    status = directory_rename(source_parent, source_workspace->component,
        destination_parent, destination_workspace->component, policy, kind);
    handle_close(destination_parent);
  }
  handle_close(source_parent);
  return status;
}

enum call_status path_rename(const struct path_context *context, const char *source,
    const char *destination, uint64_t policy, struct path_workspace *source_workspace,
    struct path_workspace *destination_workspace)
{
  return rename_path(context, source, destination, policy, source_workspace,
      destination_workspace, false);
}

enum call_status path_rename_native(const struct path_context *context, const char *source,
    const char *destination, uint64_t policy, struct path_workspace *source_workspace,
    struct path_workspace *destination_workspace)
{
  return rename_path(context, source, destination, policy, source_workspace,
      destination_workspace, true);
}

enum call_status path_link(const struct path_context *context, const char *source,
    const char *destination, struct path_workspace *source_workspace,
    struct path_workspace *destination_workspace)
{
  handle_t source_parent, destination_parent;
  enum call_status status = mutation_parent(context, source, DIRECTORY_LINK_SOURCE_RIGHTS,
      source_workspace, &source_parent, false);
  if (status != CALL_OK) {
    return status;
  }
  status = mutation_parent(context, destination, DIRECTORY_RIGHT_CREATE,
      destination_workspace, &destination_parent, false);
  if (status == CALL_OK) {
    if (source[strlen(source) - 1] == '/' || destination[strlen(destination) - 1] == '/') {
      status = CALL_WRONG_TYPE;
    } else {
      status = directory_link(source_parent, source_workspace->component,
          destination_parent, destination_workspace->component);
    }
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
      0, 0, true, workspace, &count, &unused, NULL, false, NULL);
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
