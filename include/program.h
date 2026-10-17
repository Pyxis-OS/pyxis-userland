#ifndef USERSPACE_PROGRAM_H
#define USERSPACE_PROGRAM_H

#include <bundle.h>

/* The longest candidate prefix and suffix program_open adds to a bare name. */
#define PROGRAM_LOOKUP_EXTRA sizeof("home://bin/.pxe")

/* Bare names: bin .pxe, same-name .pxb default, explicit development catalog,
 * boot .pxe, then the user's home://bin .pxe and .pxb. Only an absent
 * candidate permits fallback. The catalog comes from PYXIS_BUNDLE_CATALOG and
 * is not consulted on a direct hit. Explicit paths retain their meaning;
 * logical bin://NAME aliases omit the boot and home candidates.
 * WORKSPACE must accommodate COMMAND plus PROGRAM_LOOKUP_EXTRA bytes, and that
 * many directory slots plus CONTEXT's depth. Supply HTTP scratch for HTTP URIs.
 * On success a plain IMAGE is owned by the caller; a BUNDLE owns its borrowed
 * IMAGE and must be closed instead. Failure clears both outputs. */
enum call_status program_open(const struct path_context *context, const char *command,
    struct path_workspace *workspace, handle_t *image, struct bundle_program **bundle);

#endif
