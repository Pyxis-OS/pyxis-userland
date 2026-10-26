#ifndef USERSPACE_PKG_UPGRADE_H
#define USERSPACE_PKG_UPGRADE_H

#include <abi/handle.h>
#include <stdbool.h>
#include <stdint.h>

/* boot-init's boot evidence (docs/userland/init.md): what this boot loaded,
 * the committed boot record it read, and the boot entry's mount.pool. */
struct pkg_boot_evidence {
  uint64_t kernel_size, archive_size, record_size;
  char kernel_sha256[65], archive_sha256[65], record_sha256[65];
  uint32_t pool;
};

bool pkg_boot_evidence_read(handle_t file, struct pkg_boot_evidence *evidence,
    const char **reason);

/* Remove NAME under PARENT and everything below it; absent is success. */
bool pkg_upgrade_clear(handle_t parent, const char *name);

/* One release's payloads, staged and verified by the caller, published as the
 * next boot attempt through the shared boot publisher under the one-use
 * UPGRADE_BOOT_ESP admission (docs/wip/pkg-upgrade.md, step 3). The running
 * attempt becomes the previous menu entry. WORKSPACE is a RAM directory.
 * Handles are borrowed. PSA crypto must be initialized. */
struct pkg_upgrade_publish {
  handle_t issuer, clock, random, workspace, packages;
  handle_t kernel, archive;
  uint64_t kernel_size, archive_size;
  uint64_t from_set, to_set;
  const char *revision;
  const struct pkg_boot_evidence *evidence;
};

bool pkg_upgrade_publish(const struct pkg_upgrade_publish *request, const char **reason);

#endif
