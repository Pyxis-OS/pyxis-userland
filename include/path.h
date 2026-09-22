#ifndef USERSPACE_PATH_H
#define USERSPACE_PATH_H

#include <directory.h>

/* Owns count handles in caller-owned storage, boundary first and current last.
 * directory_rights includes LOOKUP and is requested for each directory retained
 * by init/change. Do not close entries separately or copy this owning struct.
 * The startup display path remains an initial description, not runtime authority. */
struct path_context {
  handle_t *directories;
  size_t count;
  size_t capacity;
  uint64_t directory_rights;
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
 * Context, storage and input chain must be disjoint.
 * Failure leaves an empty context and preserves every input handle. */
enum call_status path_context_init(struct path_context *context,
    handle_t *storage, size_t capacity, const handle_t *directories, size_t count,
    uint64_t rights);
void path_context_close(struct path_context *context);

/* Exact scheme:// prefixes select startup roots; otherwise use context.
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

/* Prepare a complete owned chain, then replace context. All failures preserve
 * the old working directory. Requires room for the whole chain in both context
 * and scratch storage. No process-global cwd, allocation or kernel path parser. */
enum call_status path_change(struct path_context *context, const char *path,
                              struct path_workspace *workspace);

#endif
