#include <directory.h>
#include <errno.h>
#include <handle.h>
#include <path.h>
#include <pyxis/directory.h>
#include <pyxis/working_path.h>
#include <stdlib.h>
#include <string.h>
#include "errors.h"
#include "working_path_internal.h"

#define REMOVE_BATCH_SIZE 64
#define REMOVE_CHANGE_RESTARTS 8
#define REMOVE_DIRECTORY_RIGHTS \
  (DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_ENUMERATE | DIRECTORY_RIGHT_REMOVE)

struct removal_entry {
  char *name;
  uint64_t kind;
};

struct removal_frame {
  struct removal_frame *parent;
  handle_t directory;
  char *name;
  struct directory_cursor cursor;
  struct removal_entry entries[REMOVE_BATCH_SIZE];
  size_t count, next;
  bool end;
};

struct removal_walk {
  struct path_context parents;
  char *input;
  char *buffer;
  size_t capacity;
  unsigned restarts;
};

static void close_owned(handle_t *handle, enum call_status *status)
{
  handle_t owned = *handle;
  *handle = HANDLE_INVALID;
  if (owned != HANDLE_INVALID && handle_close(owned) && *status == CALL_OK) {
    *status = CALL_IO;
  }
}

static enum call_status native_directory(handle_t directory, uint64_t required,
    struct handle_info *info)
{
  enum call_status status = handle_query(directory, info);
  if (status != CALL_OK) {
    return status;
  }
  if (info->protocol != PROTOCOL_DIRECTORY || info->kind != HANDLE_KIND_NATIVE ||
      info->transport) {
    return CALL_BAD_OPERATION;
  }
  return (info->rights & required) == required ? CALL_OK : CALL_DENIED;
}

static enum call_status grow_parents(struct path_context *parents)
{
  if (parents->count < parents->capacity) {
    return CALL_OK;
  }
  size_t maximum = SIZE_MAX / sizeof(*parents->directories);
  if (parents->count == maximum) {
    return CALL_LIMIT;
  }
  size_t capacity = parents->capacity > maximum / 2 ? maximum : parents->capacity * 2;
  if (!capacity) {
    capacity = 1;
  }
  handle_t *grown = realloc(parents->directories, capacity * sizeof(*grown));
  if (!grown) {
    return CALL_NO_MEMORY;
  }
  parents->directories = grown;
  parents->capacity = capacity;
  return CALL_OK;
}

/* This ordered walk owns every temporary parent so even '..' has a checked
 * close. Generic path helpers deliberately do not expose cleanup failures. */
static enum call_status resolve_parent(struct removal_walk *walk, const char *path,
    char **leaf, bool *trailing)
{
  if (!path || !*path || path[0] == '/') {
    return CALL_BAD_REQUEST;
  }
  walk->input = strdup(path);
  if (!walk->input) {
    return CALL_NO_MEMORY;
  }
  size_t root = libc_path_root_prefix(walk->input);
  char *component = walk->input;
  enum call_status status;
  if (root) {
    walk->input[root - 3] = '\0';
    status = grow_parents(&walk->parents);
    if (status != CALL_OK) {
      return status;
    }
    status = path_root(walk->input, &walk->parents.directories[0]);
    if (status != CALL_OK) {
      return status;
    }
    walk->parents.count = 1;
    component += root;
  } else {
    struct pyxis_working_snapshot snapshot;
    status = pyxis_working_snapshot_init(&snapshot, NULL);
    if (status != CALL_OK) {
      return status;
    }
    /* Transfer the snapshot's owned chain to checked cleanup below. */
    walk->parents = snapshot.context;
    free(snapshot.path);
    if (!walk->parents.count) {
      return CALL_UNAVAILABLE;
    }
  }

  for (;;) {
    struct handle_info info;
    status = native_directory(walk->parents.directories[walk->parents.count - 1], 0, &info);
    if (status != CALL_OK) {
      return status;
    }
    while (*component == '/') {
      ++component;
    }
    if (!*component) {
      return CALL_BAD_REQUEST;
    }
    char *end = component;
    while (*end && *end != '/') {
      ++end;
    }
    bool separator = *end == '/';
    char *next = end;
    while (*next == '/') {
      ++next;
    }
    bool final = !*next;
    *end = '\0';
    bool dot = !strcmp(component, ".");
    bool parent = !strcmp(component, "..");
    if (final) {
      if (dot || parent) {
        return CALL_BAD_REQUEST;
      }
      *leaf = component;
      *trailing = separator;
      return CALL_OK;
    }
    if (parent) {
      if (walk->parents.count == 1) {
        return CALL_DENIED;
      }
      close_owned(&walk->parents.directories[--walk->parents.count], &status);
      if (status != CALL_OK) {
        return status;
      }
    } else if (!dot) {
      status = grow_parents(&walk->parents);
      if (status != CALL_OK) {
        return status;
      }
      status = directory_lookup(walk->parents.directories[walk->parents.count - 1],
          component, DIRECTORY_KIND_DIRECTORY, info.rights,
          &walk->parents.directories[walk->parents.count]);
      if (status != CALL_OK) {
        return status;
      }
      ++walk->parents.count;
    }
    component = next;
  }
}

static void clear_entries(struct removal_frame *frame)
{
  for (size_t i = 0; i < frame->count; ++i) {
    free(frame->entries[i].name);
    frame->entries[i] = (struct removal_entry){0};
  }
  frame->count = 0;
  frame->next = 0;
}

static struct removal_frame *close_frame(struct removal_frame *frame, enum call_status *status)
{
  struct removal_frame *parent = frame->parent;
  close_owned(&frame->directory, status);
  clear_entries(frame);
  free(frame->name);
  free(frame);
  return parent;
}

static void confirmed_removal(struct removal_walk *walk, struct removal_frame *frame)
{
  walk->restarts = 0;
  if (frame) {
    frame->cursor = (struct directory_cursor){0};
    frame->end = false;
  }
}

static enum call_status collect_entries(struct removal_walk *walk, struct removal_frame *frame)
{
  clear_entries(frame);
  while (frame->count < REMOVE_BATCH_SIZE) {
    struct directory_enumerate_reply entry;
    enum call_status status = directory_enumerate(frame->directory, &frame->cursor,
        walk->buffer, walk->capacity, &entry);
    if (status == CALL_NOT_FOUND) {
      clear_entries(frame);
      frame->end = true;
      return CALL_OK;
    }
    if (status != CALL_OK) {
      return status;
    }
    if (entry.outcome == DIRECTORY_BUFFER_TOO_SMALL) {
      size_t capacity = (size_t)entry.name_size;
      if ((uint64_t)capacity != entry.name_size || capacity <= walk->capacity) {
        return CALL_LIMIT;
      }
      char *grown = realloc(walk->buffer, capacity);
      if (!grown) {
        return CALL_NO_MEMORY;
      }
      walk->buffer = grown;
      walk->capacity = capacity;
      continue;
    }
    if (entry.outcome == DIRECTORY_CHANGED) {
      clear_entries(frame);
      frame->cursor = (struct directory_cursor){0};
      if (++walk->restarts > REMOVE_CHANGE_RESTARTS) {
        return CALL_WOULD_BLOCK;
      }
      continue;
    }
    if (entry.outcome == DIRECTORY_END) {
      frame->end = true;
      return CALL_OK;
    }
    if (entry.outcome != DIRECTORY_ENTRY || entry.name_size < 2 ||
        strlen(walk->buffer) != entry.name_size - 1 || strchr(walk->buffer, '/') ||
        !strcmp(walk->buffer, ".") || !strcmp(walk->buffer, "..")) {
      return CALL_BAD_REQUEST;
    }
    if (entry.kind == DIRECTORY_KIND_OTHER) {
      return CALL_BAD_OPERATION;
    }
    char *name = strdup(walk->buffer);
    if (!name) {
      return CALL_NO_MEMORY;
    }
    frame->entries[frame->count++] = (struct removal_entry){name, entry.kind};
    frame->cursor = entry.cursor;
  }
  return CALL_OK;
}

static enum call_status open_child(struct removal_frame *frame, struct removal_entry *entry,
    handle_t *directory, uint64_t *kind)
{
  *directory = HANDLE_INVALID;
  *kind = entry->kind;
  if (*kind == DIRECTORY_KIND_UNKNOWN) {
    struct directory_typed_lookup_reply reply;
    enum call_status status = directory_lookup_typed(frame->directory, entry->name,
        0, REMOVE_DIRECTORY_RIGHTS, &reply);
    if (status == CALL_LINK_NOT_FOLLOWED) {
      *kind = DIRECTORY_KIND_SYMLINK;
      return CALL_OK;
    }
    if (status != CALL_OK) {
      return status;
    }
    if (reply.protocol == PROTOCOL_FILE) {
      *kind = DIRECTORY_KIND_FILE;
      close_owned(&reply.handle, &status);
      return status;
    }
    *kind = DIRECTORY_KIND_DIRECTORY;
    *directory = reply.handle;
  } else if (*kind == DIRECTORY_KIND_DIRECTORY) {
    enum call_status status = directory_lookup(frame->directory, entry->name,
        DIRECTORY_KIND_DIRECTORY, REMOVE_DIRECTORY_RIGHTS, directory);
    if (status != CALL_OK) {
      return status;
    }
  } else if (*kind != DIRECTORY_KIND_FILE && *kind != DIRECTORY_KIND_SYMLINK) {
    return CALL_BAD_OPERATION;
  }
  if (*directory != HANDLE_INVALID) {
    struct handle_info info;
    return native_directory(*directory, REMOVE_DIRECTORY_RIGHTS, &info);
  }
  return CALL_OK;
}

static enum call_status remove_tree(struct removal_walk *walk, handle_t parent,
    struct removal_frame **current)
{
  enum call_status status = CALL_OK;
  while (*current) {
    struct removal_frame *frame = *current;
    if (frame->next == frame->count) {
      if (!frame->end) {
        status = collect_entries(walk, frame);
        if (status != CALL_OK) {
          return status;
        }
        if (frame->count) {
          continue;
        }
      }
      if (frame->end) {
        handle_t owner = frame->parent ? frame->parent->directory : parent;
        status = directory_remove(owner, frame->name, DIRECTORY_KIND_DIRECTORY);
        if (status == CALL_OK) {
          confirmed_removal(walk, frame->parent);
        } else if (status == CALL_NOT_FOUND) {
          status = CALL_OK;
        }
        if (status != CALL_OK) {
          return status;
        }
        *current = close_frame(frame, &status);
        if (status != CALL_OK) {
          return status;
        }
        continue;
      }
    }

    struct removal_entry *entry = &frame->entries[frame->next++];
    handle_t directory;
    uint64_t kind;
    status = open_child(frame, entry, &directory, &kind);
    if (status == CALL_NOT_FOUND) {
      status = CALL_OK;
    } else if (status == CALL_OK && directory != HANDLE_INVALID) {
      struct removal_frame *child = calloc(1, sizeof(*child));
      if (!child) {
        status = CALL_NO_MEMORY;
      } else {
        child->parent = frame;
        child->directory = directory;
        directory = HANDLE_INVALID;
        child->name = entry->name;
        entry->name = NULL;
        *current = child;
      }
    } else if (status == CALL_OK) {
      status = directory_remove(frame->directory, entry->name, kind);
      if (status == CALL_OK) {
        confirmed_removal(walk, frame);
      } else if (status == CALL_NOT_FOUND) {
        status = CALL_OK;
      }
    }
    close_owned(&directory, &status);
    free(entry->name);
    entry->name = NULL;
    if (status != CALL_OK) {
      return status;
    }
  }
  return status;
}

int pyxis_directory_remove_all(const char *path)
{
  struct removal_walk walk = {0};
  struct removal_frame *frame = NULL;
  char *leaf;
  bool trailing;
  bool not_directory = false;
  enum call_status status = resolve_parent(&walk, path, &leaf, &trailing);
  if (status == CALL_WRONG_TYPE) {
    not_directory = true;
  }
  if (status != CALL_OK) {
    goto done;
  }
  handle_t parent = walk.parents.directories[walk.parents.count - 1];
  struct directory_typed_lookup_reply root;
  status = directory_lookup_typed(parent, leaf, 0, REMOVE_DIRECTORY_RIGHTS, &root);
  if (status == CALL_LINK_NOT_FOLLOWED && !trailing) {
    status = directory_remove(parent, leaf, DIRECTORY_KIND_SYMLINK);
    if (status == CALL_NOT_FOUND) {
      status = CALL_OK;
    }
    goto done;
  }
  if (status != CALL_OK) {
    goto done;
  }
  if (root.protocol != PROTOCOL_DIRECTORY) {
    status = CALL_WRONG_TYPE;
    not_directory = true;
    close_owned(&root.handle, &status);
    goto done;
  }
  struct handle_info info;
  status = native_directory(root.handle, REMOVE_DIRECTORY_RIGHTS, &info);
  if (status == CALL_OK) {
    frame = calloc(1, sizeof(*frame));
    if (!frame) {
      status = CALL_NO_MEMORY;
    } else {
      frame->directory = root.handle;
      root.handle = HANDLE_INVALID;
      frame->name = strdup(leaf);
      if (!frame->name) {
        status = CALL_NO_MEMORY;
      }
    }
  }
  close_owned(&root.handle, &status);
  if (status == CALL_OK) {
    status = remove_tree(&walk, parent, &frame);
  }
done:
  while (frame) {
    frame = close_frame(frame, &status);
  }
  while (walk.parents.count) {
    close_owned(&walk.parents.directories[--walk.parents.count], &status);
  }
  free(walk.parents.directories);
  free(walk.input);
  free(walk.buffer);
  if (status != CALL_OK) {
    errno = not_directory ? ENOTDIR : libc_path_errno(status, path, NULL);
    return -1;
  }
  return 0;
}
