#ifndef USERSPACE_BUNDLE_H
#define USERSPACE_BUNDLE_H

#include <path.h>
#include <environment.h>

struct bundle_program;

struct bundle_grant_request {
  const char *name;
  const char *resource;
  /* Selected external inventory key and optional directory URI-root alias.
   * A directory grant names either a source or a package ID; a package is
   * resolved from the launcher's packages source and is always read-only.
   * A provider source names a held exported provider root in the supplied
   * launch environment, rather than the private source inventory. */
  const char *source;
  const char *package;
  const char *root;
  uint64_t protocol;
  uint64_t rights;
  bool required;
};

/* NOT_FOUND means the bundle directory itself is absent. A missing manifest,
 * app, resource directory or entry after selection is BAD_REQUEST. */
enum call_status bundle_open(const struct path_context *context,
    const char *bundle_uri, struct bundle_program **program);

/* Open a declared command from an already selected native .pxb directory.
 * REVISION is borrowed and copied, never reopened by name. Non-NULL COMMAND must
 * name a manifest command; NULL selects the default. An incomplete bundle or
 * missing alias refuses. Full stack,
 * grants and app/resource policy is retained. CONTEXT may be NULL for publisher
 * validation; actual launch still supplies its held authority separately. */
enum call_status bundle_open_directory(const struct path_context *context,
    handle_t revision, const char *command, struct bundle_program **program);

/* Retain the fixed package selection alongside the full bundle policy. Other
 * package grants still need separately held authority from the same volume. */
enum call_status bundle_open_package_directory(const struct path_context *context,
    handle_t revision, const char *command, handle_t fixed_view,
    struct bundle_program **program);
/* Borrowed fixed view, or INVALID for an ordinary bundle selection. */
handle_t bundle_program_package_view(const struct bundle_program *program);

/* Owns the selected revision, image and read-only app/resource directories.
 * Accessors borrow handles, arrays and strings until close; never close them
 * separately. Published development trees must not be changed or deleted:
 * held native handles do not freeze writable aliases or provide a snapshot. */
void bundle_program_close(struct bundle_program *program);
handle_t bundle_program_image(const struct bundle_program *program);
uint64_t bundle_program_stack_bytes(const struct bundle_program *program);
const struct environment_value *bundle_program_entries(const struct bundle_program *program,
    size_t *count);
const struct bundle_grant_request *bundle_program_grants(const struct bundle_program *program,
    size_t *count);

#endif
