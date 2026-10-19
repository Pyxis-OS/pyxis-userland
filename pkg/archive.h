#ifndef USERSPACE_PKG_ARCHIVE_H
#define USERSPACE_PKG_ARCHIVE_H

#include "metadata.h"

/* Poll pending cancellation at bounded read boundaries. */
bool pkg_cancelled(void);

/* snapshot is one completed immutable provider FILE retained through this call.
 * destination is a fresh exclusively created directory, or HANDLE_INVALID for
 * verification only. Passed handles remain borrowed. A successful output owns
 * its dependencies; failure clears it. Failed extraction leaves staging only. */
bool pkg_archive(handle_t snapshot, const struct pkg_entry *entry,
    handle_t destination, struct pkg_manifest *manifest, const char **reason);

/* Verify an installed package, including surplus namespace entries. expected
 * optionally requires the same manifest/inventory before immutable-tree reuse.
 * directory and expected are borrowed; output owns its dependencies. */
bool pkg_tree_verify(handle_t directory, const struct pkg_manifest *expected,
    struct pkg_manifest *manifest, const char **reason);

#endif
