#ifndef USERSPACE_PATH_H
#define USERSPACE_PATH_H

#include <directory.h>

struct path_root {
  const char *name;
  handle_t handle;
};

/* Owns count handles in caller-owned storage, boundary first and current last.
 * Each handle retains its own rights; changing directory requests the parent
 * grant for each child. Do not close entries separately or copy this owning struct.
 * The startup display path remains an initial description, not runtime authority. */
struct path_context {
  handle_t *directories;
  size_t count;
  size_t capacity;
  /* Optional caller-owned complete binding set, with borrowed names/handles.
   * NULL selects immutable startup roots. Keep it alive through path calls;
   * changes affect explicit URI resolution, not the retained cwd chain. */
  const struct path_root *roots;
  size_t root_count;
  /* Optional borrowed namespace override with LOOKUP. Zero selects
   * startup_namespace(). Shells can update this after creating a namespace. */
  handle_t namespace;
};

/* Scratch storage must be disjoint from the context, inputs and outputs.
 * directories holds temporary owned handles; component holds one name plus NUL
 * (also used for the scheme). No handles remain here after any public call. */
struct path_workspace {
  handle_t *directories;
  size_t directory_capacity;
  char *component;
  size_t component_capacity;
};

/* Initialize a fresh/closed context by copying a borrowed directory chain.
 * Use startup_working_directories()/count() for the initial chain, or one
 * directory for a standalone subtree. Inputs must be directory capabilities.
 * Context, storage and input chain must be disjoint. Set optional root bindings
 * after initialization; closing the context does not close their handles.
 * Failure leaves an empty context and preserves every input handle. */
enum call_status path_context_init(struct path_context *context,
    handle_t *storage, size_t capacity, const handle_t *directories, size_t count);
void path_context_close(struct path_context *context);

/* Exact scheme:// prefixes select context bindings (or startup roots).
 * A name bound as both root and namespace service is ambiguous and fails with
 * BAD_REQUEST, including if its provider has closed. Namespace-only names
 * return UNAVAILABLE while their provider is live, or ENDPOINT_CLOSED once
 * it closes; no file bridge exists yet.
 * other paths use cwd.
 * No leading / or empty paths. Repeated / and . are accepted; .. walks the
 * retained chain, failing at its boundary. A trailing / requires a directory.
 * Names are case-sensitive literal bytes, with no URL decoding or expansion.
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

/* Resolve the parent and remove its final component in one directory operation.
 * kind is FILE, DIRECTORY or ANY. A trailing / requires DIRECTORY; a FILE
 * request with trailing / fails. Roots and final . or .. cannot be removed.
 * Intermediate components retain normal ordered resolution/boundary checks.
 * Requires REMOVE on the parent, never READ/WRITE on the child. No allocation;
 * scratch storage is caller-owned and all temporary handles are closed. */
enum call_status path_remove(const struct path_context *context, const char *path,
    uint64_t kind, struct path_workspace *workspace);

/* Resolve two parents, then rename files atomically with the native policy.
 * Both workspaces must be disjoint and stay alive through the call. Roots,
 * final . or .. and trailing / are rejected; the destination is an exact file
 * path, not a directory to append the source basename to. Context is unchanged,
 * all temporary handles are closed, and no heap allocation occurs here. */
enum call_status path_rename(const struct path_context *context, const char *source,
    const char *destination, uint64_t policy, struct path_workspace *source_workspace,
    struct path_workspace *destination_workspace);

/* Prepare a complete owned chain, then replace context. All failures preserve
 * the old working directory. The final directory must allow LOOKUP. Requires room for the whole chain in both context
 * and scratch storage. No process-global cwd, allocation or kernel path parser. */
enum call_status path_change(struct path_context *context, const char *path,
                              struct path_workspace *workspace);

#endif
