#ifndef USERSPACE_INSTALLER_PENDING_ARCHIVE_H
#define USERSPACE_INSTALLER_PENDING_ARCHIVE_H

#include "pending.h"

/* Internal storage boundary: validate the committed selection against frozen
 * defaults/catalog extracted from its actual retained READ-only boot archive.
 * No source, FILE or namespace is mutated; extracted bytes never escape. */
bool install_pending_selection(const struct install_pending_request *request,
    const struct install_pending_plan *plan, const struct install_esp_payloads *payloads);

#endif
