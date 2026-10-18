#ifndef BOOT_INIT_SELECTION_H
#define BOOT_INIT_SELECTION_H

#include "../libconfig/handoff.h"

struct boot_selection_authority {
  handle_t boot, esp_mount, ram, system_info, clock, random;
  handle_t loaded_kernel, loaded_archive;
};

/* Borrow trusted boot-init grants. Success owns the complete frozen selection
 * and READ-only handoff FILEs; the early ESP instance is already retired.
 * Payload FILEs must come directly from the kernel's private bootstrap grants;
 * the caller closes them before effects. Rescue and unavailable or incompatible
 * readiness select archive defaults. No payload is reopened by pathname. */
bool boot_selection_prepare(const struct boot_selection_authority *authority,
    bool rescue, struct config_selection *selection, struct config_handoff *handoff);

#endif
