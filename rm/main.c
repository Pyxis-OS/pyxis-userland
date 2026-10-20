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
/* Consecutive CHANGED outcomes, with no removal between them, before a directory
 * is given up on. */
#define RM_CHANGED_LIMIT 8
#define RM_NAME_CAPACITY 128
/* No directory backend reports a longer name; a larger claim is a protocol error. */
#define RM_NAME_LIMIT 65536
/* A child's rights are bounded by its parent's, so the operand's parent holds
 * the same rights as every directory the walk enters. */
#define RM_RIGHTS (DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_ENUMERATE | DIRECTORY_RIGHT_REMOVE)

struct entry {
  char *name;
  uint64_t kind;
};

struct batch {
  struct entry entries[RM_BATCH];
  size_t count;
};

/* Names this run could not remove from one directory, or that it was told were
 * already gone. Later passes must not select them again. */
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

enum removal {
  REMOVED,
  MISSING,
  FAILED,
};

enum same {
  SAME_NO,
  SAME_YES,
  SAME_UNKNOWN,
};

/* The final component of an operand, located inside the text as given. */
struct operand {
  const char *text;
  size_t name_start;
  size_t name_end;
  bool directory_only; /* A trailing / requires a directory. */
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

/* Fills the batch from the cursor, ignoring skipped names, and leaves the cursor
 * after the last entry examined. Nothing is removed during a pass, so CHANGED
 * can only come from another process. */
static enum collect collect_batch(handle_t directory, const char *path,
    const struct skipped *skipped, struct directory_cursor *cursor, struct batch *batch)
{
  size_t capacity = RM_NAME_CAPACITY;
  char *name = malloc(capacity);
  if (!name) {
    report_status(path, CALL_NO_MEMORY);
    return COLLECT_ERROR;
  }
  enum collect result = COLLECT_ERROR;
  for (;;) {
    struct directory_enumerate_reply reply;
    enum call_status status = directory_enumerate(directory, cursor, name, capacity, &reply);
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
      /* The retry must make progress, or a faulty backend would spin it forever. */
      if (reply.name_size <= capacity || reply.name_size > RM_NAME_LIMIT) {
        report_text(path, "Directory reported an invalid name size");
        break;
      }
      char *larger = realloc(name, reply.name_size);
      if (!larger) {
        report_status(path, CALL_NO_MEMORY);
        break;
      }
      name = larger;
      capacity = reply.name_size;
      continue; /* A short buffer leaves the cursor unchanged. */
    }
    if (reply.outcome != DIRECTORY_ENTRY || !reply.name_size || reply.name_size > capacity ||
        name[reply.name_size - 1] != '\0') {
      report_text(path, "Directory returned an invalid entry");
      break;
    }
    *cursor = reply.cursor;
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
 * so a directory that is one of them is refused, and so is one that cannot be
 * told apart from them. Every directory the walk enters is checked, not only the
 * operand, since a root bound inside the tree can lead to the chain. Returns
 * NULL when the directory is safe to empty. */
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

static bool empty_directory(handle_t directory, const char *path, size_t level);

/* Enters one directory by lookup from its held parent, refuses the current
 * directory chain, empties it, then removes its name. Everything below the
 * lookup uses held handles. A directory that could not be emptied is left in
 * place, and so are its ancestors. The final removal is by name: the native
 * interface has no condition that the name still means the emptied object. */
static enum removal remove_subdirectory(handle_t parent, const char *name, const char *path,
    size_t level)
{
  if (level > RM_DEPTH_LIMIT) {
    char message[64];
    snprintf(message, sizeof(message), "Nested deeper than %d levels; not removed",
        RM_DEPTH_LIMIT);
    report_text(path, message);
    return FAILED;
  }
  handle_t child;
  enum call_status status = directory_lookup(parent, name, DIRECTORY_KIND_DIRECTORY,
      RM_RIGHTS, &child);
  if (status == CALL_NOT_FOUND) {
    return MISSING;
  }
  if (status != CALL_OK) {
    report_status(path, status);
    return FAILED;
  }
  enum call_status failure;
  const char *refusal = check_working_chain(child, &failure);
  if (failure != CALL_OK) {
    report_status(path, failure);
    handle_close(child);
    return FAILED;
  }
  if (refusal) {
    report_text(path, refusal);
    handle_close(child);
    return FAILED;
  }
  /* Failures below were reported where they happened. The operand alone names
   * the outcome, so the directories between are silent. */
  bool emptied = empty_directory(child, path, level + 1);
  handle_close(child);
  if (!emptied) {
    if (level == 1) {
      report_text(path, "Not removed: some contents could not be removed");
    }
    return FAILED;
  }
  status = directory_remove(parent, name, DIRECTORY_KIND_DIRECTORY);
  if (status == CALL_NOT_FOUND) {
    return MISSING;
  }
  if (status != CALL_OK) {
    char *note = malloc(strlen(path) + sizeof(" (contents removed)"));
    if (note) {
      strcpy(note, path);
      strcat(note, " (contents removed)");
    }
    report_status(note ? note : path, status);
    free(note);
    return FAILED;
  }
  return REMOVED;
}

/* Removes a name that may be a file or a directory: REMOVE as a file first,
 * then a directory lookup if that says it is not one. Neither follows a link. */
static enum removal remove_either(handle_t directory, const char *name, const char *path,
    size_t level)
{
  enum call_status status = directory_remove(directory, name, DIRECTORY_KIND_FILE);
  if (status == CALL_OK) {
    return REMOVED;
  }
  if (status == CALL_NOT_FOUND) {
    return MISSING;
  }
  if (status == CALL_WRONG_TYPE) {
    return remove_subdirectory(directory, name, path, level);
  }
  report_status(path, status);
  return FAILED;
}

static enum removal remove_entry(handle_t directory, const char *parent,
    const struct entry *entry, size_t level)
{
  char *path = join(parent, entry->name);
  if (!path) {
    report_status(parent, CALL_NO_MEMORY);
    return FAILED;
  }
  enum removal result = FAILED;
  if (entry->kind == DIRECTORY_KIND_FILE) {
    enum call_status status = directory_remove(directory, entry->name, DIRECTORY_KIND_FILE);
    result = status == CALL_OK ? REMOVED : status == CALL_NOT_FOUND ? MISSING : FAILED;
    if (result == FAILED) {
      report_status(path, status);
    }
  } else if (entry->kind == DIRECTORY_KIND_DIRECTORY) {
    result = remove_subdirectory(directory, entry->name, path, level);
  } else if (entry->kind == DIRECTORY_KIND_UNKNOWN) {
    result = remove_either(directory, entry->name, path, level);
  } else {
    report_text(path, "Symbolic link or special entry; not removed");
  }
  free(path);
  return result;
}

/* Removes every entry of an open directory. Returns false if any entry
 * remains, after reporting each reason. LEVEL is the depth of the directory's
 * children, counting the operand as 1.
 *
 * A pass that removes something invalidates the cursor, so the next pass starts
 * again from the beginning. A pass that removes nothing leaves the cursor valid
 * and continues from it. The directory is finished when a whole traversal
 * removes nothing. */
static bool empty_directory(handle_t directory, const char *path, size_t level)
{
  struct skipped skipped = {0};
  struct directory_cursor cursor = {0};
  bool complete = true;
  size_t changed = 0;
  while (!output_failed) {
    struct batch batch = {0};
    enum collect collected = collect_batch(directory, path, &skipped, &cursor, &batch);
    if (collected == COLLECT_ERROR) {
      free_batch(&batch);
      complete = false;
      break;
    }
    if (collected == COLLECT_CHANGED) {
      free_batch(&batch);
      cursor = (struct directory_cursor){0};
      if (++changed > RM_CHANGED_LIMIT) {
        report_text(path, "Directory keeps changing; not emptied");
        complete = false;
        break;
      }
      continue;
    }
    size_t removed = 0;
    for (size_t i = 0; i < batch.count && !output_failed; ++i) {
      const struct entry *entry = &batch.entries[i];
      enum removal result = remove_entry(directory, path, entry, level);
      if (result == REMOVED) {
        ++removed;
        continue;
      }
      /* A missing name that the listing still offers must not be offered again. */
      if (result == FAILED) {
        complete = false;
      }
      if (!skip_name(&skipped, entry->name)) {
        report_status(path, CALL_NO_MEMORY);
        output_failed = true;
      }
    }
    free_batch(&batch);
    if (removed) {
      changed = 0;
      cursor = (struct directory_cursor){0};
    } else if (collected == COLLECT_DONE) {
      break;
    }
  }
  free_skipped(&skipped);
  return complete && !output_failed;
}

/* Locates the final component and refuses operands that name a root, the
 * current directory or its parent before anything is touched. The text is used
 * as given: trailing separators only make the operand directory-only. */
static const char *parse_operand(const char *text, struct operand *operand)
{
  const char *scheme = strstr(text, "://");
  const char *rest = scheme ? scheme + 3 : text;
  size_t length = strlen(rest);
  size_t end = length;
  while (end && rest[end - 1] == '/') {
    --end;
  }
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
  size_t offset = (size_t)(rest - text);
  *operand = (struct operand){
    .text = text, .name_start = offset + start, .name_end = offset + end,
    .directory_only = end < length,
  };
  return NULL;
}

/* Resolves the operand's parent once, with the walk's rights, and works from
 * that held handle. Operands
 * that are not native paths, such as provider routes, have no parent directory
 * and fall back to plain removal. */
static bool remove_operand(const char *text)
{
  if (!*text) {
    report_status(text, CALL_BAD_REQUEST);
    return false;
  }
  struct operand operand;
  const char *refusal = parse_operand(text, &operand);
  if (refusal) {
    report_text(text, refusal);
    return false;
  }
  char *parent_path = strndup(text, operand.name_start);
  char *name = strndup(text + operand.name_start, operand.name_end - operand.name_start);
  if (!parent_path || !name) {
    free(parent_path);
    free(name);
    report_status(text, CALL_NO_MEMORY);
    return false;
  }
  handle_t parent;
  enum call_status status = resolve_directory(*parent_path ? parent_path : ".",
      RM_RIGHTS, &parent);
  bool removed = false;
  if (status == CALL_BAD_OPERATION) {
    status = remove_path(text, DIRECTORY_KIND_FILE);
    removed = status == CALL_OK;
    if (!removed) {
      report_status(text, status);
    }
  } else if (status != CALL_OK) {
    report_status(text, status);
  } else {
    enum removal result = MISSING;
    status = CALL_OK;
    if (!operand.directory_only) {
      status = directory_remove(parent, name, DIRECTORY_KIND_FILE);
      result = status == CALL_OK ? REMOVED : status == CALL_NOT_FOUND ? MISSING : FAILED;
    }
    if (operand.directory_only || status == CALL_WRONG_TYPE) {
      result = remove_subdirectory(parent, name, text, 1);
    } else if (result == FAILED) {
      report_status(text, status);
    }
    if (result == MISSING) {
      report_status(text, CALL_NOT_FOUND);
    }
    removed = result == REMOVED;
    handle_close(parent);
  }
  free(parent_path);
  free(name);
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
      removed = remove_operand(argv[i]);
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
