#ifndef USERSPACE_PKG_ARCHIVE_H
#define USERSPACE_PKG_ARCHIVE_H

#include "metadata.h"

enum pkg_archive_phase {
  PKG_ARCHIVE_VERIFY,
  PKG_ARCHIVE_EXTRACT,
};

/* Optional observer. VERIFY counts bytes of the whole archive hashed; EXTRACT
 * counts inventory files written, and is reported only when extracting. Each
 * phase starts with done == 0 and a successful phase ends with done == total. */
struct pkg_archive_progress {
  void (*update)(void *context, enum pkg_archive_phase phase, uint64_t done, uint64_t total);
  void *context;
};

/* snapshot is one completed immutable provider FILE retained through this call.
 * destination is a fresh exclusively created directory, or HANDLE_INVALID for
 * verification only. Passed handles remain borrowed. A successful output owns
 * its dependencies; failure clears it. Failed extraction leaves staging only.
 * progress may be NULL. */
bool pkg_archive(handle_t snapshot, const struct pkg_entry *entry,
    handle_t destination, struct pkg_manifest *manifest,
    const struct pkg_archive_progress *progress, const char **reason);

/* An installed package's inventory, read one file path at a time in its
 * sorted order. */
struct pkg_inventory {
  handle_t file;
  uint64_t position, end;
  unsigned char buffer[4096];
  size_t used, available;
};

/* directory is the installed package's store/ID/VERSION directory, borrowed.
 * Close an opened inventory with pkg_inventory_close. */
bool pkg_inventory_open(handle_t directory, struct pkg_inventory *inventory, const char **reason);
/* The next path; false at the end with *reason NULL, or on damage with a reason. */
bool pkg_inventory_next(struct pkg_inventory *inventory, char path[PKG_PATH_MAX + 1],
    const char **reason);
void pkg_inventory_close(struct pkg_inventory *inventory);

/* Read only an installed package's manifest, unverified, for checks that can
 * refuse before the full verification. output owns its dependencies. */
bool pkg_tree_manifest(handle_t directory, struct pkg_manifest *manifest, const char **reason);

/* Verify an installed package, including surplus namespace entries. expected
 * optionally requires the same manifest/inventory before immutable-tree reuse.
 * directory and expected are borrowed; output owns its dependencies. */
bool pkg_tree_verify(handle_t directory, const struct pkg_manifest *expected,
    struct pkg_manifest *manifest, const char **reason);

#endif
