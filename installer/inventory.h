#ifndef USERSPACE_INSTALLER_INVENTORY_H
#define USERSPACE_INSTALLER_INVENTORY_H

#include "installer.h"
#include "manifest.h"

enum install_prior_state {
  INSTALL_PRIOR_EMPTY,
  INSTALL_PRIOR_MANIFEST,
  INSTALL_PRIOR_SYNTHESIZED,
  INSTALL_PRIOR_UNKNOWN,
};

struct install_prior {
  enum install_prior_state state;
  bool needs_legacy;
  const char *reason;
  struct install_manifest inventory;
};

/* Inputs are borrowed. UPDATE supplies an admitted layout and the inspected
 * ESP revision. Reads use no mounts, repairs or persistent replay. A legacy
 * scan requires an explicit choice, including when revalidating at apply.
 * False reports I/O, allocation, capacity or structural inspection failure;
 * UNKNOWN is a successful inspection with no trustworthy comparison. */
bool install_prior_read(const struct install_disk *disk,
    const struct install_update *update, bool inventory_legacy,
    struct install_prior *prior);
void install_prior_destroy(struct install_prior *prior);

#endif
