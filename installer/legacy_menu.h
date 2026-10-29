#ifndef INSTALLER_LEGACY_MENU_H
#define INSTALLER_LEGACY_MENU_H

#include "boot_entry.h"

/* Read-only adoption of the owner's exact 0.0.9 GUID-bound menu form, with
 * its optional 0.0.8 previous entry. EXPECTED_REVISION comes from the selected
 * installed pool, never the new installer revision. An unrecorded pool reports
 * unknown; only full legacy manifest/payload proof can then supply the revision.
 * Every referenced attempt
 * needs its canonical five-file namespace, complete four-payload manifest,
 * full hashes and exact GUID-bound entry.txt/revision.txt contents.
 *
 * Fresh initialized output blobs are owned by the caller. An unrelated menu
 * succeeds with ADOPTED false; a recognizable but edited/ambiguous Pyxis menu
 * refuses. Adoption replaces only the exact normal/rescue entries, retaining
 * all other bytes, including the verified previous entry. EVIDENCE contains
 * the length-delimited manifests consumed by this plan. Freeze/hash it with
 * SELECTED, and repeat this helper against writable admission's checked view,
 * comparing both outputs before any mutation. No ownership marker or whole
 * menu digest substitutes for these checks. Keep the shared hash context
 * ready; WORKSPACE is a borrowed private native RAM directory. No ESP writes,
 * retained handles, firmware changes or admission authority are granted. */
bool install_legacy_menu_plan(handle_t root, handle_t workspace,
    const struct config_target_facts *facts, uint64_t pool_partition,
    const struct config_blob *current, const char *expected_revision,
    const struct install_boot_entry_paths *next, struct config_blob *selected,
    struct config_blob *evidence, bool *adopted, const char **reason);

#endif
