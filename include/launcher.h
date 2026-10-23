#ifndef USERSPACE_LAUNCHER_H
#define USERSPACE_LAUNCHER_H

#include <stddef.h>
#include <abi/execution_group.h>
#include <abi/launcher.h>
#include <abi/syscall.h>

struct job_work;
struct program_image;
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
 * No other authority is added, except extras offered through program_launch_extras
 * to one named interpreter. Native requests pass through unchanged.
 * On failure no child is returned; request and source grants are preserved.
 * Reads do not snapshot mutable files; the interpreter receives the same file. */
enum call_status program_launch(handle_t launcher, const struct launch_request *request,
    const struct path_context *interpreter_context, handle_t *child);

/* Grants a caller offers only to one exact parsed interpreter. ROOT selection
 * opens PATH beneath the held ROOT when grants are offered, preserving the
 * trusted interpreter chain. IMAGE selection uses only a READ-restricted copy
 * of the held native file IMAGE, even when no grants are offered. An absent or
 * unusable IMAGE refuses launch; no scheme lookup substitutes for it. IMAGE
 * selects code, not trust, and never creates a trusted root. Each offered grant
 * is appended after the script grant with its name, flags and masks. Native
 * images and scripts naming any other interpreter receive none and resolve
 * normally. A request that already has an entry of one of these names is
 * rejected with CALL_ALREADY_EXISTS. Everything is borrowed until return. */
struct program_script_grant {
  const char *name;
  handle_t source;
  uint64_t rights, transport;
  uint64_t flags; /* Entry flags: ENVIRONMENT_EXPORTED, or 0 for a private entry. */
};

enum program_interpreter_selection {
  PROGRAM_INTERPRETER_ROOT,
  PROGRAM_INTERPRETER_IMAGE,
};

struct program_script_extras {
  /* Optional borrowed preparation, kept live through admission. Its held FILE
   * replaces request->image for native admission and the original script grant;
   * adaptation uses its parsed interpreter without reading the prefix again. */
  const struct program_image *prepared_image;
  enum program_interpreter_selection selection;
  const char *interpreter;
  handle_t root;
  const char *path;
  handle_t image;
  const struct program_script_grant *grants;
  size_t count;
  /* Explicit delegation for this exact held interpreter only. Preparation
   * creates a private child subtree, never forwards the ancestor. Optional
   * when absent/unavailable; successful or unknown admission retains the
   * subtree for enclosing-owner cleanup or reboot, not PROCESS retirement. */
  handle_t job_provisioner, job_clock;
  /* Optional owned result, initialized to NULL. Successful/unknown admission
   * transfers the delegation to the caller, which may remove it only after
   * confirmed retirement of the enclosing scope, not merely the interpreter. */
  struct job_work **job_delegation;
};

/* As program_launch, with optional extras (NULL for none). */
enum call_status program_launch_extras(handle_t launcher, const struct launch_request *request,
    const struct path_context *interpreter_context, const struct program_script_extras *extras,
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

/* As program_launch_batch; extras is NULL or an array with one entry per request. */
enum call_status program_launch_batch_extras(handle_t launcher,
    const struct launch_request *requests, size_t count,
    const struct path_context *interpreter_context, const struct program_script_extras *extras,
    handle_t *children, uint64_t *failed_index);

#endif
