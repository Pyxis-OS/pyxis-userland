#ifndef USERSPACE_LAUNCHER_H
#define USERSPACE_LAUNCHER_H

#include <stddef.h>
#include <abi/execution_group.h>
#include <abi/launcher.h>
#include <abi/syscall.h>

struct program_image;
struct bundle_program;
struct command_selection;
struct path_context;
struct space_definition;

/* LAUNCH permits scoped creation through the selected launcher. An unbound
 * launcher requires an ungrouped caller and creates a local root; a bound
 * launcher creates a subgroup within its binding. Members may use their own
 * or descendant bindings. The creator stays outside the new group. The reply
 * atomically returns two owned, distinct handles: CONTROL|WAIT supervision and
 * a group/space-bound LAUNCH launcher, with no CPU binding. Closing the final
 * CONTROL grant requests subtree termination. Preserve *reply on failure.
 * An untrustworthy response returns CALL_OUTCOME_UNKNOWN; do not retry
 * automatically, because the group may have been created. */
enum call_status launcher_create_group(handle_t launcher,
    struct execution_group_create_reply *reply);

/* Seal subtree admission with CONTROL authority. Sealing is idempotent;
 * existing members and subgroups continue running. An untrustworthy response
 * returns CALL_OUTCOME_UNKNOWN because admission may already have been sealed. */
enum call_status execution_group_seal(handle_t supervision);

/* Request termination with CONTROL authority. Seals admission and stops existing
 * members and subgroups at safe kernel boundaries. Idempotent success acknowledges
 * the request; it does not report completion. An untrustworthy response returns
 * CALL_OUTCOME_UNKNOWN because termination may already have been requested. */
enum call_status execution_group_terminate(handle_t supervision);

/* Observe completion with WAIT authority. Blocks until sealed admission, member
 * reclamation, subgroup completion and attributed cleanup finish. Storage-only
 * handles may outlive completion. Completion is repeatable, returns no payload
 * and does not describe aggregate program success. Does not consume the handle
 * or add CONTROL authority. WAIT-only observers do not supervise.
 * An untrustworthy response returns CALL_OUTCOME_UNKNOWN. Open empty groups
 * are not complete. */
enum call_status execution_group_wait(handle_t observer);
/* Same completion boundary, with immutable subtree flags. Preserve *result on
 * failure; does not retain additional control or inspect individual members. */
enum call_status execution_group_wait_result(handle_t observer,
    struct execution_group_result *result);

/* Explicit pointers/counts in request refer to caller storage, borrowed until
 * return. Native statuses; clears *child on failure. Success returns an owned
 * process-control handle with WAIT|TERMINATE. Closing it does not terminate the
 * child. Source handles survive success and failure; no target-space/CPU parameter.
 * An untrustworthy reply returns CALL_OUTCOME_UNKNOWN: the child may have been
 * published, so never replay that launch automatically. */
enum call_status launcher_launch(handle_t launcher, const struct launch_request *request,
                                  handle_t *child);

/* Requests and all source handles are borrowed until return. Valid count is
 * 1..LAUNCH_BATCH_MAX. Both outputs are required and must be separate from
 * requests and each other. For a valid count, children are cleared on failure.
 * Success returns one owned WAIT|TERMINATE handle per request. A stage failure names its
 * index; a request-level failure leaves failed_index at LAUNCH_NO_STAGE. An
 * untrustworthy response returns CALL_OUTCOME_UNKNOWN and must not be retried
 * automatically, because the children may have been published. */
enum call_status launcher_launch_batch(handle_t launcher, const struct launch_request *requests,
    size_t count, handle_t *children, uint64_t *failed_index);

/* Retain a READ FILE and parse its bounded prefix once. Success owns *prepared;
 * failure clears it. The interpreter accessor borrows the parsed URI until close,
 * or returns NULL for a native image. Holding the FILE pins the object, not its
 * bytes: neither preparation nor later launch snapshots mutable contents. */
enum call_status program_image_prepare(handle_t image, struct program_image **prepared);
const char *program_image_interpreter(const struct program_image *prepared);
/* Select code for a prepared script by retaining READ on a native FILE. The
 * selection owns its reference until close or successful replacement; failure
 * preserves the previous selection. Launch uses only this held selection and
 * never substitutes a URI lookup. This adds no child authority. */
enum call_status program_image_select_interpreter(struct program_image *prepared, handle_t image);
/* As program_image_select_interpreter, for an interpreter resolved as a bundle:
 * the code is the bundle's entry FILE. Success transfers SELECTION to PREPARED,
 * which closes it with the preparation; failure leaves it with the caller. A
 * manifest entry or grant named "script" refuses with CALL_ALREADY_EXISTS,
 * because launch hands the script over under that name. The selection adds no
 * authority: the caller prepares the launch from program_image_interpreter_bundle
 * with its own held sources, exactly as for an explicit launch of that bundle,
 * and passes the request to program_launch_prepared. Selecting a native FILE
 * afterwards drops the bundle. */
enum call_status program_image_select_interpreter_bundle(struct program_image *prepared,
    struct command_selection *selection);
/* The selected interpreter's bundle, borrowed until close, or NULL when the
 * interpreter is a native FILE or none is selected. */
struct bundle_program *program_image_interpreter_bundle(const struct program_image *prepared);
void program_image_close(struct program_image *prepared);

/* Launch native PXE or a single-level shebang script from request->image.
 * Caller-owned arrays/strings must be valid C storage. Uses heap scratch space.
 * Interpreters resolve through the caller's startup roots, or explicit
 * interpreter_context roots when supplied, never child bindings. The context
 * may instead supply held directory bindings; NULL selects the startup environment snapshot.
 * It is borrowed until return.
 * Script launches require argv[0] as a diagnostic script name and no existing
 * resource named "script". Append a READ grant for the original image and use
 * interpreter URI, original argv[0], then original arguments as the child argv.
 * No other authority is added. Native requests pass through unchanged.
 * On failure no child is returned; request and source grants are preserved.
 * Reads do not snapshot mutable files; the interpreter receives the same file. */
enum call_status program_launch(handle_t launcher, const struct launch_request *request,
    const struct path_context *interpreter_context, handle_t *child);

/* As program_launch, using a previously prepared input when non-NULL. The
 * preparation and any selected interpreter are borrowed until return. Its held
 * FILE replaces request->image; its prefix is not parsed again. NULL prepares
 * request->image normally. */
enum call_status program_launch_prepared(handle_t launcher, const struct launch_request *request,
    const struct program_image *input, const struct path_context *interpreter_context,
    handle_t *child);

/* As program_launch, but the child becomes the first process of a new space
 * created through FACTORY; see space_create_started for the space's rules. */
enum call_status program_create_space(handle_t factory, const struct space_definition *space,
    const uint64_t *cpus, uint64_t cpu_count, const struct launch_request *request,
    const struct path_context *interpreter_context, handle_t *child);

/* Resolve a single shebang level for every stage before submitting the batch.
 * Native requests pass through unchanged. A preparation failure names its
 * stage and launches no children. Temporary interpreter handles and arrays are
 * released on every path; caller requests and source grants are preserved. */
enum call_status program_launch_batch(handle_t launcher, const struct launch_request *requests,
    size_t count, const struct path_context *interpreter_context,
    handle_t *children, uint64_t *failed_index);

/* As program_launch_batch, using inputs[i] when non-NULL. The optional array
 * has one entry per request; its preparations and selected interpreters are
 * borrowed until return. NULL entries prepare their request images normally. */
enum call_status program_launch_batch_prepared(handle_t launcher,
    const struct launch_request *requests, size_t count,
    const struct program_image *const *inputs, const struct path_context *interpreter_context,
    handle_t *children, uint64_t *failed_index);

#endif
