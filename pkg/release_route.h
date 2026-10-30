#ifndef USERSPACE_PKG_RELEASE_ROUTE_H
#define USERSPACE_PKG_RELEASE_ROUTE_H

#include "metadata.h"

/* Index order is release publication order. A route stages one hop; reboot
 * verifies that hop before another client resolves the remaining route. */
bool pkg_release_route(const struct pkg_entry *entries, size_t count,
    const char *running, const char *requested, const struct pkg_entry **next,
    const struct pkg_entry **target, const char **reason);

#endif
