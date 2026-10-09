#include "cp.h"
#include "../common/directory.h"
#include <file.h>
#include <handle.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* cp settings, not ABI: nesting below the root, entries per tree and entry
 * name bytes. The traversal is iterative over a fixed stack, so these bound
 * handles and time, not the program stack or the heap. */
#define TREE_MAX_DEPTH 32
#define TREE_MAX_ENTRIES 65536
#define TREE_NAME_BYTES CP_NAME_BYTES
#define SENTINEL_BYTES 26
#define PATH_BYTES CP_PATH_BYTES

/* One open directory pair. Level 0 is the root, whose handles the caller
 * owns; deeper levels own theirs. Names are kept for messages only. */
struct level {
  handle_t source;
  handle_t destination;
  struct directory_cursor cursor;
  char name[TREE_NAME_BYTES];
};

struct tree {
  const char *source;
  const char *destination;
  const char *sentinel;
  bool copy;
  uint64_t entries;
  uint64_t files;
  uint64_t directories;
};

static struct level levels[TREE_MAX_DEPTH + 1];
static uint64_t next_sentinel;
static char source_path[PATH_BYTES];
static char destination_path[PATH_BYTES];

static void append(char *out, size_t *used, const char *text)
{
  size_t length = strlen(text);
  size_t room = PATH_BYTES - 1 - *used;
  if (length > room) {
    length = room;
  }
  memcpy(out + *used, text, length);
  *used += length;
  out[*used] = '\0';
}

/* Display text such as root/a/b/leaf; long paths are truncated. */
static const char *entry_path(char *out, const char *root, unsigned depth, const char *leaf)
{
  size_t used = 0;
  out[0] = '\0';
  append(out, &used, root);
  for (unsigned i = 1; i <= depth; ++i) {
    append(out, &used, "/");
    append(out, &used, levels[i].name);
  }
  if (leaf) {
    append(out, &used, "/");
    append(out, &used, leaf);
  }
  return out;
}

static void fail_status(const struct tree *tree, unsigned depth, const char *leaf,
    enum call_status status)
{
  report_directory_error("cp", entry_path(source_path, tree->source, depth, leaf), status);
}

static void fail_text(const struct tree *tree, unsigned depth, const char *leaf,
    const char *text)
{
  fprintf(stderr, "cp: %s: %s\n", entry_path(source_path, tree->source, depth, leaf), text);
}

static void close_level(struct level *level, bool copy)
{
  handle_close(level->source);
  if (copy) {
    handle_close(level->destination);
  }
}

static enum cp_result copy_entry_file(struct tree *tree, unsigned depth, const char *entry)
{
  handle_t input;
  enum call_status status = directory_lookup(levels[depth].source, entry,
      DIRECTORY_KIND_FILE, FILE_RIGHT_READ, &input);
  if (status != CALL_OK) {
    fail_status(tree, depth, entry, status);
    return CP_FAILED;
  }
  enum cp_result result = cp_copy_file_handle(input,
      entry_path(source_path, tree->source, depth, entry), levels[depth].destination, entry,
      entry_path(destination_path, tree->destination, depth, entry));
  if (result == CP_SUCCESS) {
    ++tree->files;
  }
  return result;
}

/* Walk the tree depth-first. With copy unset it only enumerates and checks the
 * limits, entry kinds and sentinel. The walk closes every non-root level. */
static enum cp_result walk(struct tree *tree, handle_t source_root, handle_t destination_root)
{
  levels[0] = (struct level){.source = source_root, .destination = destination_root};
  unsigned depth = 0;
  enum cp_result result = CP_SUCCESS;
  for (;;) {
    struct level *level = &levels[depth];
    char entry[TREE_NAME_BYTES];
    struct directory_enumerate_reply reply;
    enum call_status status = directory_enumerate(level->source, &level->cursor, entry,
        sizeof(entry), &reply);
    if (status != CALL_OK) {
      fail_status(tree, depth, NULL, status);
      result = CP_FAILED;
      break;
    }
    if (reply.outcome == DIRECTORY_END) {
      if (depth == 0) {
        break;
      }
      close_level(level, tree->copy);
      --depth;
      continue;
    }
    if (reply.outcome == DIRECTORY_CHANGED) {
      fail_text(tree, depth, NULL, "Source changed during copy");
      result = CP_FAILED;
      break;
    }
    if (reply.outcome == DIRECTORY_BUFFER_TOO_SMALL) {
      fail_text(tree, depth, NULL, "Entry name longer than 255 bytes");
      result = CP_FAILED;
      break;
    }
    if (reply.outcome != DIRECTORY_ENTRY) {
      fail_status(tree, depth, NULL, CALL_BAD_REQUEST);
      result = CP_FAILED;
      break;
    }
    level->cursor = reply.cursor;
    if (++tree->entries > TREE_MAX_ENTRIES) {
      fail_text(tree, depth, NULL, "More than 65536 entries");
      result = CP_FAILED;
      break;
    }
    if (!tree->copy && !strcmp(entry, tree->sentinel)) {
      fail_text(tree, depth, entry,
          "Destination is inside the source, or this is a leftover cp sentinel");
      result = CP_FAILED;
      break;
    }

    if (reply.kind == DIRECTORY_KIND_FILE) {
      if (tree->copy) {
        result = copy_entry_file(tree, depth, entry);
        if (result != CP_SUCCESS) {
          break;
        }
      }
    } else if (reply.kind == DIRECTORY_KIND_DIRECTORY) {
      if (depth == TREE_MAX_DEPTH) {
        fail_text(tree, depth, entry, "Directories nested deeper than 32 levels");
        result = CP_FAILED;
        break;
      }
      handle_t source = HANDLE_INVALID;
      handle_t destination = HANDLE_INVALID;
      status = directory_lookup(level->source, entry, DIRECTORY_KIND_DIRECTORY,
          CP_SOURCE_RIGHTS, &source);
      if (status != CALL_OK) {
        fail_status(tree, depth, entry, status);
        result = CP_FAILED;
        break;
      }
      if (tree->copy) {
        status = directory_create(level->destination, entry, DIRECTORY_KIND_DIRECTORY,
            CP_DIRECTORY_RIGHTS, &destination);
        if (status != CALL_OK) {
          handle_close(source);
          report_directory_error("cp",
              entry_path(destination_path, tree->destination, depth, entry), status);
          result = status == CALL_OUTCOME_UNKNOWN ? CP_UNCERTAIN : CP_FAILED;
          break;
        }
        ++tree->directories;
      }
      ++depth;
      levels[depth] = (struct level){.source = source, .destination = destination};
      strcpy(levels[depth].name, entry);
    } else {
      const char *text = reply.kind == DIRECTORY_KIND_SYMLINK ? "Symbolic link not copied" :
          reply.kind == DIRECTORY_KIND_OTHER ? "Special file not copied" :
          "Unknown entry type not copied";
      fail_text(tree, depth, entry, text);
      result = CP_FAILED;
      break;
    }
  }
  while (depth > 0) {
    close_level(&levels[depth], tree->copy);
    --depth;
  }
  return result;
}

/* Undo the new root after a failed check: nothing has been copied into it. */
static enum cp_result discard_root(const struct tree *tree, handle_t parent, handle_t root,
    const char *name, bool sentinel_present)
{
  enum cp_result result = CP_FAILED;
  if (sentinel_present) {
    enum call_status status = directory_remove(root, tree->sentinel, DIRECTORY_KIND_FILE);
    if (status != CALL_OK && status != CALL_NOT_FOUND) {
      report_directory_error("cp", tree->destination, status);
      fprintf(stderr, "cp: %s: Could not remove %s; it remains\n", tree->destination,
          tree->sentinel);
      result = CP_UNCERTAIN;
    }
  }
  if (handle_close(root) != 0) {
    report_directory_error("cp", tree->destination, CALL_BAD_HANDLE);
    result = CP_UNCERTAIN;
  }
  enum call_status status = directory_remove(parent, name, DIRECTORY_KIND_DIRECTORY);
  if (status != CALL_OK && status != CALL_NOT_FOUND) {
    report_directory_error("cp", tree->destination, status);
    fprintf(stderr, "cp: %s: Could not remove the new directory; it remains\n",
        tree->destination);
    result = CP_UNCERTAIN;
  }
  return result;
}

enum cp_result cp_copy_tree(handle_t source_root, const char *source, handle_t parent,
    const char *name, const char *destination)
{
  char sentinel[SENTINEL_BYTES];
  snprintf(sentinel, sizeof(sentinel), ".cp-tree-%016" PRIx64, next_sentinel++);
  struct tree tree = {.source = source, .destination = destination, .sentinel = sentinel};

  /* The root is created exclusively: an existing name of any kind is refused. */
  handle_t root;
  enum call_status status = directory_create(parent, name, DIRECTORY_KIND_DIRECTORY,
      CP_DIRECTORY_RIGHTS, &root);
  if (status != CALL_OK) {
    handle_close(source_root);
    report_directory_error("cp", destination, status);
    if (status == CALL_OUTCOME_UNKNOWN) {
      fprintf(stderr, "cp: %s: Creation not confirmed; check the name\n", destination);
      return CP_UNCERTAIN;
    }
    return CP_FAILED;
  }

  /* No object identity exists, so prove a destination inside the source by
   * putting a unique name in the new root and looking for it in the source. */
  handle_t marker;
  status = directory_create(root, sentinel, DIRECTORY_KIND_FILE, 0, &marker);
  if (status != CALL_OK) {
    handle_close(source_root);
    report_directory_error("cp", destination, status);
    return discard_root(&tree, parent, root, name, false) == CP_UNCERTAIN ?
        CP_UNCERTAIN : CP_FAILED;
  }
  handle_close(marker);
  enum cp_result result = walk(&tree, source_root, HANDLE_INVALID);
  if (result != CP_SUCCESS) {
    handle_close(source_root);
    enum cp_result discarded = discard_root(&tree, parent, root, name, true);
    return result == CP_UNCERTAIN || discarded == CP_UNCERTAIN ? CP_UNCERTAIN : CP_FAILED;
  }
  status = directory_remove(root, sentinel, DIRECTORY_KIND_FILE);
  if (status != CALL_OK) {
    handle_close(source_root);
    report_directory_error("cp", destination, status);
    fprintf(stderr, "cp: %s: Could not remove %s; it remains\n", destination, sentinel);
    handle_close(root);
    return CP_UNCERTAIN;
  }

  tree.copy = true;
  tree.entries = 0;
  tree.directories = 1;
  result = walk(&tree, source_root, root);
  if (result != CP_SUCCESS) {
    fprintf(stderr,
        "cp: %s: Incomplete copy of %s (%" PRIu64 " files, %" PRIu64
        " directories created); nothing removed\n",
        destination, source, tree.files, tree.directories);
  }
  bool closed = handle_close(source_root) == 0;
  closed = handle_close(root) == 0 && closed;
  if (!closed) {
    report_directory_error("cp", destination, CALL_BAD_HANDLE);
    if (result == CP_SUCCESS) {
      result = CP_FAILED;
    }
  }
  return result;
}
