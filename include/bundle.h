#ifndef USERSPACE_BUNDLE_H
#define USERSPACE_BUNDLE_H

#include <path.h>
#include <startup.h>

struct bundle_program;

struct bundle_grant_request {
  const char *name;
  const char *resource;
  uint64_t protocol;
  uint64_t rights;
  bool required;
};

/* Explicit development catalog: {"format":1,"bundles":["root://name.pxb"]}.
 * Validate the entire registration before selecting a bare command name.
 * Duplicate IDs/commands and collisions with held bin://NAME.pxe reject it.
 * These functions only prepare native .pxb programs; policy supplies grants.
 * Failure clears *program and leaves the caller's context unchanged. */
enum call_status bundle_command_open(const struct path_context *context,
    const char *catalog_uri, const char *command, struct bundle_program **program);
enum call_status bundle_open(const struct path_context *context,
    const char *bundle_uri, struct bundle_program **program);

/* Owns the selected revision, image and read-only app/resource directories.
 * Accessors borrow handles, arrays and strings until close; never close them
 * separately. Published development trees must not be changed or deleted:
 * held native handles do not freeze writable aliases or provide a snapshot. */
void bundle_program_close(struct bundle_program *program);
handle_t bundle_program_image(const struct bundle_program *program);
uint64_t bundle_program_stack_bytes(const struct bundle_program *program);
const struct startup_binding *bundle_program_roots(const struct bundle_program *program,
    size_t *count);
const struct bundle_grant_request *bundle_program_grants(const struct bundle_program *program,
    size_t *count);

#endif
