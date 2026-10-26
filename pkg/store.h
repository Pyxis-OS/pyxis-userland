#ifndef PKG_STORE_H
#define PKG_STORE_H

#include "metadata.h"
#include <directory.h>
#include <abi/package_commands.h>

#define PKG_READ_RIGHTS (DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_ENUMERATE | \
    DIRECTORY_RIGHT_READ_FILES)
#define PKG_WRITE_RIGHTS (PKG_READ_RIGHTS | DIRECTORY_RIGHT_CREATE | \
    DIRECTORY_RIGHT_REMOVE | DIRECTORY_RIGHT_WRITE_FILES)
#define PKG_LOCK ".writer-lock"

struct pkg_origin {
  char name[PACKAGE_ID_MAX + 1];
  char fingerprint[65];
  bool only_source; /* Review-only legacy adoption; not stored in set records. */
};

struct pkg_selection {
  uint64_t number;
  struct pkg_identity *entries;
  struct pkg_origin *origins;
  size_t count, capacity;
};

bool pkg_directory(handle_t parent, const char *name, bool create, uint64_t rights,
    handle_t *directory, const char **reason);
bool pkg_text(handle_t parent, const char *name, size_t limit, char **bytes,
    size_t *size, const char **reason);
bool pkg_write_text(handle_t parent, const char *name, const void *bytes, size_t size,
    const char **reason);
bool pkg_selection_load(handle_t packages,
    struct pkg_selection *selection, const char **reason);
void pkg_selection_free(struct pkg_selection *selection);
/* A set's command projection: commands command records followed by roots
 * root records, one per package with files. */
struct pkg_projection {
  struct package_command_record *records;
  size_t commands, roots;
};

/* A set's sdk index, the bytes of sets/N.sdk with a zero set number, which
 * publication fills in. */
struct pkg_sdk {
  uint8_t *bytes;
  size_t size;
};

bool pkg_publish(handle_t packages, const struct pkg_selection *selection,
    const struct pkg_projection *projection, const struct pkg_sdk *sdk,
    bool *uncertain, const char **reason);
/* Both of set NUMBER's indexes equal the ones built from its selection. */
bool pkg_indexes_verify(handle_t packages, uint64_t number,
    const struct pkg_projection *projection, const struct pkg_sdk *sdk, const char **reason);
/* Bootstrap the active set's missing indexes; an index already present is
 * neither checked nor repaired. No current or retention change. */
bool pkg_reindex(handle_t packages, uint64_t number,
    const struct pkg_projection *projection, const struct pkg_sdk *sdk, const char **reason);
bool pkg_tree_open(handle_t packages, const struct pkg_identity *identity,
    uint64_t rights, handle_t *tree, const char **reason);

#endif
