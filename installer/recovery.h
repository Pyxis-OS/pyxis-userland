#ifndef INSTALLER_RECOVERY_H
#define INSTALLER_RECOVERY_H

#include "publication_snapshot.h"

#define INSTALL_RECOVERY_LEGACY_FALLBACK_NAME "legacy-fallback.conf"

struct install_recovery {
  const char *reason;
  struct config_blob manifest, fallback, legacy_fallback;
  struct config_file_digest kernel, archive;
  struct install_source sources[2];
  struct config_target_facts facts;
  bool present, legacy_migration;
};

/* Fresh output only; keep the shared hash context initialized across preparation
 * and provision. Sources[0] borrows the immutable kernel FILE; sources[1] owns
 * the archive FILE. Destroy even after failed preparation.
 * Frozen read-only Review requires exact canonical names, a GUID-bound manifest,
 * both full payload hashes and an unchanged fallback before reusing recovery.
 * A wholly absent recovery directory proposes first provision. ALLOW_LEGACY is
 * restricted by the installer to an unrecorded Coexist Update: it may adopt only
 * the exact selected-ESP 0.0.8 bootstrap rescue template and pinned payload hashes.
 * Its fallback bytes and original pair are retained. Existing canonical recovery
 * may contain the exact retained legacy-fallback.conf, verified with that pair.
 * An incomplete or unrecognized namespace, foreign fallback or config shadow refuses.
 * Workspace borrows a private native RAM parent; existing payload readback uses
 * an unlinked bulk-transfer FILE, closed before returning on every outcome. */
bool install_recovery_prepare(handle_t root, handle_t boot, handle_t loaded_kernel,
    handle_t workspace, const struct config_target_facts *facts, bool allow_legacy,
    struct install_recovery *recovery);
/* Destructive Classic planning only: inspect immutable sources and render the
 * reviewed planned GUIDs, without opening an ESP. Planned facts remain data;
 * valid=false/domain=0 are preserved. Caller revalidates the reviewed layout and
 * replaces facts with actual native issuer facts before provision. */
bool install_recovery_prepare_fresh(handle_t boot, handle_t loaded_kernel,
    const struct config_target_facts *planned, struct install_recovery *recovery);
/* Creates a wholly new immutable pair and manifest, then publishes its fallback.
 * A reviewed legacy migration also syncs/read-verifies a retained old fallback
 * and staged new fallback before a bounded BEGIN/CHECK/revalidation/rename/sync/END.
 * It never truncates the live fallback or deletes the legacy pair. Other existing
 * recovery is verified, never rewritten. Errors retain partial artifacts and do
 * not authorize retry, repair or deletion. CLOCK bounds the replacement interval. */
bool install_recovery_provision(handle_t root, handle_t control, handle_t workspace,
    handle_t clock, const struct install_recovery *recovery);
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
