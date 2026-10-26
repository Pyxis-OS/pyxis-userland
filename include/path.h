#ifndef USERSPACE_PATH_H
#define USERSPACE_PATH_H

#include <directory.h>

/* Owns count handles in caller-owned storage, boundary first and current last.
 * Each handle retains its own rights; changing directory requests the parent
 * grant for each child. Do not close entries separately or copy this owning struct.
 * The startup display path remains an initial description, not runtime authority. */
struct path_context {
  handle_t *directories;
  size_t count;
  size_t capacity;

};

/* Scratch storage must be disjoint from the context, inputs and outputs.
 * directories holds temporary owned handles; component holds one name plus NUL
 * (also used for the scheme). No handles remain here after any public call. */

struct path_workspace {
  handle_t *directories;
  size_t directory_capacity;
  char *component;
  size_t component_capacity;
  /* Optional copied metadata, borrowed READ clock and earlier deadline. */
  handle_t clock;
  uint64_t deadline_ns;
};

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize a fresh/closed context by copying a borrowed directory chain.
 * Use startup_working_directories()/count() for the initial chain, or one
 * directory for a standalone subtree. Inputs must be directory capabilities.
 * Context, storage and input chain must be disjoint. Exact names resolve
 * through the caller-bound environment; no alternate naming map is attached.
 * Failure leaves an empty context and preserves every input handle. */
enum call_status path_context_init(struct path_context *context,
    handle_t *storage, size_t capacity, const handle_t *directories, size_t count);
void path_context_close(struct path_context *context);

/* The root NAME:// names: the environment's root of that name. Packages are
 * read through a pkg:// root, never as their own NAME://. Success returns an
 * owned handle; NOT_FOUND means there is none. */
enum call_status path_root(const char *name, handle_t *root);
/* Exact name:// prefixes capture the caller's current environment value.
 * Directory values start native traversal; other values open the opaque remainder.
 * TEXT values capture an immutable native READ FILE.
 * Captures own their authority through the operation, outside table guards. Directory
 * operations on non-directory values are unsupported. Other paths use cwd.
 * Native paths reject leading / and empty input. Repeated / and . are accepted;
 * .. walks the
 * retained chain, failing at its boundary. A trailing / requires a directory.
 * Names are case-sensitive literal bytes, with no URL decoding or expansion.
 * Provider URI tails retain every separator and dot component unchanged.
 * Generic OPEN carries workspace->deadline_ns as its copied and delivery
 * deadline; zero waits without a deadline.
 * Components are walked in order: missing/.. still fails at missing.
 *
 * Success returns a new owned handle of kind with exactly rights. context may
 * be NULL for an explicit scheme. Failure clears *handle, unwinds temporary
 * grants and leaves context unchanged. CALL_LIMIT includes insufficient scratch
 * storage; BAD_REQUEST covers syntax, NOT_FOUND unknown schemes, UNAVAILABLE a
 * missing working directory, DENIED a boundary escape. Other call errors pass
 * through. Buffer sizes are caller choices, not ABI path/depth limits. */
enum call_status path_resolve(const struct path_context *context, const char *path,
    uint64_t kind, uint64_t rights, struct path_workspace *workspace, handle_t *handle);

/* Same owned-handle lookup, but provider routes return BAD_OPERATION without
 * issuing OPEN. Native held FILE values are accepted; actual exported values are refused. */
enum call_status path_resolve_native(const struct path_context *context, const char *path,
    uint64_t kind, uint64_t rights, struct path_workspace *workspace, handle_t *handle);

/* Native DIRECTORY admission retaining its actual rights. Only required_rights
 * are required on the result; traversal still needs held LOOKUP authority.
 * Exported directories and provider routes are refused before invocation. */
enum call_status path_open_directory_native(const struct path_context *context, const char *path,
    uint64_t required_rights, struct path_workspace *workspace, handle_t *handle);

/* Native mutation companions. Each walk captures the named root once and
 * retains actual directory grants. Recursive creation follows original ordered
 * components, checks existing directories and may leave a completed prefix.
 * No provider invocation, mutation replay, rollback or implicit sync. */
enum call_status path_create_directory_native(const struct path_context *context, const char *path,
    bool recursive, struct path_workspace *workspace);
enum call_status path_remove_native(const struct path_context *context, const char *path,
    uint64_t kind, struct path_workspace *workspace);
enum call_status path_rename_native(const struct path_context *context, const char *source,
    const char *destination, uint64_t policy, struct path_workspace *source_workspace,
    struct path_workspace *destination_workspace);

/* Shared file open/create route for libc and shell redirects. Provider opens
 * never fall back to directory creation, including after provider NOT_FOUND.
 * Native creation occurs only after an absent file lookup, with CREATE on the
 * parent; an intervening creator is looked up once. No truncation or allocation.
 * Outputs and workspace ownership follow path_resolve(). */
enum call_status path_open_file(const struct path_context *context, const char *path,
    uint64_t rights, bool create, struct path_workspace *workspace, handle_t *handle);

/* Exclusive empty-file creation through the resolved parent's CREATE right.
 * An existing name returns ALREADY_EXISTS, never opens or truncates it.
 * Provider routes cannot create files; trailing / and final . or .. fail.
 * No allocation; owned output and scratch lifetime follow path_resolve(). */
enum call_status path_create_file(const struct path_context *context, const char *path,
    uint64_t rights, struct path_workspace *workspace, handle_t *handle);

/* Capture an exact environment name once, dispatching by its attested protocol.
 * PROVIDER receives the opaque tail and workspace deadline unchanged, requiring
 * its independent REMOVE right. FILE and ANY allow provider removal; DIRECTORY
 * refuses it. Held FILE removal is unsupported. No relookup, fallback or retry.
 * A delivered provider mutation may report OUTCOME_UNKNOWN.
 * Native paths resolve the parent and remove its final component in one
 * directory operation. kind is FILE, DIRECTORY or ANY. A trailing / requires
 * DIRECTORY; a FILE request with trailing / fails. Roots and final . or .. cannot be removed.
 * Intermediate components retain normal ordered resolution/boundary checks.
 * Requires REMOVE on the parent, never READ/WRITE on the child. No allocation;
 * scratch storage is caller-owned and all temporary handles are closed. */
enum call_status path_remove(const struct path_context *context, const char *path,
    uint64_t kind, struct path_workspace *workspace);

/* Resolve the parent and create its final component as a directory. A trailing /
 * is accepted. An existing name of either kind, including a root or a final .
 * or .. that resolves, returns ALREADY_EXISTS; missing parents are not created. Requires
 * CREATE on the parent. No allocation; scratch storage is caller-owned and all
 * temporary handles, including the new directory's, are closed. */
enum call_status path_create_directory(const struct path_context *context, const char *path,
    struct path_workspace *workspace);

/* Resolve two parents, then rename files/directories atomically with native policy.
 * Both workspaces must be disjoint and stay alive through the call. Roots,
 * final . or .. are rejected; trailing / requires a directory at the atomic
 * edit. The destination is an exact path, not a directory to append the source
 * basename to. Context and its held cwd chain are unchanged, all temporary
 * handles are closed, and no heap allocation occurs here. */
enum call_status path_rename(const struct path_context *context, const char *source,
    const char *destination, uint64_t policy, struct path_workspace *source_workspace,
    struct path_workspace *destination_workspace);

/* Resolve two parents and exclusively link a regular file to the exact target
 * path. Source needs LOOKUP, READ_FILES and WRITE_FILES; destination needs CREATE
 * plus LOOKUP for traversal. Same-volume npfs only, with no provider OPEN or
 * copy fallback. Roots, final . or .. and trailing / are rejected. Workspaces
 * must be disjoint; all temporary handles are closed and cwd is unchanged.
 * No allocation, retry or implicit sync. OUTCOME_UNKNOWN must not be replayed. */
enum call_status path_link(const struct path_context *context, const char *source,
    const char *destination, struct path_workspace *source_workspace,
    struct path_workspace *destination_workspace);

/* Prepare a complete owned chain, then replace context. All failures preserve
 * the old working directory. The final directory must allow LOOKUP. Requires room for the whole chain in both context
 * and scratch storage. No process-global cwd, allocation or kernel path parser. */
enum call_status path_change(struct path_context *context, const char *path,
                              struct path_workspace *workspace);

#ifdef __cplusplus
}
#endif

#endif
