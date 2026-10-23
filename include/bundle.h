#ifndef USERSPACE_BUNDLE_H
#define USERSPACE_BUNDLE_H

#include <path.h>
#include <environment.h>

struct bundle_program;

/* The root a manifest's "packages" list grants (docs/wip/sdk-view.md,
 * decision 48). A bundle never inherits its launcher's root of this name. */
#define BUNDLE_PACKAGES_ROOT "pkg"

struct bundle_grant_request {
  const char *name;
  const char *resource;
  /* Selected external inventory key and optional directory URI-root alias.
   * A provider source names a held exported provider root in the supplied
   * launch environment, rather than the private source inventory. */
  const char *source;
  /* Only the grant named BUNDLE_PACKAGES_ROOT, which the manifest's
   * "packages" list creates: a read-only pkg:// holding just these package
   * IDs, selected from the launcher's packages source. */
  const char *const *packages;
  size_t package_count;
  const char *root;
  /* Optional canonical lo/ethN/wlanN role for TCP/UDP/echo. Preparation resolves
   * it using supplied registry READ, then attenuates the held native factory.
   * The child receives no registry unless it separately declares that grant. */
  const char *interface;
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
