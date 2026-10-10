#ifndef USERSPACE_PROGRAM_H
#define USERSPACE_PROGRAM_H

#include <bundle.h>

/* Bare names: bin .pxe, same-name .pxb default, explicit development catalog,
 * then boot .pxe. Only an absent candidate permits fallback. The catalog comes
 * from PYXIS_BUNDLE_CATALOG and is not consulted on a direct hit. Explicit paths
 * retain their meaning; logical bin://NAME aliases omit rescue fallback.
 * WORKSPACE must accommodate COMMAND plus sizeof("boot://.pxe") bytes, and that
 * many directory slots plus CONTEXT's depth. Supply HTTP scratch for HTTP URIs.
 * On success a plain IMAGE is owned by the caller; a BUNDLE owns its borrowed
 * IMAGE and must be closed instead. Failure clears both outputs. */
enum call_status program_open(const struct path_context *context, const char *command,
    struct path_workspace *workspace, handle_t *image, struct bundle_program **bundle);

#endif
