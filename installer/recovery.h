#ifndef INSTALLER_RECOVERY_H
#define INSTALLER_RECOVERY_H

#include "publication_snapshot.h"

struct install_recovery {
  struct config_blob manifest, fallback;
  struct config_file_digest kernel, archive;
  struct install_source sources[2];
  struct config_target_facts facts;
  bool present;
};

/* Fresh output only; keep the shared hash context initialized across preparation
 * and provision. Sources[0] borrows the immutable kernel FILE; sources[1] owns
 * the archive FILE. Destroy even after failed preparation.
 * Frozen read-only Review requires exact canonical names, a GUID-bound manifest,
 * both full payload hashes and an unchanged fallback before reusing recovery.
 * A wholly absent recovery directory proposes first provision; any incomplete or
 * unrecognized namespace, foreign fallback or an EFI/BOOT or EFI/pyxis config shadow refuses.
 * Workspace borrows a private native RAM parent; existing payload readback uses
 * an unlinked bulk-transfer FILE, closed before returning on every outcome. */
bool install_recovery_prepare(handle_t root, handle_t boot, handle_t loaded_kernel,
    handle_t workspace, const struct config_target_facts *facts, struct install_recovery *recovery);
/* Destructive Classic planning only: inspect immutable sources and render the
 * reviewed planned GUIDs, without opening an ESP. Planned facts remain data;
 * valid=false/domain=0 are preserved. Caller revalidates the reviewed layout and
 * replaces facts with actual native issuer facts before provision. */
bool install_recovery_prepare_fresh(handle_t boot, handle_t loaded_kernel,
    const struct config_target_facts *planned, struct install_recovery *recovery);
/* Creates only a wholly new immutable directory and a previously absent static
 * fallback, in that order. Reopens and hashes the payloads before manifest and
 * fallback publication. Existing recovery is verified, never rewritten. Errors
 * retain partial artifacts; no automatic retry, repair, deletion or replacement. */
bool install_recovery_provision(handle_t root, handle_t control, handle_t workspace,
    const struct install_recovery *recovery);
uint64_t install_recovery_space(const struct install_recovery *recovery, uint64_t cluster);
void install_recovery_destroy(struct install_recovery *recovery);
/* The canonical primary is reviewed separately by the boot-entry planner.
 * Other entries require the record's previous/obsolete menu names and
 * matching bytes, or a committed transaction's exact staging name and the full
 * menu reconstructed from its boot binding and verified inherited menu bytes.
 * Only actual absence permits creating a missing menu directory. POOL_PARTITION
 * is the menu's mount.pool=. */
bool install_recovery_menu_review(handle_t root, const struct config_blob *record,
    const struct config_target_facts *facts, uint64_t pool_partition);

/* New publication refuses retained recorded cleanup; repeat this check at
 * writable admission before any program or recovery mutation. */
bool install_recovery_resolved(handle_t root, const struct config_blob *record);

#endif
