#include "../common/directory.h"
#include <abi/file_info.h>
#include <handle.h>
#include <pyxis/working_path.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Deeper trees are reported, not removed: each level holds a handle and a path copy. */
#define RM_DEPTH_LIMIT 64
/* Removing an entry invalidates the enumeration cursor, so each pass names a
 * bounded batch, removes it, and enumerates again from the start. */
#define RM_BATCH 64
#define RM_CHANGED_RETRIES 8
#define RM_NAME_CAPACITY 128
#define RM_RIGHTS (DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_ENUMERATE | DIRECTORY_RIGHT_REMOVE)

struct entry {
  char *name;
  uint64_t kind;
};

struct batch {
  struct entry entries[RM_BATCH];
  size_t count;
};

/* Names this run could not remove from one directory. They stay in place, so
 * later passes must not select them again. */
struct skipped {
  char **names;
  size_t count;
  size_t capacity;
};

enum collect {
  COLLECT_DONE,
  COLLECT_FULL,
  COLLECT_CHANGED,
  COLLECT_ERROR,
};

enum same {
  SAME_NO,
  SAME_YES,
  SAME_UNKNOWN,
};

static bool output_failed;

static int usage(void)
{
  fputs("usage: rm [-r] [--] path...\n", stderr);
  return EXIT_FAILURE;
}

static void report_status(const char *path, enum call_status status)
{
  if (report_directory_error("rm", path, status) < 0) {
    output_failed = true;
  }
}

static void report_text(const char *path, const char *message)
{
  if (fprintf(stderr, "rm: %s: %s\n", path, message) < 0) {
    output_failed = true;
  }
}

static char *join(const char *directory, const char *name)
{
  size_t base = strlen(directory);
  size_t length = strlen(name);
  bool slash = base && directory[base - 1] != '/';
  char *path = malloc(base + slash + length + 1);
  if (!path) {
    return NULL;
  }
  memcpy(path, directory, base);
  if (slash) {
    path[base] = '/';
  }
  memcpy(path + base + slash, name, length + 1);
  return path;
}

static void free_batch(struct batch *batch)
{
  for (size_t i = 0; i < batch->count; ++i) {
    free(batch->entries[i].name);
  }
  batch->count = 0;
}

static void free_skipped(struct skipped *skipped)
{
  for (size_t i = 0; i < skipped->count; ++i) {
    free(skipped->names[i]);
  }
  free(skipped->names);
  *skipped = (struct skipped){0};
}

static bool is_skipped(const struct skipped *skipped, const char *name)
{
  for (size_t i = 0; i < skipped->count; ++i) {
    if (!strcmp(skipped->names[i], name)) {
      return true;
    }
  }
  return false;
}

static bool skip_name(struct skipped *skipped, const char *name)
{
  if (skipped->count == skipped->capacity) {
    size_t capacity = skipped->capacity ? skipped->capacity * 2 : 8;
    char **names = realloc(skipped->names, capacity * sizeof(*names));
    if (!names) {
      return false;
    }
    skipped->names = names;
    skipped->capacity = capacity;
  }
  char *copy = strdup(name);
  if (!copy) {
    return false;
  }
  skipped->names[skipped->count++] = copy;
  return true;
}

/* Fills the batch from a fresh enumeration, ignoring skipped names. Nothing is
 * removed during a pass, so CHANGED can only come from another process. */
static enum collect collect_batch(handle_t directory, const char *path,
    const struct skipped *skipped, struct batch *batch)
{
  size_t capacity = RM_NAME_CAPACITY;
  char *name = malloc(capacity);
  if (!name) {
    report_status(path, CALL_NO_MEMORY);
    return COLLECT_ERROR;
  }
  enum collect result = COLLECT_ERROR;
  struct directory_cursor cursor = {0};
  for (;;) {
    struct directory_enumerate_reply reply;
    enum call_status status = directory_enumerate(directory, &cursor, name, capacity, &reply);
    if (status != CALL_OK) {
      report_status(path, status);
      break;
    }
    if (reply.outcome == DIRECTORY_END) {
      result = COLLECT_DONE;
      break;
    }
    if (reply.outcome == DIRECTORY_CHANGED) {
      result = COLLECT_CHANGED;
      break;
    }
    if (reply.outcome == DIRECTORY_BUFFER_TOO_SMALL) {
      char *larger = realloc(name, reply.name_size);
      if (!larger) {
        report_status(path, CALL_NO_MEMORY);
        break;
      }
      name = larger;
      capacity = reply.name_size;
      continue; /* A short buffer leaves the cursor unchanged. */
    }
    cursor = reply.cursor;
    if (is_skipped(skipped, name)) {
      continue;
    }
    char *copy = strdup(name);
    if (!copy) {
      report_status(path, CALL_NO_MEMORY);
      break;
    }
    batch->entries[batch->count++] = (struct entry){.name = copy, .kind = reply.kind};
    if (batch->count == RM_BATCH) {
      result = COLLECT_FULL;
      break;
    }
  }
  free(name);
  return result;
}

static bool empty_directory(handle_t directory, const char *path, size_t depth);

/* Descends into one subdirectory, then removes its now-empty name. A child
 * that could not be emptied is left in place, and so are its ancestors. */
static bool remove_subdirectory(handle_t parent, const char *name, const char *path,
    size_t depth)
{
  if (depth + 1 > RM_DEPTH_LIMIT) {
    char message[64];
    snprintf(message, sizeof(message), "Deeper than %d levels; not removed", RM_DEPTH_LIMIT);
    report_text(path, message);
    return false;
  }
  handle_t child;
  enum call_status status = directory_lookup(parent, name, DIRECTORY_KIND_DIRECTORY,
      RM_RIGHTS, &child);
  if (status == CALL_NOT_FOUND) {
    return true;
  }
  if (status != CALL_OK) {
    report_status(path, status);
    return false;
  }
  /* The failures below were already reported; they are not repeated per level. */
  bool emptied = empty_directory(child, path, depth + 1);
  handle_close(child);
  if (!emptied) {
    return false;
  }
  status = directory_remove(parent, name, DIRECTORY_KIND_DIRECTORY);
  if (status != CALL_OK && status != CALL_NOT_FOUND) {
    report_status(path, status);
    return false;
  }
  return true;
}

/* A missing name counts as removed: another process got there first. */
static bool remove_entry(handle_t directory, const char *parent, const struct entry *entry,
    size_t depth)
{
  char *path = join(parent, entry->name);
  if (!path) {
    report_status(parent, CALL_NO_MEMORY);
    return false;
  }
  bool removed = false;
  if (entry->kind == DIRECTORY_KIND_FILE) {
    enum call_status status = directory_remove(directory, entry->name, DIRECTORY_KIND_FILE);
    removed = status == CALL_OK || status == CALL_NOT_FOUND;
    if (!removed) {
      report_status(path, status);
    }
  } else if (entry->kind == DIRECTORY_KIND_DIRECTORY) {
    removed = remove_subdirectory(directory, entry->name, path, depth);
  } else {
    report_text(path, "Symbolic link or special entry; not removed");
  }
  free(path);
  return removed;
}

/* Removes every entry of an open directory. Returns false if any entry
 * remains, after reporting each reason. */
static bool empty_directory(handle_t directory, const char *path, size_t depth)
{
  struct skipped skipped = {0};
  struct batch batch = {0};
  bool complete = true;
  size_t retries = 0;
  while (!output_failed) {
    enum collect collected = collect_batch(directory, path, &skipped, &batch);
    if (collected == COLLECT_ERROR) {
      complete = false;
      break;
    }
    if (collected == COLLECT_CHANGED) {
      free_batch(&batch);
      if (++retries > RM_CHANGED_RETRIES) {
        report_text(path, "Directory keeps changing; not emptied");
        complete = false;
        break;
      }
      continue;
    }
    if (!batch.count) {
      break;
    }
    for (size_t i = 0; i < batch.count && !output_failed; ++i) {
      const struct entry *entry = &batch.entries[i];
      if (!remove_entry(directory, path, entry, depth)) {
        complete = false;
        if (!skip_name(&skipped, entry->name)) {
          report_status(path, CALL_NO_MEMORY);
          output_failed = true;
        }
      }
    }
    free_batch(&batch);
  }
  free_batch(&batch);
  free_skipped(&skipped);
  return complete && !output_failed;
}

static enum same compare_identity(const struct file_info_reply *left,
    const struct file_info_reply *right)
{
  if (!(left->valid & right->valid & FILE_INFO_DOMAIN_VALID)) {
    return SAME_UNKNOWN;
  }
  if (left->domain != right->domain) {
    return SAME_NO;
  }
  if (!(left->valid & right->valid & FILE_INFO_OBJECT_VALID)) {
    return SAME_UNKNOWN;
  }
  return left->object == right->object ? SAME_YES : SAME_NO;
}

/* The shell's chain is the current directory and every ancestor up to its
 * boundary. Removing any of them would leave the caller in a removed directory,
 * so an operand that is one of them is refused, and so is one that cannot be
 * told apart from them. Returns NULL when the directory is safe to empty. */
static const char *check_working_chain(handle_t directory, enum call_status *failure)
{
  struct file_info_reply target;
  *failure = directory_info(directory, &target);
  if (*failure != CALL_OK) {
    return NULL;
  }
  const struct path_context *context;
  *failure = pyxis_working_context(&context);
  if (*failure != CALL_OK) {
    return NULL;
  }
  for (size_t i = 0; i < context->count; ++i) {
    struct file_info_reply link;
    enum same same = directory_info(context->directories[i], &link) == CALL_OK ?
        compare_identity(&target, &link) : SAME_UNKNOWN;
    if (same == SAME_YES) {
      return "Is the current directory or one of its ancestors; not removed";
    }
    if (same == SAME_UNKNOWN) {
      return "Cannot tell it from the current directory; not removed";
    }
  }
  return NULL;
}

/* Refuses operands that name a root, the current directory or its parent
 * before anything is removed. path_remove never removes a root either, but
 * only after the tree below it would already be gone. */
static const char *refused_operand(const char *path, size_t *length)
{
  const char *scheme = strstr(path, "://");
  const char *rest = scheme ? scheme + 3 : path;
  size_t end = strlen(rest);
  while (end && rest[end - 1] == '/') {
    --end;
  }
  *length = (size_t)(rest - path) + end;
  if (!end) {
    return "Is a root; not removed";
  }
  size_t start = end;
  while (start && rest[start - 1] != '/') {
    --start;
  }
  size_t component = end - start;
  if ((component == 1 && rest[start] == '.') ||
      (component == 2 && rest[start] == '.' && rest[start + 1] == '.')) {
    return "Refusing to remove '.' or '..'";
  }
  return NULL;
}

static bool remove_tree_operand(const char *path)
{
  if (!*path) {
    report_status(path, CALL_BAD_REQUEST);
    return false;
  }
  size_t length;
  const char *refusal = refused_operand(path, &length);
  if (refusal) {
    report_text(path, refusal);
    return false;
  }
  char *target = malloc(length + 1);
  if (!target) {
    report_status(path, CALL_NO_MEMORY);
    return false;
  }
  memcpy(target, path, length);
  target[length] = '\0';

  /* A file needs no traversal; WRONG_TYPE says it is something to descend into. */
  enum call_status status = remove_path(target, DIRECTORY_KIND_FILE);
  bool removed = status == CALL_OK;
  if (status == CALL_WRONG_TYPE) {
    handle_t directory;
    status = resolve_directory(target, RM_RIGHTS, &directory);
    if (status != CALL_OK) {
      report_status(path, status);
    } else {
      enum call_status failure;
      refusal = check_working_chain(directory, &failure);
      if (failure != CALL_OK) {
        report_status(path, failure);
      } else if (refusal) {
        report_text(path, refusal);
      } else if (empty_directory(directory, target, 0)) {
        status = remove_path(target, DIRECTORY_KIND_DIRECTORY);
        removed = status == CALL_OK;
        if (!removed) {
          report_status(path, status);
          report_text(path, "Contents were removed, but the directory itself was not");
        }
      } else {
        report_text(path, "Not removed: some contents could not be removed");
      }
      handle_close(directory);
    }
  } else if (!removed) {
    report_status(path, status);
  }
  free(target);
  return removed;
}

int main(int argc, char **argv)
{
  bool recursive = false;
  int first = 1;
  for (; first < argc; ++first) {
    if (!strcmp(argv[first], "--")) {
      ++first;
      break;
    }
    if (!strcmp(argv[first], "-r")) {
      recursive = true;
    } else if (argv[first][0] == '-' && argv[first][1]) {
      fprintf(stderr, "rm: unknown option %s\n", argv[first]);
      return usage();
    } else {
      break;
    }
  }
  if (first >= argc) {
    return usage();
  }
  int result = EXIT_SUCCESS;
  for (int i = first; i < argc && !output_failed; ++i) {
    bool removed;
    if (recursive) {
      removed = remove_tree_operand(argv[i]);
    } else {
      enum call_status status = remove_path(argv[i], DIRECTORY_KIND_FILE);
      removed = status == CALL_OK;
      if (!removed) {
        report_status(argv[i], status);
      }
    }
    if (!removed) {
      result = EXIT_FAILURE;
    }
  }
  return output_failed ? EXIT_FAILURE : result;
}
