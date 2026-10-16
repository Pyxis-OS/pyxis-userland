#ifndef USERSPACE_CONFIG_EDITOR_READINESS_H
#define USERSPACE_CONFIG_EDITOR_READINESS_H

#include "record.h"
#include "../libconfig/selection.h"

/* Inputs are frozen owned snapshots borrowed for this call. The trusted early
 * reader supplies provenance, actually loaded revisions and exact payload
 * digests obtained from the immutable boot FILEs; caller-authored
 * claims are not boot authority. Hashing requires the shared PSA context.
 * No FILE handles, writer authority, effects or boot-init activation occur.
 * Unknown/prepared/unbound/mismatching readiness and rescue select the entire
 * archive set. Missing loaded-payload evidence also refuses ESP selection.
 * A bound archive record never certifies or decodes ESP inputs.
 * Selection owns its resulting byte snapshots and decoded values. */
bool config_readiness_choose(const struct config_bytes archive[CONFIG_LUA_COUNT],
    const struct config_bytes *archive_catalog, const char *kernel_revision,
    const char *program_revision, const struct config_file_digest *loaded_kernel,
    const struct config_file_digest *loaded_archive, const struct config_bytes *completion,
    const struct config_bytes esp[CONFIG_LUA_COUNT], const struct config_bytes *esp_catalog,
    bool provenance_valid, bool rescue, struct config_selection *selection,
    const char **reason);

#endif
