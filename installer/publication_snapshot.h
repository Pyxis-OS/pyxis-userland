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
/* Count actual free FAT clusters, never FSInfo hints. The caller keeps the
 * source quiescent through its RO planning mount or initial raw claim. This
 * grants no reservation; repeat against the admitted target before writes. */
bool install_snapshot_capacity(const struct install_disk *disk,
    const struct config_target_facts *facts, uint64_t *free_bytes, uint64_t *cluster_bytes);
bool install_snapshot_payload_revalidate(const struct install_disk *disk,
    const struct config_target_facts *facts, const char *path,
    const struct config_file_digest *digest);

#endif
