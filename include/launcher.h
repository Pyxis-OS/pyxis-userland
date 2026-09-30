#ifndef USERSPACE_LAUNCHER_H
#define USERSPACE_LAUNCHER_H

#include <stddef.h>
#include <abi/execution_group.h>
#include <abi/launcher.h>
#include <abi/syscall.h>

struct path_context;

/* CREATE_GROUP authority on an unbound launcher returns two owned, distinct
 * handles: a supervision grant with CONTROL|WAIT and a group-bound launcher
 * with LAUNCH only. Closing the final CONTROL grant requests termination.
 * Preserve *reply on failure. An untrustworthy response returns CALL_OUTCOME_UNKNOWN;
 * do not retry automatically, because the group may have been created. */
enum call_status launcher_create_group(handle_t launcher,
    struct execution_group_create_reply *reply);

/* Seal group admission with CONTROL authority. Sealing is idempotent; existing
 * members continue running. An untrustworthy response returns
 * CALL_OUTCOME_UNKNOWN because admission may already have been sealed. */
enum call_status execution_group_seal(handle_t supervision);

/* Request termination with CONTROL authority. Seals admission and stops existing
 * members at safe kernel boundaries. Idempotent success acknowledges the request;
 * it does not report completion. An untrustworthy response returns
 * CALL_OUTCOME_UNKNOWN because termination may already have been requested. */
enum call_status execution_group_terminate(handle_t supervision);

/* Observe completion with WAIT authority. Blocks until sealed admission, member
 * reclamation and group-owned cleanup finish. Completion is repeatable, returns
 * no payload and does not describe aggregate program success. Does not consume
 * the handle or add CONTROL authority. WAIT-only observers do not supervise.
 * An untrustworthy response returns CALL_OUTCOME_UNKNOWN. Open empty groups
 * are not complete. */
enum call_status execution_group_wait(handle_t observer);

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
 * Interpreters resolve through the caller's startup roots, or explicit
 * interpreter_context roots when supplied, never child bindings. The context
 * may also select the current namespace; NULL selects the startup snapshot.
 * It is borrowed until return.
 * Script launches require argv[0] as a diagnostic script name and no existing
 * resource named "script". Append a READ grant for the original image and use
 * interpreter URI, original argv[0], then original arguments as the child argv.
 * No other authority is added. Native requests pass through unchanged.
 * On failure no child is returned; request and source grants are preserved.
 * Reads do not snapshot mutable files; the interpreter receives the same file. */
enum call_status program_launch(handle_t launcher, const struct launch_request *request,
    const struct path_context *interpreter_context, handle_t *child);

/* Resolve a single shebang level for every stage before submitting the batch.
 * Native requests pass through unchanged. A preparation failure names its
 * stage and launches no children. Temporary interpreter handles and arrays are
 * released on every path; caller requests and source grants are preserved. */
enum call_status program_launch_batch(handle_t launcher, const struct launch_request *requests,
    size_t count, const struct path_context *interpreter_context,
    handle_t *children, uint64_t *failed_index);

#endif
