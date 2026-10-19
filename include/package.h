#ifndef USERSPACE_PACKAGE_H
#define USERSPACE_PACKAGE_H

#include <abi/directory.h>
#include <abi/syscall.h>
#include <stdbool.h>
#include <stddef.h>

/* The packages volume (docs/interfaces/packages.md): store/ID/VERSION/tree,
 * sets/N.lua and a current file naming the active set. */
#define PACKAGE_SOURCE "packages"
#define PACKAGE_ID_MAX 64
#define PACKAGE_VERSION_MAX 64
/* Rights a package tree may carry: packages are always read-only. */
#define PACKAGE_TREE_RIGHTS (DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_ENUMERATE | \
                             DIRECTORY_RIGHT_READ_FILES)

struct package_set;

bool package_id_valid(const char *id);

/* Read current and its set record through PACKAGES, a directory with LOOKUP
 * and READ_FILES on the volume root. A missing current is the empty set.
 * Success owns *set (free it with package_set_free); failure clears it and
 * sets *reason (static text) naming what is damaged. One load serves every
 * package of one launch or space start, so they all come from one set. */
enum call_status package_set_load(handle_t packages, struct package_set **set,
    const char **reason);
/* Explicit record for management/recovery; number zero is the empty set.
 * Does not inspect current or select a fallback. */
enum call_status package_set_load_number(handle_t packages, uint64_t number,
    struct package_set **set, const char **reason);
uint64_t package_set_number(const struct package_set *set);
/* Borrowed strings remain valid until set is freed. */
bool package_set_entry(const struct package_set *set, size_t index,
    const char **id, const char **version);
size_t package_set_count(const struct package_set *set);
void package_set_free(struct package_set *set);

/* Open package ID's tree from SET with RIGHTS, a subset of PACKAGE_TREE_RIGHTS.
 * CALL_NOT_FOUND means SET doesn't list ID or its store tree is missing.
 * Success owns *tree; failure clears it and sets *reason (static text). */
enum call_status package_set_open(const struct package_set *set, handle_t packages,
    const char *id, uint64_t rights, handle_t *tree, const char **reason);

#endif
