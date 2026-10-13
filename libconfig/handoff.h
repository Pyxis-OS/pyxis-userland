#ifndef LIBCONFIG_HANDOFF_H
#define LIBCONFIG_HANDOFF_H

#include "selection.h"
#include <abi/handle.h>
#include <abi/syscall.h>

struct config_handoff {
  handle_t files[CONFIG_LUA_COUNT];
};

struct config_handoff_bytes {
  char *bytes;
  size_t size;
};

/* Initialize handles to HANDLE_INVALID (or {0}). The caller supplies a private,
 * unshared native RAM parent and keeps it private for the entire call. A RAM
 * type check cannot prove namespace exclusivity. Each fixed-name file is
 * unlinked before writing; successful output owns only native READ grants.
 * The parent and selection remain borrowed; no path or ESP authority escapes.
 * Admission requires all three frozen sources to be bounded literal tables.
 * Failure closes every created grant; no automatic retry or named-file repair.
 */
bool config_handoff_create(handle_t private_parent, const struct config_selection *selection,
    struct config_handoff *handoff, enum call_status *status);
bool config_handoff_close(struct config_handoff *handoff);

/* Borrow a trusted handoff FILE with exactly native READ authority. Read and
 * recompare its bytes and held identity/size without resolving a path. Stable
 * reads alone do not prove the producer removed every writer; the trusted
 * constructor's private namespace and writer retirement establish that.
 * Initialize snapshot with {0}; read replaces its owned, NUL-terminated bytes.
 */
bool config_handoff_read(handle_t file, struct config_handoff_bytes *snapshot,
    enum call_status *status);
void config_handoff_bytes_free(struct config_handoff_bytes *snapshot);

#endif
