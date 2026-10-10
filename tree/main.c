#include "../ls/ls.h"
#include <pyxis/stdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Deeper trees are reported, not followed: each level holds its listing. */
#define TREE_DEPTH_LIMIT 64
#define INDENT_WIDTH 4

struct tree {
  struct ls_output output;
  size_t levels; /* Levels to show below an operand; SIZE_MAX for all. */
  size_t directories, files;
  bool failed;
  char prefix[TREE_DEPTH_LIMIT * INDENT_WIDTH + 1];
};

static int usage(void)
{
  fprintf(stderr, "usage: tree [-L levels] [--] [directory...]\n");
  return EXIT_FAILURE;
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

/* Prints the entries of PATH, then each subdirectory beneath its own line.
 * A directory that cannot be listed still appears; the failure is reported
 * and the walk continues. Returns -1 only when output fails. */
static int walk(struct tree *tree, const char *path, size_t depth)
{
  struct ls_listing listing = {0};
  bool loaded;
  if (ls_load_listing(path, &tree->output, &listing, &loaded) != 0) {
    tree->failed = true;
  }
  int result = 0;
  for (size_t i = 0; loaded && i < listing.count && result == 0; ++i) {
    const struct ls_entry *entry = &listing.entries[i];
    bool last = i + 1 == listing.count;
    bool directory = entry->kind == LS_DIRECTORY;
    if (fputs(tree->prefix, stdout) == EOF || fputs(last ? "`-- " : "|-- ", stdout) == EOF ||
        ls_print_name(entry, tree->output.terminal) != 0 || fputc('\n', stdout) == EOF) {
      result = -1;
      break;
    }
    if (!directory) {
      ++tree->files;
      continue;
    }
    ++tree->directories;
    if (depth + 1 >= tree->levels) {
      continue;
    }
    if (depth + 1 >= TREE_DEPTH_LIMIT) {
      fprintf(stderr, "tree: %s/%s: Deeper than %d levels; not listed\n", path, entry->name,
          TREE_DEPTH_LIMIT);
      tree->failed = true;
      continue;
    }
    char *child = join(path, entry->name);
    if (!child) {
      fprintf(stderr, "tree: %s/%s: Out of memory\n", path, entry->name);
      tree->failed = true;
      continue;
    }
    size_t used = strlen(tree->prefix);
    memcpy(tree->prefix + used, last ? "    " : "|   ", INDENT_WIDTH + 1);
    result = walk(tree, child, depth + 1);
    tree->prefix[used] = '\0';
    free(child);
  }
  ls_free_listing(&listing);
  return result;
}

static bool parse_levels(const char *text, size_t *levels)
{
  char *end;
  unsigned long value = strtoul(text, &end, 10);
  if (!*text || *end || value < 1 || value > TREE_DEPTH_LIMIT) {
    return false;
  }
  *levels = value;
  return true;
}

int main(int argc, char **argv)
{
  struct tree tree = {.output = {.program = "tree"}, .levels = SIZE_MAX};
  bool options = true;
  int operands = 0;
  for (int i = 1; i < argc; ++i) {
    const char *argument = argv[i];
    if (options && !strcmp(argument, "--")) {
      options = false;
    } else if (options && argument[0] == '-' && argument[1]) {
      const char *value = NULL;
      if (argument[1] == 'L') {
        value = argument[2] ? argument + 2 : (i + 1 < argc ? argv[++i] : NULL);
      }
      if (argument[1] != 'L') {
        fprintf(stderr, "tree: unknown option: %s\n", argument);
        return usage();
      }
      if (!value || !parse_levels(value, &tree.levels)) {
        fprintf(stderr, "tree: -L needs a number of levels from 1 to %d\n", TREE_DEPTH_LIMIT);
        return EXIT_FAILURE;
      }
    } else {
      argv[++operands] = argv[i];
    }
  }

  struct startup_stream binding;
  if (pyxis_stdio_stream(stdout, &binding) != 0) {
    perror("tree: stdout");
    return EXIT_FAILURE;
  }
  tree.output.terminal = binding.protocol == PROTOCOL_CONSOLE;

  int roots = operands ? operands : 1;
  for (int i = 0; i < roots; ++i) {
    const char *path = operands ? argv[i + 1] : ".";
    if (ls_print_text(path, tree.output.terminal) != 0 || fputc('\n', stdout) == EOF ||
        walk(&tree, path, 0) != 0) {
      perror("tree: stdout");
      return EXIT_FAILURE;
    }
  }
  if (printf("\n%zu %s, %zu %s\n", tree.directories,
      tree.directories == 1 ? "directory" : "directories", tree.files,
      tree.files == 1 ? "file" : "files") < 0 || fflush(stdout) == EOF) {
    perror("tree: stdout");
    return EXIT_FAILURE;
  }
  return tree.failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
