#ifndef USERSPACE_INSTALLER_PENDING_ARCHIVE_H
#define USERSPACE_INSTALLER_PENDING_ARCHIVE_H

#include "pending.h"

/* Freeze the four bounded config members of the actual newc boot archive.
 * FILES starts with {0}; caller owns and frees every resulting blob, including
 * partial output on failure. No FILE or namespace is changed. */
bool install_pending_archive_configs(const struct install_source *source,
    struct config_blob files[CONFIG_SET_FILES]);

/* Internal storage boundary: validate the committed selection against frozen
 * defaults/catalog extracted from its actual retained READ-only boot archive.
 * No source, FILE or namespace is mutated; extracted bytes never escape. */
bool install_pending_selection(const struct install_pending_request *request,
    const struct install_pending_plan *plan, const struct install_esp_payloads *payloads);

#endif
