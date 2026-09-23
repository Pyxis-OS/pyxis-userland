#ifndef USERSPACE_LAUNCHER_H
#define USERSPACE_LAUNCHER_H

#include <abi/launcher.h>
#include <abi/syscall.h>

/* Explicit pointers/counts in request refer to caller storage, borrowed until
 * return. Native statuses; clears *child on failure. Success returns an owned
 * process-control handle with WAIT. Closing it does not terminate the child.
 * Source handles survive success and failure; no target-space/CPU parameter. */
enum call_status launcher_launch(handle_t launcher, const struct launch_request *request,
                                  handle_t *child);

/* Launch native PXE or a single-level shebang script from request->image.
 * Caller-owned arrays/strings must be valid C storage. Uses heap scratch space.
 * Interpreters resolve through the caller's startup roots, not child bindings.
 * Script launches require argv[0] as a diagnostic script name and no existing
 * resource named "script". Append a READ grant for the original image and use
 * interpreter URI, original argv[0], then original arguments as the child argv.
 * No other authority is added. Native requests pass through unchanged.
 * On failure no child is returned; request and source grants are preserved.
 * Reads do not snapshot mutable files; the interpreter receives the same file. */
enum call_status program_launch(handle_t launcher, const struct launch_request *request,
                                 handle_t *child);

#endif
