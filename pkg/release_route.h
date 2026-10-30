#ifndef USERSPACE_PKG_RELEASE_ROUTE_H
#define USERSPACE_PKG_RELEASE_ROUTE_H

#include "metadata.h"

/* Index order is release publication order. A route stages one hop; reboot
 * verifies that hop before another client resolves the remaining route. A dev
 * build has no RUNNING release and sits immediately after UPGRADE_FROM. */
bool pkg_release_route(const struct pkg_entry *entries, size_t count,
    const char *running, const char *upgrade_from, const char *requested,
    const struct pkg_entry **next, const struct pkg_entry **target, const char **reason);

#endif
