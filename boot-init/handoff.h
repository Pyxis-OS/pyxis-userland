#ifndef BOOT_INIT_HANDOFF_H
#define BOOT_INIT_HANDOFF_H

#include "../libconfig/handoff.h"

struct boot_evidence_authority {
  handle_t esp_mount, ram, clock, random;
  handle_t loaded_kernel, loaded_archive;
  uint32_t pool; /* The boot entry's mount.pool GPT entry. */
};

/* Borrow trusted boot-init grants. When the ESP holds a committed record and
 * the loaded payloads hash, EVIDENCE receives a READ-only FILE:
 * "PYXIS-BOOT-EVIDENCE 1", then kernel, archive and record lines of size and
 * SHA-256 and a pool line naming the boot entry's mount.pool, which pkg
 * upgrade writes into the new entry; otherwise it stays HANDLE_INVALID and boot
 * continues. The early ESP
 * reader is retired before return; false only when that retirement fails.
 * Payload FILEs come directly from the kernel's private bootstrap grants; the
 * caller closes them afterwards. */
bool boot_evidence_prepare(const struct boot_evidence_authority *authority, handle_t *evidence);

/* The pool's session and network configuration as READ-only handoff FILEs in
 * HANDOFF, made in a private RAM parent under RAM that is removed before return.
 * The bytes are borrowed and already validated. */
bool boot_configuration_handoff(handle_t ram, const struct config_handoff_bytes *session,
    const struct config_handoff_bytes *network, struct config_handoff *handoff,
    enum call_status *status);

#endif
