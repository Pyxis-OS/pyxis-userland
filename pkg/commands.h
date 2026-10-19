#ifndef PKG_COMMANDS_H
#define PKG_COMMANDS_H

#include "store.h"

/* Read only declared exports and validate each target from its held tree.
 * Successful output owns a sorted array; no executable discovery occurs. */
bool pkg_commands_build(handle_t packages, const struct pkg_selection *selection,
    struct package_command_record **records, size_t *count, const char **reason);

#endif
