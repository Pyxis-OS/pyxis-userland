#ifndef USERSPACE_LS_H
#define USERSPACE_LS_H

#include <stddef.h>
#include <stdint.h>
#include <term.h>

enum ls_kind {
  LS_FILE,
  LS_DIRECTORY,
  LS_PROGRAM,
  LS_SCRIPT,
};

struct ls_entry {
  char *name;
  size_t width;
  enum ls_kind kind;
  uint64_t size;
  bool size_known;
};

/* Owns the entry array and every name; stores no directory or file handles. */
struct ls_listing {
  struct ls_entry *entries;
  size_t count;
  size_t capacity;
};

struct ls_output {
  const char *program; /* Diagnostic prefix; NULL means "ls". */
  bool terminal;
  bool one_per_line;
  bool long_listing;
  size_t columns;
};

/* loaded permits printing a complete enumeration even if file details failed.
 * An incomplete enumeration is never published. The caller always frees it. */
int ls_load_listing(const char *path, const struct ls_output *output,
    struct ls_listing *listing, bool *loaded);
void ls_free_listing(struct ls_listing *listing);
int ls_print_text(const char *text, bool terminal);
/* One name in its kind color, with the directory slash; no newline. */
int ls_print_name(const struct ls_entry *entry, bool terminal);
int ls_print_listing(const struct ls_listing *listing, const struct ls_output *output);

#endif
