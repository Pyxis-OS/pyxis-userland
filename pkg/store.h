#ifndef PKG_STORE_H
#define PKG_STORE_H

#include "metadata.h"
#include <directory.h>

#define PKG_READ_RIGHTS (DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_ENUMERATE | \
    DIRECTORY_RIGHT_READ_FILES)
#define PKG_WRITE_RIGHTS (PKG_READ_RIGHTS | DIRECTORY_RIGHT_CREATE | \
    DIRECTORY_RIGHT_REMOVE | DIRECTORY_RIGHT_WRITE_FILES)
#define PKG_LOCK ".writer-lock"

struct pkg_selection {
  uint64_t number;
  struct pkg_identity *entries;
  size_t count, capacity;
};

bool pkg_directory(handle_t parent, const char *name, bool create, uint64_t rights,
    handle_t *directory, const char **reason);
bool pkg_text(handle_t parent, const char *name, size_t limit, char **bytes,
    size_t *size, const char **reason);
bool pkg_write_text(handle_t parent, const char *name, const void *bytes, size_t size,
    const char **reason);
bool pkg_selection_load(handle_t packages, bool current, uint64_t number,
    struct pkg_selection *selection, const char **reason);
void pkg_selection_free(struct pkg_selection *selection);
bool pkg_predecessor(handle_t packages, uint64_t number, uint64_t *previous,
    const char **reason);
bool pkg_publish(handle_t packages, const struct pkg_selection *selection,
    uint64_t previous, bool *uncertain, const char **reason);
bool pkg_tree_open(handle_t packages, const struct pkg_identity *identity,
    uint64_t rights, handle_t *tree, const char **reason);

#endif
