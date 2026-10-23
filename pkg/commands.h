#ifndef PKG_COMMANDS_H
#define PKG_COMMANDS_H

#include "store.h"

/* Read only declared exports and validate each target from its held tree.
 * Successful output owns the sorted command records followed by the sorted
 * root records, one per package with files; no executable discovery occurs. */
bool pkg_commands_build(handle_t packages, const struct pkg_selection *selection,
    struct pkg_projection *projection, const char **reason);

#endif
