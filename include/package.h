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

bool package_id_valid(const char *id);

/* Check current and its set record through PACKAGES, a directory with LOOKUP
 * and READ_FILES on the volume root. A missing current is an empty set.
 * Returns CALL_OK with the set's package count, or a failure with *reason
 * naming what is damaged (static text). Reads at most two small files. */
enum call_status package_set_check(handle_t packages, size_t *count, const char **reason);

/* Open package ID's tree in the current set with RIGHTS, a subset of
 * PACKAGE_TREE_RIGHTS. CALL_NOT_FOUND means the set doesn't list ID or its
 * store tree is missing; other failures mean a damaged current or set record.
 * Success owns *tree; failure clears it and sets *reason (static text). The
 * set is read anew on each call, so a switch is seen by the next call. */
enum call_status package_open_tree(handle_t packages, const char *id, uint64_t rights,
    handle_t *tree, const char **reason);

#endif
