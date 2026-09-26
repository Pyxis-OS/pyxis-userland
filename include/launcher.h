#ifndef USERSPACE_LAUNCHER_H
#define USERSPACE_LAUNCHER_H

#include <stddef.h>
#include <abi/launcher.h>
#include <abi/syscall.h>

/* Explicit pointers/counts in request refer to caller storage, borrowed until
 * return. Native statuses; clears *child on failure. Success returns an owned
 * process-control handle with WAIT. Closing it does not terminate the child.
 * Source handles survive success and failure; no target-space/CPU parameter. */
enum call_status launcher_launch(handle_t launcher, const struct launch_request *request,
                                  handle_t *child);

/* Requests and all source handles are borrowed until return. Valid count is
 * 1..LAUNCH_BATCH_MAX. Both outputs are required and must be separate from
 * requests and each other. For a valid count, children are cleared on failure.
 * Success returns one owned WAIT handle per request. A stage failure names its
 * index; a request-level failure leaves failed_index at LAUNCH_NO_STAGE. An
 * untrustworthy response returns CALL_OUTCOME_UNKNOWN and must not be retried
 * automatically, because the children may have been published. */
enum call_status launcher_launch_batch(handle_t launcher, const struct launch_request *requests,
    size_t count, handle_t *children, uint64_t *failed_index);

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

/* Resolve a single shebang level for every stage before submitting the batch.
 * Native requests pass through unchanged. A preparation failure names its
 * stage and launches no children. Temporary interpreter handles and arrays are
 * released on every path; caller requests and source grants are preserved. */
enum call_status program_launch_batch(handle_t launcher, const struct launch_request *requests,
    size_t count, handle_t *children, uint64_t *failed_index);

#endif
