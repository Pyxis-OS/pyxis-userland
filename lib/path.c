#include <abi/file.h>
#include <handle.h>
#include <path.h>
#include <startup.h>

static void close_chain(handle_t *directories, size_t count)
{
  while (count) {
    --count;
    handle_close(directories[count]);
    directories[count] = HANDLE_INVALID;
  }
}

enum call_status path_context_init(struct path_context *context,
    handle_t *storage, size_t capacity, const handle_t *directories, size_t count,
    uint64_t rights)
{
  if (!context) {
    return CALL_BAD_REQUEST;
  }
  *context = (struct path_context){0};
  if ((capacity && !storage) || (count && !directories) ||
      !(rights & DIRECTORY_RIGHT_LOOKUP) || (rights & ~DIRECTORY_RIGHTS)) {
    return CALL_BAD_REQUEST;
  }
  if (count > capacity) {
    return CALL_LIMIT;
  }
  for (size_t i = 0; i < count; ++i) {
    enum call_status status = handle_copy_restricted(directories[i], rights, &storage[i]);
    if (status != CALL_OK) {
      close_chain(storage, i);
      return status;
    }
  }
  *context = (struct path_context){storage, count, capacity, rights};
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
    const char **path, uint64_t rights, bool preserve_rights,
    struct path_workspace *workspace, size_t *count)
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
    root = startup_root(workspace->component);
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
    enum call_status status = preserve_rights ? handle_copy(source[*count], destination) :
                             handle_copy_restricted(source[*count], rights, destination);
    if (status != CALL_OK) {
      return status;
    }
    ++*count;
  }
  return CALL_OK;
}

static enum call_status walk(const struct path_context *context, const char *path,
    uint64_t kind, uint64_t rights, uint64_t directory_rights, bool preserve_rights,
    struct path_workspace *workspace, size_t *count, handle_t *file)
{
  if (!path || !path[0] || path[0] == '/' || !workspace ||
      (workspace->directory_capacity && !workspace->directories) ||
      (workspace->component_capacity && !workspace->component)) {
    return CALL_BAD_REQUEST;
  }
  enum call_status status = starting_chain(context, &path, directory_rights,
      preserve_rights, workspace, count);
  if (status != CALL_OK) {
    return status;
  }

  for (;;) {
    while (*path == '/') {
      ++path;
    }
    if (!*path) {
      return kind == DIRECTORY_KIND_DIRECTORY ? CALL_OK : CALL_WRONG_TYPE;
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
      continue;
    }
    if (length == 2 && start[0] == '.' && start[1] == '.') {
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
    if (!*path && !trailing_separator && kind == DIRECTORY_KIND_FILE) {
      return directory_lookup(parent, workspace->component, kind, rights, file);
    }
    if (*count == workspace->directory_capacity) {
      return CALL_LIMIT;
    }
    status = directory_lookup(parent, workspace->component, DIRECTORY_KIND_DIRECTORY,
        directory_rights, &workspace->directories[*count]);
    if (status != CALL_OK) {
      return status;
    }
    ++*count;
  }
}

enum call_status path_resolve(const struct path_context *context, const char *path,
    uint64_t kind, uint64_t rights, struct path_workspace *workspace, handle_t *handle)
{
  if (!handle) {
    return CALL_BAD_REQUEST;
  }
  *handle = HANDLE_INVALID;
  uint64_t directory_rights = DIRECTORY_RIGHT_LOOKUP;
  if (kind == DIRECTORY_KIND_FILE) {
    if (rights & ~FILE_RIGHT_READ) {
      return CALL_BAD_REQUEST;
    }
    if (rights & FILE_RIGHT_READ) {
      directory_rights |= DIRECTORY_RIGHT_READ_FILES;
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
  enum call_status status = walk(context, path, kind, rights, directory_rights,
      true, workspace, &count, handle);
  if (status == CALL_OK && kind == DIRECTORY_KIND_DIRECTORY) {
    status = handle_copy_restricted(workspace->directories[count - 1], rights, handle);
  }
  if (count) {
    close_chain(workspace->directories, count);
  }
  return status;
}

enum call_status path_change(struct path_context *context, const char *path,
                              struct path_workspace *workspace)
{
  if (!context || !(context->directory_rights & DIRECTORY_RIGHT_LOOKUP)) {
    return CALL_BAD_REQUEST;
  }
  size_t count = 0;
  handle_t unused;
  enum call_status status = walk(context, path, DIRECTORY_KIND_DIRECTORY,
      context->directory_rights, context->directory_rights, false, workspace, &count, &unused);
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
