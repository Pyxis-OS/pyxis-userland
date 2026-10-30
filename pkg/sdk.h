#ifndef PKG_SDK_H
#define PKG_SDK_H

#include "store.h"

/* Index the view files of SELECTION's dev packages: everything under include/
 * and the files directly in lib/ and lib/pkgconfig/ (docs/wip/sdk-view.md,
 * decision 45). A path that two packages provide, or that is a file in one and
 * a directory in another, refuses the set and names both packages, as does an
 * index past the format's field widths. Success owns sdk->bytes. */
bool pkg_sdk_build(handle_t packages, const struct pkg_selection *selection,
    struct pkg_sdk *sdk, const char **reason);

#endif
