#ifndef INSTALLER_PUBLICATION_SNAPSHOT_H
#define INSTALLER_PUBLICATION_SNAPSHOT_H

#include "installer.h"
#include "../config-editor/storage.h"

/* Read-only snapshots through the native planning view. All returned bytes are
 * owned; no handle is retained. Missing parent/files are explicit absence. */
bool install_snapshot_file(handle_t root, const char *path, size_t limit,
    struct config_blob *blob);
bool install_snapshot_facts(handle_t control, struct config_target_facts *facts);
bool install_snapshot_same_target(const struct config_target_facts *left,
    const struct config_target_facts *right);
/* Recheck frozen inputs under the initial whole-disk claim, using the shared
 * FAT reader. Never mount or mutate while the raw claim is held. */
bool install_snapshot_revalidate(const struct install_disk *disk,
    const struct config_target_facts *facts, const struct config_blob current[CONFIG_ALL_FILES],
    const struct config_blob *record);
bool install_snapshot_payload_revalidate(const struct install_disk *disk,
    const struct config_target_facts *facts, const char *path,
    const struct config_file_digest *digest);
bool install_pool_boot_snapshot(const struct install_disk *disk, struct config_blob *blob);

#endif
